#!/usr/bin/env python3
"""reimu-h264: the screen as H.264 from the GPU's encoder, for the REIMU web KVM.

The BMC's video engine only makes JPEG (~40 frames/s at 1920x1080). While the OS
runs, this agent gives the KVM page 60 frames/s at a few Mbit/s: ffmpeg encodes
the screen on the video card (VAAPI), reimu-kvm on the BMC connects here and
forwards the stream to the browser, which decodes it in hardware (WebCodecs).
Keyboard and mouse still go through the BMC's USB HID.

The picture is taken in one of two ways:
  kms   as root (the reimu-host.sh service): the buffer the card is showing,
        straight to the encoder without a copy through the CPU (like Sunshine);
        any screen: login, desktop, text console. The mouse pointer is a
        hardware overlay and is not in it (the KVM page draws its own).
  x11   as the desktop user: the X desktop via x11grab, pointer included.

  ./reimu-h264.py                     serve on :8555 to the BMC 192.168.10.79
  ./reimu-h264.py --allow 10.0.0.5    the BMC's address (only it may connect)
  ./reimu-h264.py --fps 60 --bitrate 8   frames/s, Mbit/s (peak 2x)
  ./reimu-h264.py --grab x11          force a way (default: kms when root)
  ./reimu-h264.py --test              encode 5 s locally and print the rate

The encoder runs only while the BMC is connected; each connection starts with a
key frame. Every 5 s a UDP datagram "REIMU-H264 <port>" goes to the BMC's
reimu-kvm (port 8443): that is how the KVM page finds this host by itself.

Sound: what plays on the default output of the logged-in user's PipeWire/
PulseAudio (its monitor), as Opus 48 kHz stereo; off with --no-audio.

Stream to the BMC: one JSON line {"codec","w","h","fps","audio"}, then frames
as u32 length (big endian), u8 kind, payload: kind 0/1 = H.264 access unit
(Annex B; 1 = key frame), kind 2 = one Opus packet.
"""
import argparse, ctypes, glob, json, os, re, select, signal, socket, struct, subprocess, sys, threading, time

ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("--port", type=int, default=8555)
ap.add_argument("--allow", default="192.168.10.79", help="BMC address allowed to connect (comma separated)")
ap.add_argument("--fps", type=int, default=60)
ap.add_argument("--bitrate", type=float, default=8, help="Mbit/s")
ap.add_argument("--gop", type=float, default=1.0, help="seconds between key frames")
ap.add_argument("--no-audio", action="store_true")
ap.add_argument("--audio-bitrate", type=int, default=96, help="kbit/s")
ap.add_argument("--announce-port", type=int, default=8443, help="reimu-kvm's port on the BMC (UDP)")
ap.add_argument("--grab", choices=["auto", "kms", "x11"], default="auto")
ap.add_argument("--test", action="store_true")
args = ap.parse_args()


def log(*a):
    print(time.strftime("%F %T"), *a, flush=True)


def gpu():
    """The DRM card that drives a monitor and is not the ASPEED (the BMC's VGA)."""
    for card in sorted(glob.glob("/sys/class/drm/card[0-9]")):
        drv = os.path.basename(os.path.realpath(card + "/device/driver"))
        if drv == "ast":
            continue
        if any(open(st).read().strip() == "connected" for st in glob.glob(card + "-*/status")):
            return "/dev/dri/" + os.path.basename(card)
    return None


class _Plane(ctypes.Structure):
    _fields_ = [("count_formats", ctypes.c_uint32), ("formats", ctypes.c_void_p), ("plane_id", ctypes.c_uint32),
                ("crtc_id", ctypes.c_uint32), ("fb_id", ctypes.c_uint32), ("crtc_x", ctypes.c_uint32),
                ("crtc_y", ctypes.c_uint32), ("x", ctypes.c_uint32), ("y", ctypes.c_uint32),
                ("possible_crtcs", ctypes.c_uint32), ("gamma_size", ctypes.c_uint32)]


class _PlaneRes(ctypes.Structure):
    _fields_ = [("count_planes", ctypes.c_uint32), ("planes", ctypes.POINTER(ctypes.c_uint32))]


class _Props(ctypes.Structure):
    _fields_ = [("count_props", ctypes.c_uint32), ("props", ctypes.POINTER(ctypes.c_uint32)),
                ("prop_values", ctypes.POINTER(ctypes.c_uint64))]


class _Prop(ctypes.Structure):
    _fields_ = [("prop_id", ctypes.c_uint32), ("flags", ctypes.c_uint32), ("name", ctypes.c_char * 32)]


class _FB(ctypes.Structure):
    _fields_ = [("fb_id", ctypes.c_uint32), ("width", ctypes.c_uint32), ("height", ctypes.c_uint32)]


def kms_screen(card):
    """(plane id, width, height) of the primary plane on screen. kmsgrab by
    itself takes the first plane with a buffer, which can be the 256x256
    hardware cursor."""
    drm = ctypes.CDLL("libdrm.so.2")
    for f, res in (("drmModeGetPlaneResources", _PlaneRes), ("drmModeGetPlane", _Plane),
                   ("drmModeObjectGetProperties", _Props), ("drmModeGetProperty", _Prop), ("drmModeGetFB", _FB)):
        getattr(drm, f).restype = ctypes.POINTER(res)
    fd = os.open(card, os.O_RDWR)
    try:
        drm.drmSetClientCap(fd, ctypes.c_uint64(2), ctypes.c_uint64(1))  # DRM_CLIENT_CAP_UNIVERSAL_PLANES
        pr = drm.drmModeGetPlaneResources(fd)
        if not pr:
            raise RuntimeError(f"{card}: no planes")
        for i in range(pr.contents.count_planes):
            pl = drm.drmModeGetPlane(fd, pr.contents.planes[i])
            if not pl or not pl.contents.fb_id:
                continue
            props = drm.drmModeObjectGetProperties(fd, pl.contents.plane_id, ctypes.c_uint32(0xeeeeeeee))
            kind = None
            for k in range(props.contents.count_props if props else 0):
                pp = drm.drmModeGetProperty(fd, props.contents.props[k])
                if pp and pp.contents.name == b"type":
                    kind = props.contents.prop_values[k]
            if kind == 1:  # DRM_PLANE_TYPE_PRIMARY
                fb = drm.drmModeGetFB(fd, pl.contents.fb_id)
                if fb:
                    return pl.contents.plane_id, fb.contents.width, fb.contents.height
        raise RuntimeError(f"{card}: nothing on screen (blanked?)")
    finally:
        os.close(fd)


def find_x():
    """DISPLAY/XAUTHORITY of this user's X session (works from ssh too)."""
    d = os.environ.get("DISPLAY", "")
    if d.startswith(":") and subprocess.run(["xrandr", "-q"], capture_output=True).returncode == 0:
        return
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            if os.stat(f"/proc/{pid}").st_uid != os.getuid():
                continue
            env = dict(l.split("=", 1) for l in open(f"/proc/{pid}/environ", "rb").read().decode(errors="replace").split("\0") if "=" in l)
        except OSError:
            continue
        if env.get("DISPLAY", "").startswith(":"):
            os.environ["DISPLAY"] = env["DISPLAY"]
            if "XAUTHORITY" in env:
                os.environ["XAUTHORITY"] = env["XAUTHORITY"]
            if subprocess.run(["xrandr", "-q"], capture_output=True).returncode == 0:
                return
    sys.exit("no X session of this user found (log in to the desktop, X11)")


def x11_size():
    out = subprocess.run(["xrandr", "-q"], capture_output=True, text=True).stdout
    m = re.search(r"current (\d+) x (\d+)", out)
    return (int(m.group(1)), int(m.group(2))) if m else (1920, 1080)


GRAB = None  # "kms" or "x11", chosen in main()
CARD = None
PLANE = None


def size():
    global PLANE
    if GRAB != "kms":
        return x11_size()
    PLANE, w, h = kms_screen(CARD)
    return w, h


def ffmpeg_cmd(w, h, out):
    b = args.bitrate
    if GRAB == "kms":
        # the scanout buffer as a DRM PRIME frame, mapped into VAAPI on the same card
        src = ["-device", CARD, "-f", "kmsgrab", "-plane_id", str(PLANE), "-framerate", str(args.fps), "-i", "-",
               "-vf", f"hwmap=derive_device=vaapi,scale_vaapi=w={w}:h={h}:format=nv12"]
    else:
        src = ["-vaapi_device", "/dev/dri/renderD128",
               "-f", "x11grab", "-draw_mouse", "1", "-framerate", str(args.fps), "-video_size", f"{w}x{h}",
               "-fflags", "nobuffer", "-i", os.environ["DISPLAY"],
               "-vf", "hwupload,scale_vaapi=format=nv12"]
    return [
        "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", *src,
        "-c:v", "h264_vaapi", "-profile:v", "high", "-rc_mode", "VBR",
        "-b:v", f"{b}M", "-maxrate", f"{2 * b}M", "-g", str(max(1, int(args.fps * args.gop))), "-bf", "0",
        "-aud", "1", "-flush_packets", "1", "-f", "h264", out,
    ]


AUD = b"\x00\x00\x00\x01\x09"


def access_units(stream):
    """Split an Annex B stream (with access unit delimiters) into access units."""
    buf = b""
    while True:
        chunk = stream.read1(1 << 16) if hasattr(stream, "read1") else os.read(stream.fileno(), 1 << 16)
        if not chunk:
            if buf:
                yield buf
            return
        buf += chunk
        while True:
            i = buf.find(AUD, 1)
            if i < 0:
                break
            yield buf[:i]
            buf = buf[i:]


def is_key(au):
    # NAL types after start codes: 5 = IDR slice
    i = 0
    while True:
        i = au.find(b"\x00\x00\x01", i)
        if i < 0 or i + 3 >= len(au):
            return False
        if au[i + 3] & 0x1F == 5:
            return True
        i += 3


def pulse_user():
    """How to reach the sound server of the user on the screen (seat0): as root,
    run as that user with its XDG_RUNTIME_DIR (PipeWire may turn away other
    users); otherwise our own session. None: nobody's sound to take."""
    if os.geteuid() != 0:
        return {}
    try:
        sess = subprocess.run(["loginctl", "show-seat", "seat0", "-p", "ActiveSession", "--value"],
                              capture_output=True, text=True).stdout.strip()
        uid = subprocess.run(["loginctl", "show-session", sess, "-p", "User", "--value"],
                             capture_output=True, text=True).stdout.strip()
    except OSError:
        return None
    if not uid.isdigit() or not os.path.exists(f"/run/user/{uid}/pulse/native"):
        return None
    import pwd
    pw = pwd.getpwuid(int(uid))
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": pw.pw_dir, "XDG_RUNTIME_DIR": f"/run/user/{uid}"}
    return {"user": pw.pw_uid, "group": pw.pw_gid, "extra_groups": [], "env": env}


def ogg_packets(stream):
    """Packets of an Ogg stream (pages: 27-byte header, segment table, data)."""
    def read(n):
        b = b""
        while len(b) < n:
            c = stream.read(n - len(b))
            if not c:
                raise EOFError
            b += c
        return b
    pkt = b""
    try:
        while True:
            h = read(27)
            if h[:4] != b"OggS":
                return
            segs = read(h[26])
            data = read(sum(segs))
            off = 0
            for l in segs:
                pkt += data[off:off + l]
                off += l
                if l < 255:
                    yield pkt
                    pkt = b""
    except EOFError:
        return


def audio_cmd():
    # fatal only: stopping it mid-stream makes ffmpeg complain about the pipe
    return ["ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "fatal",
            "-f", "pulse", "-fragment_size", "3840", "-i", "@DEFAULT_MONITOR@",
            "-ac", "2", "-ar", "48000", "-c:a", "libopus", "-b:a", f"{args.audio_bitrate}k",
            "-application", "lowdelay", "-frame_duration", "20",
            "-f", "ogg", "-page_duration", "20000", "-flush_packets", "1", "-"]


def send_audio(conn, lock, p, stop):
    n = 0
    for pkt in ogg_packets(p.stdout):
        n += 1
        if n <= 2 or stop.is_set():  # OpusHead, OpusTags
            continue
        try:
            with lock:
                conn.sendall(struct.pack(">IB", len(pkt), 2) + pkt)
        except OSError:
            return


def serve(conn, peer):
    try:
        w, h = size()
    except (RuntimeError, OSError) as e:
        log(f"{peer}: {e}")
        conn.close()
        return
    p = subprocess.Popen(ffmpeg_cmd(w, h, "-"), stdout=subprocess.PIPE)
    who = None if args.no_audio else pulse_user()
    a = subprocess.Popen(audio_cmd(), stdout=subprocess.PIPE, cwd="/", **who) if who is not None else None
    hdr = {"codec": "avc1.640028", "w": w, "h": h, "fps": args.fps, "audio": "opus" if a else ""}
    log(f"{peer}: streaming {w}x{h}@{args.fps} H.264 {args.bitrate} Mbit/s ({GRAB}){', Opus sound' if a else ''}")
    n = 0
    lock, stop = threading.Lock(), threading.Event()
    try:
        conn.sendall((json.dumps(hdr) + "\n").encode())
        if a:
            threading.Thread(target=send_audio, args=(conn, lock, a, stop), daemon=True).start()
        for au in access_units(p.stdout):
            with lock:
                conn.sendall(struct.pack(">IB", len(au), 1 if is_key(au) else 0) + au)
            n += 1
            # a size change of the desktop: start over with the new size
            # (kmsgrab stops by itself when the buffer on screen changes)
            if GRAB == "x11" and n % (args.fps * 2) == 0 and x11_size() != (w, h):
                log("desktop size changed")
                break
    except OSError as e:
        log(f"{peer}: {e}")
    finally:
        stop.set()
        for proc in (p, a):
            if not proc:
                continue
            proc.terminate()
            try:
                proc.wait(2)
            except subprocess.TimeoutExpired:
                proc.kill()
        conn.close()
        log(f"{peer}: closed after {n} frames")


def announce(bmcs):
    """Tell the BMCs where this agent is (they may not know the host's address)."""
    msg = f"REIMU-H264 {args.port}".encode()
    while True:
        for b in bmcs:
            try:
                for fam, _, _, _, sa in socket.getaddrinfo(b, args.announce_port, type=socket.SOCK_DGRAM):
                    with socket.socket(fam, socket.SOCK_DGRAM) as u:
                        u.sendto(msg, sa)
            except OSError:
                pass  # no route yet (boot): next round
        time.sleep(5)


def main():
    global GRAB, CARD
    GRAB = args.grab
    if GRAB == "auto":
        GRAB = "kms" if os.geteuid() == 0 else "x11"
    if GRAB == "kms":
        CARD = gpu()
        if not CARD:
            sys.exit("no video card with a monitor (besides the ASPEED)")
    else:
        find_x()
    if args.test:
        w, h = size()
        t = time.time()
        p = subprocess.Popen(ffmpeg_cmd(w, h, "-"), stdout=subprocess.PIPE)
        n = size = keys = 0
        for au in access_units(p.stdout):
            n += 1
            size += len(au)
            keys += is_key(au)
            if time.time() - t > 5:
                break
        p.terminate()
        dt = time.time() - t
        print(f"{w}x{h}: {n / dt:.1f} frames/s, {size * 8 / dt / 1e6:.2f} Mbit/s, {keys} key frames")
        return
    allow = {a.strip() for a in args.allow.split(",") if a.strip()}
    s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    s.bind(("::", args.port))
    s.listen(4)
    what = CARD if GRAB == "kms" else "display " + os.environ["DISPLAY"]
    log(f"reimu-h264 on :{args.port}, {GRAB} grab of {what}, allowed: {', '.join(sorted(allow))}")
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
    threading.Thread(target=announce, args=(sorted(allow),), daemon=True).start()
    while True:
        conn, addr = s.accept()
        peer = addr[0].removeprefix("::ffff:")
        if peer not in allow:
            log(f"{peer}: refused (not in --allow)")
            conn.close()
            continue
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=serve, args=(conn, peer), daemon=True).start()


if __name__ == "__main__":
    main()
