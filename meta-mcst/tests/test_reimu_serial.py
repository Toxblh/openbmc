#!/usr/bin/env python3
"""Integration test for reimu-serial using a pseudo-terminal as the 'serial port'."""
import base64, json, os, socket, subprocess, sys, time, tty, signal

BIN = sys.argv[1]
WORK = sys.argv[2]
os.makedirs(WORK, exist_ok=True)

master, slave = os.openpty()
tty.setraw(slave)
slave_name = os.ttyname(slave)
conf = os.path.join(WORK, "reimu-serial.conf")
with open(conf, "w") as f:
    f.write("# test\n")
    f.write(f"host {slave_name} 115200 lines=20000 rawlog={WORK}/sol.log console Консоль платформы (COM1)\n")
    f.write(f"com2 /dev/nonexistent-tty 9600 lines=500 Второй COM\n")

def start():
    p = subprocess.Popen([BIN, "-c", conf, "-v"], stderr=open(os.path.join(WORK, "daemon.log"), "a"))
    for _ in range(50):
        if os.path.exists("/run/reimu-serial/port0.sock"):
            break
        time.sleep(0.1)
    time.sleep(0.3)
    return p

def connect(idx=0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(f"/run/reimu-serial/port{idx}.sock")
    s.settimeout(5)
    return s

class Reader:
    def __init__(self, s):
        self.s, self.buf = s, b""
    def msg(self):
        while b"\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise EOFError
            self.buf += d
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)
    def until(self, typ):
        out = []
        while True:
            m = self.msg()
            out.append(m)
            if m["type"] == typ:
                return out

def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond:
        global failed
        failed = True

failed = False
for f in os.listdir("/var/log/reimu-serial") if os.path.isdir("/var/log/reimu-serial") else []:
    if f.startswith(("host", "com2")):
        os.unlink(os.path.join("/var/log/reimu-serial", f))
d = start()

# 1. feed 25000 lines from the 'host'
t0 = time.time() * 1000
N = 25000
chunk = "".join(f"line {i:05d} привет\r\n" for i in range(N)).encode()
for i in range(0, len(chunk), 4096):
    os.write(master, chunk[i:i+4096])
    time.sleep(0.001)
os.write(master, b"partial-no-newline")
time.sleep(1.0)

s = connect(0)
r = Reader(s)
msgs = r.until("history_end")
hello = msgs[0]
check(hello["type"] == "hello" and hello["name"] == "host" and hello["title"] == "Консоль платформы (COM1)", "hello message")
check(len(hello["ports"]) == 2 and hello["ports"][1]["name"] == "com2", "hello lists ports")
lines = [l for m in msgs if m["type"] == "history" for l in m["lines"]]
check(len(lines) == 20000, f"history keeps last 20000 lines (got {len(lines)})")
first = base64.b64decode(lines[0][1]).decode()
last = base64.b64decode(lines[-1][1]).decode()
check(first == "line 05000 привет" and last == "line 24999 привет", f"history window content ({first!r}..{last!r})")
check(all(t0 - 1000 <= l[0] <= time.time() * 1000 for l in lines), "timestamps are wall-clock ms")
check(all(lines[i][0] <= lines[i+1][0] for i in range(len(lines)-1)), "timestamps monotonic")
pe = msgs[-1]
check(base64.b64decode(pe["partial"][1]) == b"partial-no-newline", "partial line delivered")

# 2. live data broadcast
os.write(master, b" done\r\nnext line\r\n")
live = b""
while live.count(b"\n") < 2:
    m = r.msg()
    if m["type"] == "data":
        live += base64.b64decode(m["d"])
check(live == b" done\r\nnext line\r\n", "live data stream")

# 3. client -> serial write
s.sendall((json.dumps({"type": "write", "d": base64.b64encode("ls -la\r".encode()).decode()}) + "\n").encode())
time.sleep(0.3)
got = os.read(master, 1000)
check(got == b"ls -la\r", f"write to serial ({got!r})")

# 4. pty bridge (minicom path)
pty_path = os.path.realpath("/run/reimu-serial/host.pty")
pfd = os.open(pty_path, os.O_RDWR | os.O_NOCTTY)
tty.setraw(pfd)
termios_flush = __import__("termios").tcflush(pfd, __import__("termios").TCIFLUSH)
os.write(master, b"to-pty\r\n")
time.sleep(0.3)
got = os.read(pfd, 1000)
check(b"to-pty" in got, f"serial -> pty ({got!r})")
os.write(pfd, b"from-pty")
time.sleep(0.3)
check(os.read(master, 100) == b"from-pty", "pty -> serial")
os.close(pfd)

# 5. obmc-console compatible abstract socket
c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
c.connect("\0obmc-console")
c.settimeout(3)
time.sleep(0.3)
os.write(master, b"raw-client\r\n")
check(c.recv(100) == b"raw-client\r\n", "obmc-console raw socket receive")
c.sendall(b"x")
time.sleep(0.2)
check(os.read(master, 10) == b"x", "obmc-console raw socket send")
c.close()

# 6. second port: offline device, still serves hello + empty history
s2 = connect(1)
m = Reader(s2).until("history_end")
check(m[0]["online"] is False and m[0]["maxLines"] == 500 and m[-1]["count"] == 0, "offline port reported")
s2.close()

# 7. baud change persisted
s.sendall(b'{"type":"baud","value":57600}\n')
st = r.until("status")[-1]
check(st["baud"] == 57600, "baud change acknowledged")

# 8. raw log
time.sleep(0.2)
raw = open(f"{WORK}/sol.log", "rb").read()
check(raw.startswith(b"line 00000") and b"raw-client" in raw, "raw sol.log compatible log")

# 9. restart keeps history
s.close()
d.send_signal(signal.SIGTERM); d.wait(5)
d = start()
s = connect(0)
msgs = Reader(s).until("history_end")
lines = [l for m in msgs if m["type"] == "history" for l in m["lines"]]
texts = [base64.b64decode(l[1]).decode() for l in lines]
check(len(lines) == 20000 and texts[-1] == "raw-client", f"history survives restart (n={len(lines)}, last={texts[-1]!r})")
check(msgs[0]["baud"] == 57600, "baud override survives restart")
os.unlink("/var/lib/reimu-serial/host.baud")

# 10. dump CLI
out = subprocess.run([BIN, "-c", conf, "--dump", "host", "-n", "3"], capture_output=True, text=True, timeout=10).stdout
check(len(out.splitlines()) == 3 and out.splitlines()[-1].endswith("] raw-client"), f"--dump CLI ({out.splitlines()[-1] if out else ''})")

# 11. many lines throughput
t = time.time()
big = b"".join(b"x" * 100 + b"\r\n" for _ in range(20000))
os.write(master, big[:0])
for i in range(0, len(big), 4096):
    os.write(master, big[i:i+4096])
time.sleep(1)
s3 = connect(0)
msgs = Reader(s3).until("history_end")
check(sum(len(m["lines"]) for m in msgs if m["type"] == "history") == 20000, f"2MB burst handled ({time.time()-t:.1f}s)")
s3.close()

s.close()
d.send_signal(signal.SIGTERM); d.wait(5)
print("RESULT:", "FAILED" if failed else "ALL PASSED")
sys.exit(1 if failed else 0)
