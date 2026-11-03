#!/usr/bin/env python3
"""Unit tests of reimu-h264 (the host's screen as H.264/H.265 for the KVM page).

    python3 tools/host/tests/test_reimu_h264.py [-v]

No ffmpeg, no video card: the encoder is a fake process that writes a canned
Annex B stream, the BMC a socket pair.
"""
import importlib.machinery, importlib.util, io, json, os, socket, struct, sys, threading, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "reimu-h264.py")


def load():
    argv, sys.argv = sys.argv, [SRC, "--no-audio", "--grab", "x11"]
    try:
        loader = importlib.machinery.SourceFileLoader("reimu_h264", SRC)
        spec = importlib.util.spec_from_loader("reimu_h264", loader)
        m = importlib.util.module_from_spec(spec)
        loader.exec_module(m)
        return m
    finally:
        sys.argv = argv


rh = load()

H264_AUD = b"\x00\x00\x00\x01\x09\xf0"
HEVC_AUD = b"\x00\x00\x00\x01\x46\x01\x50"
H264_IDR = H264_AUD + b"\x00\x00\x00\x01\x67sps\x00\x00\x00\x01\x68pps\x00\x00\x00\x01\x65" + b"\x11" * 20
H264_P = H264_AUD + b"\x00\x00\x00\x01\x41" + b"\x22" * 20
# H.265: VPS 32, SPS 33, PPS 34 (type << 1), IDR_W_RADL 19, TRAIL_R 1, CRA 21
HEVC_IDR = HEVC_AUD + b"\x00\x00\x00\x01\x40\x01vps\x00\x00\x00\x01\x42\x01sps\x00\x00\x00\x01\x44\x01pps\x00\x00\x00\x01\x26\x01" + b"\x33" * 20
HEVC_P = HEVC_AUD + b"\x00\x00\x00\x01\x02\x01" + b"\x44" * 20
HEVC_CRA = HEVC_AUD + b"\x00\x00\x00\x01\x2a\x01" + b"\x55" * 20


class FakeProc:
    """what serve() needs of a Popen'd ffmpeg"""

    def __init__(self, data):
        self.stdout = io.BufferedReader(io.BytesIO(data))
        self.returncode = None

    def terminate(self):
        self.returncode = -15

    def wait(self, t=None):
        return self.returncode

    def kill(self):
        pass


class Request(unittest.TestCase):
    def ask(self, line):
        a, b = socket.socketpair()
        if line is not None:
            a.sendall(line)
        try:
            return rh.request(b)
        finally:
            a.close()
            b.close()

    def test_video_hevc(self):
        self.assertEqual(self.ask(b"VIDEO hevc\n"), ("VIDEO", {"hevc"}))

    def test_video_plain_and_old_bmc(self):
        self.assertEqual(self.ask(b"VIDEO\n"), ("VIDEO", set()))
        self.assertEqual(self.ask(None), ("VIDEO", set()))  # reimu-kvm before 2026-10: no line

    def test_audio(self):
        self.assertEqual(self.ask(b"AUDIO\n")[0], "AUDIO")


class Stream(unittest.TestCase):
    def test_hevc_access_units_and_keys(self):
        aus = list(rh.access_units(io.BufferedReader(io.BytesIO(HEVC_IDR + HEVC_P + HEVC_CRA + HEVC_P)), rh.CODECS["hevc"]["aud"]))
        self.assertEqual(aus, [HEVC_IDR, HEVC_P, HEVC_CRA, HEVC_P])
        self.assertEqual([rh.is_key(a, "hevc") for a in aus], [True, False, True, False])

    def test_h264_unchanged(self):
        aus = list(rh.access_units(io.BufferedReader(io.BytesIO(H264_IDR + H264_P)), rh.CODECS["h264"]["aud"]))
        self.assertEqual(aus, [H264_IDR, H264_P])
        self.assertEqual([rh.is_key(a, "h264") for a in aus], [True, False])

    def test_ffmpeg_cmd(self):
        rh.GRAB = "kms"
        rh.CARD, rh.PLANE = "/dev/dri/card1", 41
        c = rh.ffmpeg_cmd(1920, 1080, "-", "hevc")
        self.assertIn("hevc_vaapi", c)
        self.assertEqual(c[c.index("-bf") + 1], "0")
        self.assertEqual(c[c.index("-f", c.index("-c:v")) + 1], "hevc")
        self.assertEqual(c[c.index("-profile:v") + 1], "main")
        # a third fewer bits than H.264 for the same picture
        self.assertLess(float(c[c.index("-b:v") + 1].rstrip("M")), rh.args.bitrate)
        h = rh.ffmpeg_cmd(1920, 1080, "-", "h264")
        self.assertIn("h264_vaapi", h)
        self.assertEqual(h[h.index("-b:v") + 1], "%gM" % rh.args.bitrate)


class Serve(unittest.TestCase):
    """serve(): the header names the codec that comes; H.265 asked for and the
    encoder gives nothing: H.264 instead (and not H.265 again)"""

    def run_serve(self, line, outputs):
        started = []

        def popen(cmd, **kw):
            codec = "hevc" if "hevc_vaapi" in cmd else "h264"
            started.append(codec)
            return FakeProc(outputs[codec])

        a, b = socket.socketpair()
        saved = rh.subprocess.Popen, rh.size
        rh.subprocess.Popen, rh.size = popen, lambda: (1920, 1080)
        try:
            a.sendall(line)
            t = threading.Thread(target=rh.serve, args=(b, "bmc"))
            t.start()
            data = b""
            a.settimeout(5)
            while True:
                try:
                    c = a.recv(65536)
                except socket.timeout:
                    break
                if not c:
                    break
                data += c
            t.join(5)
        finally:
            rh.subprocess.Popen, rh.size = saved
            a.close()
        hdr, rest = data.split(b"\n", 1)
        frames = []
        while len(rest) >= 5:
            n, kind = struct.unpack(">IB", rest[:5])
            frames.append((kind, rest[5:5 + n]))
            rest = rest[5 + n:]
        return json.loads(hdr), frames, started

    def setUp(self):
        rh.HEVC_OK = None
        rh.GRAB, rh.CARD, rh.PLANE = "kms", "/dev/dri/card1", 41

    def test_hevc(self):
        hdr, frames, started = self.run_serve(b"VIDEO hevc\n", {"hevc": HEVC_IDR + HEVC_P, "h264": H264_IDR})
        self.assertTrue(hdr["codec"].startswith("hev1"), hdr)
        self.assertEqual(frames, [(1, HEVC_IDR), (0, HEVC_P)])
        self.assertEqual(started, ["hevc"])

    def test_old_page_gets_h264(self):
        hdr, frames, started = self.run_serve(b"VIDEO\n", {"hevc": HEVC_IDR, "h264": H264_IDR + H264_P})
        self.assertEqual(hdr["codec"], "avc1.640028")
        self.assertEqual(frames, [(1, H264_IDR), (0, H264_P)])
        self.assertEqual(started, ["h264"])

    def test_hevc_encoder_fails_h264_instead(self):
        hdr, frames, started = self.run_serve(b"VIDEO hevc\n", {"hevc": b"", "h264": H264_IDR})
        self.assertEqual(hdr["codec"], "avc1.640028")
        self.assertEqual(frames, [(1, H264_IDR)])
        self.assertEqual(started, ["hevc", "h264"])
        # the next connection does not try H.265 again
        hdr, frames, started = self.run_serve(b"VIDEO hevc\n", {"hevc": b"", "h264": H264_IDR})
        self.assertEqual(started, ["h264"])


if __name__ == "__main__":
    unittest.main()
