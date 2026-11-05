#!/usr/bin/env python3
"""Grab screenshots from the display over serial.

  python3 tools/screenshot.py /dev/ttyUSB0 [--wait 15] KEY=out.png ...

KEY picks the screen first (d = dashboard, c = camera, g = settings,
p = printer list, _ = whatever is showing); see ui_debug_command. Opening
the port can reset the board, so all shots share one connection and
--wait gives it time to boot and connect. Needs pyserial and Pillow.
"""
import argparse
import base64
import time

import serial
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("port")
ap.add_argument("--wait", type=float, default=15)
ap.add_argument("shots", nargs="+")
args = ap.parse_args()

s = serial.Serial(args.port, 115200, timeout=1)
time.sleep(args.wait)


def grab(out):
    s.reset_input_buffer()
    s.write(b"s")
    img = Image.new("RGB", (320, 240), (255, 0, 255))  # magenta = lost data
    area, data = None, {}

    def paste():
        x, y, w, h = area
        buf = bytearray(w * h * 2)
        good = bytearray(w * h * 2)
        for off, chunk in data.items():
            buf[off:off + len(chunk)] = chunk
            good[off:off + len(chunk)] = b"\x01" * len(chunk)
        for i in range(w * h):
            if not good[2 * i]:
                continue
            v = buf[2 * i] | buf[2 * i + 1] << 8  # LVGL RGB565, little-endian
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            img.putpixel((x + i % w, y + i // w), (r * 255 // 31, g * 255 // 63, b * 255 // 31))

    deadline = time.time() + 60
    while time.time() < deadline:
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("[shot]"):
            if area:
                paste()
            rest = line[6:].strip()
            if rest == "end":
                break
            area, data = tuple(int(v) for v in rest.split()), {}
        elif area and line.startswith("@"):
            # "@offset base64"; drop lines that another task's log broke up.
            try:
                off_s, b64 = line[1:].split(" ", 1)
                off, chunk = int(off_s), base64.b64decode(b64, validate=True)
            except Exception:
                continue
            total = area[2] * area[3] * 2
            if off % 384 == 0 and len(chunk) == min(384, total - off):
                data[off] = chunk
    img.save(out)
    print("saved", out)


for spec in args.shots:
    key, out = spec.split("=", 1)
    if key != "_":
        s.write(key.encode())
        time.sleep(2)
    grab(out)
