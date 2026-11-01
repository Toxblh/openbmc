#!/usr/bin/env python3
"""The independent reference: a byte log through pyte, a terminal of the size
the host believes: pyte-screen.py LOG ROWS COLS -> JSON lines of the screen"""
import json, re, sys, pyte
rows, cols = int(sys.argv[2]), int(sys.argv[3])
class Screen(pyte.Screen):
    # CSI > 4;2 m and such (vim's modifyOtherKeys request): no display change
    def select_graphic_rendition(self, *attrs, **kw):
        if kw.get("private"):
            return
        super().select_graphic_rendition(*attrs)


screen = Screen(cols, rows)
stream = pyte.ByteStream(screen)
# DCS (vim's XTGETTCAP queries): pyte prints them, a terminal doesn't
stream.feed(re.sub(rb"\x1bP.*?\x1b\\", b"", open(sys.argv[1], "rb").read(), flags=re.S))
print(json.dumps([l.rstrip() for l in screen.display]))
