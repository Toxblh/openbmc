#!/usr/bin/env python3
"""Which parts "reimu-ab install" writes, and in what order, over every state
of the flash: the shell functions of reimu-ab (choose, plan_writes) with its
reads of the flash faked, against a model of what boots.

    test_ab_plan.py [REIMU_AB] [SH...]   (default: ../../meta-ab64/.../reimu-ab, sh)

The state: each slot's digests (rofs-X's trailer) missing, or present with
its kernel and its rofs each matching or not; the kernel U-Boot booted (a or
b); state ok, fallback, or started by kexec (reimu.ram: state ok or fallback,
nothing booted by U-Boot; ram: the rofs from the initrd); the slot asked for
(none, a, b, both). 25 x 2 x 5 x 4 = 1000 cases.

The model (what the board can boot): a rofs boots only if it matches its
digest (the initramfs checks it); a kernel boots if it matches its digest or
U-Boot booted it, and otherwise may or may not (U-Boot checks the FIT's own
hash, which nothing here reads): both are tried. A plan is safe if for every
such guess that boots before, something boots before and during each write
(the part being written boots no more; once written it does). Checked:
  - a plan writes both parts of exactly the slots asked for (none: the slot
    not running; fallback: the booted kernel's slot) and is safe;
  - a refusal only when no order of those writes is safe;
  - the order "both" picks when both slots are intact: B, then A.
"""
import itertools, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, "..", "..", "meta-ab64", "recipes-mcst", "reimu-ab", "files", "reimu-ab")
SHELLS = sys.argv[2:] or ["sh"]

# per slot: None (no digests) or (kernel matches, rofs matches)
SLOT_STATES = [None, (True, True), (True, False), (False, True), (False, False)]
# (state file's state, started by kexec)
BOOTS = [("ok", False), ("fallback", False), ("ok", True), ("fallback", True), ("ram", True)]
ARGS = ["", "a", "b", "both"]
other = {"a": "b", "b": "a"}


def cases():
    for sa, sb, k, (st, kx), arg in itertools.product(SLOT_STATES, SLOT_STATES, "ab", BOOTS, ARGS):
        yield {"a": sa, "b": sb}, k, st, kx, arg


def digest_ok(slots, part):  # "kernel-a" -> matches its digest
    p, s = part.split("-")
    t = slots[s]
    return t is not None and t[0 if p == "kernel" else 1]


def model_safe(slots, k, st, kx, order):
    """every guess for the kernels nothing proves: bootable before -> bootable throughout"""
    proven = {x for x in ("kernel-a", "kernel-b", "rofs-a", "rofs-b") if digest_ok(slots, x)}
    if st != "ram" and not kx:
        proven.add("kernel-" + k)
    unknown = [x for x in ("kernel-a", "kernel-b") if x not in proven]
    for guess in itertools.product([False, True], repeat=len(unknown)):
        boots = set(proven) | {x for x, g in zip(unknown, guess) if g}

        def ok(b):
            return any(x in b for x in ("kernel-a", "kernel-b")) and any(x in b for x in ("rofs-a", "rofs-b"))
        if not ok(boots):
            continue
        b = set(boots)
        for x in order:
            if not ok(b - {x}):
                return False
            b.add(x)
    return True


def targets(slots, k, st, arg):
    if arg == "both":
        return ["a", "b"]
    if arg:
        return [arg]
    return [k] if st == "fallback" else [other[k]]


SNIPPET = r'''
part_ok() { in_list "$FAKE_DIGEST" $2-$1; }
booted() { case $1 in kernel) echo $FAKE_K ;; state) echo $FAKE_ST ;; rofs) echo x ;; esac; }
kexec_started() { [ "$FAKE_KX" = 1 ]; }
die() { echo "REFUSED $*"; exit 1; }
log() { :; }
'''


def run(shell):
    script = ["REIMU_AB_LIB=1 . %s" % TOOL, SNIPPET]
    all_cases = list(cases())
    for i, (slots, k, st, kx, arg) in enumerate(all_cases):
        dig = " ".join(x for x in ("kernel-a", "kernel-b", "rofs-a", "rofs-b") if digest_ok(slots, x))
        script.append('( FAKE_DIGEST="%s" FAKE_K=%s FAKE_ST=%s FAKE_KX=%d; choose %s; echo "PLAN %s"; ) | sed "s/^/%d /"'
                      % (dig, k, st, kx, arg, "$slots|$plan", i))
    r = subprocess.run([shell], input="\n".join(script) + "\n", capture_output=True, text=True)
    out = {}
    for l in r.stdout.splitlines():
        i, _, rest = l.partition(" ")
        out[int(i)] = rest
    return all_cases, out, r.stderr


def main():
    failed = 0
    for shell in SHELLS:
        all_cases, out, err = run(shell)
        bad = []
        n_plan = n_ref = 0
        for i, (slots, k, st, kx, arg) in enumerate(all_cases):
            res = out.get(i, "NO OUTPUT " + err[-200:])
            want = targets(slots, k, st, arg)
            parts = [p + "-" + s for s in want for p in ("kernel", "rofs")]
            any_safe = any(model_safe(slots, k, st, kx, perm) for perm in itertools.permutations(parts))
            desc = "a=%s b=%s booted=%s state=%s%s arg=%s" % (slots["a"], slots["b"], k, st, " kexec" if kx else "", arg or "-")
            if res.startswith("PLAN "):
                n_plan += 1
                sl, _, plan = res[5:].partition("|")
                order = plan.split()
                if sorted(sl.split()) != sorted(want) or sorted(order) != sorted(parts):
                    bad.append("%s: writes %s (slots %s), wants slots %s" % (desc, order, sl, want))
                elif not model_safe(slots, k, st, kx, order):
                    bad.append("%s: UNSAFE order %s" % (desc, order))
                if arg == "both" and all(digest_ok(slots, x) for x in parts) and order[:2] != ["kernel-b", "rofs-b"]:
                    bad.append("%s: both intact, but not B first: %s" % (desc, order))
            elif res.startswith("REFUSED "):
                n_ref += 1
                if any_safe:
                    bad.append("%s: refused (%s) though a safe order exists" % (desc, res[8:]))
            else:
                bad.append("%s: %s" % (desc, res))
        print("%s: %d cases, %d plans, %d refusals, %d wrong" % (shell, len(all_cases), n_plan, n_ref, len(bad)))
        for b in bad[:25]:
            print("  FAIL " + b)
        failed += len(bad)
    print("\n%d failed" % failed)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
