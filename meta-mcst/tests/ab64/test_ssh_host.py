#!/usr/bin/env python3
"""reimu-ssh-host (the web console's «SSH: платформа» tab) against fakes:
ssh records where it was asked to go, reimu-usbnet answers its status line.

The platform's address over the USB network is found by itself (the host's
lease, reimu-usbnet status host_ipv4) on every connect: only the user is
asked, once, and kept. A full user@IP typed in is kept as a fixed target
(as before). Without the USB network: the full address is asked.

    python3 test_ssh_host.py [path/to/reimu-ssh-host]
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                         os.path.join(HERE, "../../meta-common/recipes-mcst/reimu-serial/files/reimu-ssh-host"))
failed = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failed.append(what)


def stand():
    d = tempfile.mkdtemp(prefix="sshhost-")
    b = os.path.join(d, "bin")
    os.makedirs(b)
    # ssh: one line per call, its target (the last argument); the next
    # usbnet address, if queued, becomes current (a new lease)
    open(os.path.join(b, "ssh"), "w").write(
        '#!/bin/sh\nfor a; do t=$a; done\necho "$t" >> "$D/calls"\n'
        '[ -s "$D/next_ip" ] && mv "$D/next_ip" "$D/ip"\nexit 0\n')
    open(os.path.join(b, "ssh-keygen"), "w").write(
        '#!/bin/sh\nwhile [ $# -gt 0 ]; do [ "$1" = -f ] && f=$2; shift; done\n'
        'echo key > "$f"; echo "ssh-ed25519 AAAA test" > "$f.pub"\n')
    # (not on PATH on the BMC: /usr/libexec; given by REIMU_USBNET)
    open(os.path.join(b, "reimu-usbnet"), "w").write(
        '#!/bin/sh\n[ "$1" = status ] || exit 2\nip=$(cat "$D/ip" 2>/dev/null)\n'
        'printf \'{"enabled":true,"internet":false,"state":"configured","address":"172.31.83.1","host_ipv4":"%s","error":""}\\n\' "$ip"\n')
    for f in os.listdir(b):
        os.chmod(os.path.join(b, f), 0o755)
    return d


def run(d, stdin, ip=None, next_ip=None):
    if ip is not None:
        open(os.path.join(d, "ip"), "w").write(ip)
    if next_ip is not None:
        open(os.path.join(d, "next_ip"), "w").write(next_ip)
    if os.path.exists(os.path.join(d, "calls")):
        os.unlink(os.path.join(d, "calls"))
    env = dict(os.environ, D=d, REIMU_SSH_STATE=os.path.join(d, "state"), REIMU_USBNET=os.path.join(d, "bin", "reimu-usbnet"),
               PATH=os.path.join(d, "bin") + ":" + os.environ["PATH"])
    p = subprocess.run(["bash", SCRIPT], input=stdin, capture_output=True, text=True, env=env, timeout=20)
    calls = open(os.path.join(d, "calls")).read().split() if os.path.exists(os.path.join(d, "calls")) else []
    return p.stdout, calls


def state(d, name):
    f = os.path.join(d, "state", name)
    return open(f).read().strip() if os.path.exists(f) else None


# 1. first time, the host on the USB network: only the user asked
d = stand()
out, calls = run(d, "alice\n", ip="172.31.83.2")
check("Пользователь на платформе" in out and "пользователь@IP" not in out.split("Пользователь на платформе")[0],
      "first time with the USB network: asks only the user")
check(calls == ["alice@172.31.83.2"], "connects to the host's USB address: %s" % calls)
check(state(d, "user") == "alice" and state(d, "target") is None, "keeps the user, no fixed target")

# 2. next time: no question, the address found again
out, calls = run(d, "", ip="172.31.83.3")
check("Пользователь на платформе" not in out and calls == ["alice@172.31.83.3"], "next time: no question, today's lease: %s" % calls)

# 3. after ssh ends: Enter again finds the address again (a new lease meanwhile)
out, calls = run(d, "\n", ip="172.31.83.2", next_ip="172.31.83.4")
check(calls == ["alice@172.31.83.2", "alice@172.31.83.4"], "Enter after exit: again, with the new lease: %s" % calls)

# 4. after exit a user alone: that user, the address still found
out, calls = run(d, "bob\n", ip="172.31.83.2")
check(calls == ["alice@172.31.83.2", "bob@172.31.83.2"] and state(d, "user") == "bob", "a user typed after exit: %s" % calls)

# 5. after exit a full user@IP: a fixed target from then on
out, calls = run(d, "carol@10.0.0.5\n", ip="172.31.83.2")
check(calls == ["bob@172.31.83.2", "carol@10.0.0.5"] and state(d, "target") == "carol@10.0.0.5", "user@IP after exit: fixed: %s" % calls)
out, calls = run(d, "", ip="172.31.83.2")
check(calls == ["carol@10.0.0.5"], "a fixed target stays, the USB address not used: %s" % calls)

# 6. a fixed target from before (older firmware): used as it is
d = stand()
os.makedirs(os.path.join(d, "state"))
open(os.path.join(d, "state", "target"), "w").write("dave@192.0.2.7\n")
out, calls = run(d, "", ip="172.31.83.2")
check(calls == ["dave@192.0.2.7"], "an old saved user@IP: as it is: %s" % calls)

# 7. no USB network: the full address asked, an empty Enter says what to type
d = stand()
out, calls = run(d, "\nerin\nerin@192.0.2.9\n", ip="")
check("пользователь@IP" in out and calls == ["erin@192.0.2.9"], "no USB network: user@IP asked: %s" % calls)
check("например" in out, "an empty or bad answer says how it should look")

# 8. the first question: an empty Enter is answered, not asked silently again
d = stand()
out, calls = run(d, "\nfrank\n", ip="172.31.83.2")
check(out.count("Пользователь на платформе") == 2 and "имя пользователя" in out and calls == ["frank@172.31.83.2"],
      "an empty user: told, asked again: %s" % calls)

print("%d failed" % len(failed))
sys.exit(1 if failed else 0)
