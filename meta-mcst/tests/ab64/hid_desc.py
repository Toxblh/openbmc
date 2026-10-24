#!/usr/bin/env python3
"""The KVM's USB HID report descriptors (obmc_hid hid.0 keyboard, hid.1 mouse,
made by create_usbhid.sh) as the tests expect them, and a small report
descriptor parser (HID 1.11, 6.2.2) for checking what configfs holds.

The expected bytes are built here from the items (HID 1.11 6.2.2.2: prefix =
bTag << 4 | bType << 2 | bSize), not copied from the script's hex: a typo in
either shows up as a mismatch.

    hid_desc.py FILE [keyboard|mouse]   parse FILE, compare when named
"""
import sys

MAIN, GLOBAL, LOCAL = 0, 1, 2
MAIN_TAGS = {0x8: "Input", 0x9: "Output", 0xb: "Feature", 0xa: "Collection", 0xc: "End Collection"}
GLOBAL_TAGS = {0x0: "Usage Page", 0x1: "Logical Minimum", 0x2: "Logical Maximum", 0x3: "Physical Minimum",
               0x4: "Physical Maximum", 0x5: "Unit Exponent", 0x6: "Unit", 0x7: "Report Size",
               0x8: "Report ID", 0x9: "Report Count", 0xa: "Push", 0xb: "Pop"}
LOCAL_TAGS = {0x0: "Usage", 0x1: "Usage Minimum", 0x2: "Usage Maximum", 0x3: "Designator Index",
              0x4: "Designator Minimum", 0x5: "Designator Maximum", 0x7: "String Index",
              0x8: "String Minimum", 0x9: "String Maximum", 0xa: "Delimiter"}
TAGS = {MAIN: MAIN_TAGS, GLOBAL: GLOBAL_TAGS, LOCAL: LOCAL_TAGS}
NAMES = {(t, tag): n for t, d in TAGS.items() for tag, n in d.items()}
BYTAG = {n: k for k, n in NAMES.items()}


def item(name, value=None, size=None):
    """one short item; size in bytes (0, 1, 2, 4), the smallest that holds
    value unless given"""
    btype, tag = BYTAG[name]
    if value is None:
        data = b""
    else:
        if size is None:
            size = 1 if -128 <= value <= 255 else 2 if -32768 <= value <= 65535 else 4
        data = (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little")
    code = {0: 0, 1: 1, 2: 2, 4: 3}[len(data)]
    return bytes([tag << 4 | btype << 2 | code]) + data


def build(items):
    return b"".join(item(*i) for i in items)


# HID Usage Tables: Generic Desktop 0x01, Keyboard 0x07, LEDs 0x08, Button
# 0x09, Consumer 0x0c. Main item data bits: 0 Constant, 1 Variable, 2 Relative.
KEYBOARD_ITEMS = [
    ("Usage Page", 0x01), ("Usage", 0x06),                  # Generic Desktop, Keyboard
    ("Collection", 0x01),                                   # Application
    ("Usage Page", 0x07), ("Usage Minimum", 0xe0), ("Usage Maximum", 0xe7),  # LeftControl..Right GUI
    ("Logical Minimum", 0), ("Logical Maximum", 1),
    ("Report Size", 1), ("Report Count", 8), ("Input", 0x02),   # 8 modifier bits
    ("Report Count", 1), ("Report Size", 8), ("Input", 0x03),   # reserved byte
    ("Report Count", 5), ("Report Size", 1),
    ("Usage Page", 0x08), ("Usage Minimum", 0x01), ("Usage Maximum", 0x05),  # Num Lock..Kana
    ("Output", 0x02),
    ("Report Count", 1), ("Report Size", 3), ("Output", 0x03),  # LED padding
    ("Report Count", 6), ("Report Size", 8),
    ("Logical Minimum", 0), ("Logical Maximum", 0x65),
    ("Usage Page", 0x07), ("Usage Minimum", 0x00), ("Usage Maximum", 0x65),
    ("Input", 0x00),                                        # 6 key codes (array)
    ("End Collection",),
]
MOUSE_ITEMS = [
    ("Usage Page", 0x01), ("Usage", 0x02),                  # Generic Desktop, Mouse
    ("Collection", 0x01),                                   # Application
    ("Usage", 0x01), ("Collection", 0x00),                  # Pointer, Physical
    ("Usage Page", 0x09), ("Usage Minimum", 1), ("Usage Maximum", 3),  # buttons 1..3
    ("Logical Minimum", 0), ("Logical Maximum", 1),
    ("Report Count", 3), ("Report Size", 1), ("Input", 0x02),
    ("Report Count", 1), ("Report Size", 5), ("Input", 0x03),  # padding
    ("Usage Page", 0x01), ("Usage", 0x30), ("Usage", 0x31),  # X, Y
    ("Physical Minimum", 0), ("Physical Maximum", 32767),
    ("Logical Minimum", 0), ("Logical Maximum", 32767),
    ("Unit", 0x11), ("Unit Exponent", 0),                   # SI Lin: distance
    ("Report Size", 16), ("Report Count", 2), ("Input", 0x02),  # absolute
    ("Usage", 0x38),                                        # Wheel
    ("Physical Minimum", 0), ("Physical Maximum", 0), ("Unit", 0),
    ("Logical Minimum", -127), ("Logical Maximum", 127),
    ("Report Size", 8), ("Report Count", 1), ("Input", 0x06),  # relative
    ("Usage Page", 0x0c), ("Usage", 0x0238, 2),             # Consumer, AC Pan
    ("Logical Minimum", -127), ("Logical Maximum", 127),
    ("Report Size", 8), ("Report Count", 1), ("Input", 0x06),
    ("End Collection",), ("End Collection",),
]
KEYBOARD = build(KEYBOARD_ITEMS)
MOUSE = build(MOUSE_ITEMS)
# hid.N: (name, descriptor, protocol, subclass, report_length)
# The keyboard exactly as in the stock firmware: the host's boot firmware (the
# PNS boot menu) takes that one; don't change it without checking there.
# The absolute mouse: no boot subclass (0/0, create_usbhid.sh's default; the
# stock gadget had 1/2, HID_PTR_BOOT=1 brings it back).
FUNCTIONS = {"hid.0": ("keyboard", KEYBOARD, 1, 1, 8), "hid.1": ("mouse", MOUSE, 0, 0, 7)}
assert len(KEYBOARD) == 63 and len(MOUSE) == 93


def parse(desc):
    """(problems, input report bytes, output report bytes): every item a known
    main/global/local tag, collections balanced, Push/Pop balanced, report
    sizes; problems is a list of strings, empty when the descriptor is sound"""
    problems, depth, i = [], 0, 0
    g = {"Report Size": 0, "Report Count": 0, "Report ID": 0}
    stack, bits, ids = [], {"Input": 0, "Output": 0, "Feature": 0}, set()
    while i < len(desc):
        b = desc[i]
        if b == 0xfe:  # long item: bDataSize, bLongItemTag, data
            if i + 2 >= len(desc):
                problems.append("byte %d: truncated long item" % i)
                break
            problems.append("byte %d: long item (none expected)" % i)
            i += 3 + desc[i + 1]
            continue
        size = (0, 1, 2, 4)[b & 3]
        btype, tag = (b >> 2) & 3, b >> 4
        if i + 1 + size > len(desc):
            problems.append("byte %d: item 0x%02x needs %d data bytes, %d left" % (i, b, size, len(desc) - i - 1))
            break
        data = int.from_bytes(desc[i + 1:i + 1 + size], "little")
        name = NAMES.get((btype, tag))
        if name is None:
            problems.append("byte %d: unknown %s item tag 0x%x (prefix 0x%02x)"
                            % (i, ("main", "global", "local", "reserved")[btype], tag, b))
        elif name == "Collection":
            depth += 1
        elif name == "End Collection":
            depth -= 1
            if depth < 0:
                problems.append("byte %d: End Collection without a Collection (stack underflow)" % i)
                depth = 0
        elif name in bits:
            if depth == 0:
                problems.append("byte %d: %s outside any collection" % (i, name))
            bits[name] += g["Report Size"] * g["Report Count"]
        elif name in g:
            g[name] = data
            if name == "Report ID":
                ids.add(data)
        elif name == "Push":
            stack.append(dict(g))
        elif name == "Pop":
            if not stack:
                problems.append("byte %d: Pop without Push" % i)
            else:
                g = stack.pop()
        i += 1 + size
    if depth:
        problems.append("%d collection(s) left open" % depth)
    if stack:
        problems.append("%d Push without Pop" % len(stack))
    if ids:
        problems.append("report IDs %s (the gadget writes reports without one)" % sorted(ids))
    for k, v in bits.items():
        if v % 8:
            problems.append("%s report is %d bits, not whole bytes" % (k, v))
    return problems, bits["Input"] // 8, bits["Output"] // 8


def check_function(desc, fn, report_length=None):
    """problems of a function's descriptor (hid.0 or hid.1): exact bytes,
    parse, report length"""
    name, want, _, _, length = FUNCTIONS[fn]
    out = []
    if desc != want:
        out.append("%s descriptor: %d bytes %s, expected %d bytes %s"
                   % (name, len(desc), desc.hex(" "), len(want), want.hex(" ")))
    problems, inbytes, _ = parse(desc)
    out += ["%s descriptor: %s" % (name, p) for p in problems]
    if inbytes != length:
        out.append("%s descriptor: input report %d bytes, expected %d" % (name, inbytes, length))
    if report_length is not None and report_length != length:
        out.append("%s report_length %d, expected %d" % (name, report_length, length))
    return out


if __name__ == "__main__":
    d = open(sys.argv[1], "rb").read()
    problems, inb, outb = parse(d)
    print("%d bytes, input report %d bytes, output %d" % (len(d), inb, outb))
    if len(sys.argv) > 2:
        fn = {"keyboard": "hid.0", "mouse": "hid.1"}[sys.argv[2]]
        problems = check_function(d, fn)
    for p in problems:
        print("  " + p)
    sys.exit(1 if problems else 0)
