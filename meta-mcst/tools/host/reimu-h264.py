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
key frame.

Stream to the BMC: one JSON line {"codec","w","h","fps"}, then frames as
u32 length (big endian), u8 key frame, H.264 access unit (Annex B).
"""
import argparse, glob, json, os, re, select, signal, socket, struct, subprocess, sys, threading, time

ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("--port", type=int, default=8555)
ap.add_argument("--allow", default="192.168.10.79", help="BMC address allowed to connect (comma separated)")
ap.add_argument("--fps", type=int, default=60)
ap.add_argument("--bitrate", type=float, default=8, help="Mbit/s")
ap.add_argument("--gop", type=float, default=1.0, help="seconds between key frames")
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
        if any(open(c + "/status").read().strip() == "connected" for c in glob.glob(card + "-*/status")):
            return "/dev/dri/" + os.path.basename(card)
    return None


def kms_size(card):
    """Size of the buffer on screen: ffmpeg's kmsgrab tells it when opening."""
    out = subprocess.run(["ffmpeg", "-hide_banner", "-nostdin", "-device", card, "-f", "kmsgrab", "-i", "-",
                          "-frames:v", "1", "-f", "null", "-"], capture_output=True, text=True, timeout=10).stderr
    m = re.search(r"Video: .*?(\d{3,5})x(\d{3,5})", out)
    if not m:
        raise RuntimeError("kmsgrab: " + (out.strip().splitlines() or ["no output"])[-1])
    return int(m.group(1)), int(m.group(2))


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


def size():
    return kms_size(CARD) if GRAB == "kms" else x11_size()


def ffmpeg_cmd(w, h, out):
    b = args.bitrate
    if GRAB == "kms":
        # the scanout buffer as a DRM PRIME frame, mapped into VAAPI on the same card
        src = ["-device", CARD, "-f", "kmsgrab", "-framerate", str(args.fps), "-i", "-",
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


def serve(conn, peer):
    try:
        w, h = size()
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        log(f"{peer}: {e}")
        conn.close()
        return
    p = subprocess.Popen(ffmpeg_cmd(w, h, "-"), stdout=subprocess.PIPE)
    hdr = {"codec": "avc1.640028", "w": w, "h": h, "fps": args.fps}
    log(f"{peer}: streaming {w}x{h}@{args.fps} H.264 {args.bitrate} Mbit/s ({GRAB})")
    n = 0
    try:
        conn.sendall((json.dumps(hdr) + "\n").encode())
        for au in access_units(p.stdout):
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
        p.terminate()
        try:
            p.wait(2)
        except subprocess.TimeoutExpired:
            p.kill()
        conn.close()
        log(f"{peer}: closed after {n} frames")


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
