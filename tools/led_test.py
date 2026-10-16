#!/usr/bin/env python3
"""Cycle the display's RGB LED through numbered colours / patterns via the bridge.

Useful to see what reads well through a coloured light pipe:

    python3 tools/led_test.py colors              # 8 colours, steady
    python3 tools/led_test.py patterns --rgb 255,0,0
    python3 tools/led_test.py stop

The display shows the number and name of the current step in big letters.
"""

import argparse
import json
import time
import urllib.request

BRIDGE = "http://127.0.0.1:47321"

COLORS = [
    ("1 red", (255, 0, 0)),
    ("2 orange", (255, 60, 0)),
    ("3 amber", (255, 140, 0)),
    ("4 green", (0, 255, 0)),
    ("5 cyan", (0, 255, 180)),
    ("6 blue", (0, 0, 255)),
    ("7 white", (255, 255, 255)),
    ("8 pink", (255, 0, 160)),
]
PATTERNS = ["steady", "breathe", "slow breathe", "fast pulse", "double blink", "blink"]


def send(cfg):
    req = urllib.request.Request(BRIDGE + "/api/device", data=json.dumps(cfg).encode(),
                                 headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=3).read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["colors", "patterns", "stop"])
    ap.add_argument("--seconds", type=float, default=7)
    ap.add_argument("--loops", type=int, default=6)
    ap.add_argument("--rgb", default="255,0,0", help="colour for the pattern test")
    ap.add_argument("--only", help="comma separated colour numbers, e.g. 1,4,7")
    a = ap.parse_args()
    if a.mode == "stop":
        send({"led_rgb": 0})
        return
    if a.mode == "colors":
        steps = [(n, rgb, 0) for n, rgb in COLORS]
        if a.only:
            keep = {int(x) for x in a.only.split(",")}
            steps = [s for s in steps if int(s[0].split()[0]) in keep]
    else:
        rgb = tuple(int(x) for x in a.rgb.split(","))
        steps = [(f"{i + 1} {p}", rgb, i) for i, p in enumerate(PATTERNS)]
    try:
        for _ in range(a.loops):
            for label, rgb, pat in steps:
                send({"led_rgb": list(rgb), "led_pat": pat, "led_label": label, "led_ms": int(a.seconds * 1000) + 1500})
                time.sleep(a.seconds)
    finally:
        send({"led_rgb": 0})


if __name__ == "__main__":
    main()
