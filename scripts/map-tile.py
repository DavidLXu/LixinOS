#!/usr/bin/env python3
"""Serve one visible OpenStreetMap tile to a QEMU guestfwd client."""
import io
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from PIL import Image


def tile(z, x, y):
    cache = Path.home() / "Library/Caches/LixinOS/osm"
    path = cache / str(z) / str(x) / f"{y}.png"
    if not path.exists() or time.time() - path.stat().st_mtime > 7 * 86400:
        url = f"https://tile.openstreetmap.org/{z}/{x}/{y}.png"
        try:
            png = subprocess.check_output(["curl", "--fail", "--silent", "--max-time", "10",
                                           "-A", "LixinOS-Maps/1.0 (interactive desktop viewer)", url])
            with Image.open(io.BytesIO(png)) as image:
                if image.size != (256, 256):
                    raise ValueError("unexpected tile size")
                image.verify()
            path.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as tmp:
                tmp.write(png)
            os.replace(tmp.name, path)
        except (OSError, ValueError, subprocess.SubprocessError):
            if not path.exists():
                raise
    with Image.open(path) as im:
        indexed = im if im.mode == "P" else im.convert("RGB").quantize(colors=256)
        palette = bytes(indexed.getpalette("RGB")).ljust(768, b'\0')
        colors = [(r >> 3) << 11 | (g >> 2) << 5 | b >> 3 for r,g,b in zip(*[iter(palette)]*3)]
        return struct.pack("<256H", *colors) + indexed.tobytes()


if __name__ == "__main__":
    request = sys.stdin.buffer.readline(48)
    match = re.fullmatch(rb"(\d{1,2}) (\d{1,6}) (\d{1,6})\n", request)
    if match:
        z,x,y = map(int, match.groups())
        if z <= 19 and max(x,y) < 1 << z:
            try:
                sys.stdout.buffer.write(tile(z,x,y))
            except (OSError, ValueError, subprocess.SubprocessError):
                sys.exit(1)
