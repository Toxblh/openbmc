#!/usr/bin/env python3
"""A "platform" for the web console tests: a program on a pty behind a QEMU
UART socket, paced as a real 115200 baud line.

    com-pty-bridge.py SOCK LOG ROWS COLS TERM CMD...

ROWS COLS: what the tty says (TIOCSWINSZ; 0 0 as a serial getty leaves it).
LOG: every byte the program wrote, for the independent reference.
"""
import fcntl, os, pty, select, signal, socket, struct, sys, termios, time

sock_path, log_path, rows, cols, term = sys.argv[1:6]
cmd = sys.argv[6:]
RATE = 11520  # bytes/s

s = socket.socket(socket.AF_UNIX)
s.connect(sock_path)
pid, m = pty.fork()
if pid == 0:
    os.environ["TERM"] = term
    os.environ["LANG"] = os.environ["LC_ALL"] = "en_US.UTF-8"
    os.execvp(cmd[0], cmd)
fcntl.ioctl(m, termios.TIOCSWINSZ, struct.pack("HHHH", int(rows), int(cols), 0, 0))
log = open(log_path, "wb", buffering=0)
sent, t0, gone = 0, time.time(), False
signal.signal(signal.SIGTERM, lambda *a: os.kill(pid, signal.SIGKILL) or sys.exit(0))
while not gone:
    r, _, _ = select.select([m, s], [], [], 1)
    if m in r:
        try:
            d = os.read(m, 512)
        except OSError:
            d = b""
        if not d:
            gone = True
        else:
            log.write(d)
            s.sendall(d)
            sent += len(d)
            ahead = sent / RATE - (time.time() - t0)
            if ahead > 0:
                time.sleep(ahead)
            elif ahead < -1:
                t0 = time.time() - sent / RATE - 0.1
    if s in r:
        d = s.recv(4096)
        if not d:
            gone = True
        else:
            os.write(m, d)
