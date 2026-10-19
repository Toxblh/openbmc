#!/usr/bin/env python3
"""com-paced.py SOCKET BYTES_PER_S SECONDS TAG: writes boot-log-like lines to a COM
socket at a real line rate (QEMU feeds an emulated UART as fast as the socket
allows, MB/s, which overruns any 115200-baud reader; this doesn't)."""
import socket, sys, time
path, rate, secs, tag = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
s = socket.socket(socket.AF_UNIX); s.connect(path)
sent = 0; n = 0; t0 = time.time()
while time.time() - t0 < secs:
    line = f"\x1b[32m[ OK ]\x1b[0m {tag} строка {n:06d} загрузка сервиса\r\n".encode()
    s.sendall(line); sent += len(line); n += 1
    ahead = sent / rate - (time.time() - t0)
    if ahead > 0: time.sleep(ahead)
print(f"{tag}: {n} lines, {sent} bytes in {time.time()-t0:.1f}s ({sent/(time.time()-t0)/1024:.1f} KiB/s)")
