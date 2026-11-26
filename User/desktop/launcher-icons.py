"""Compose the launcher icon bank from artwork in User/desktop/icons.

The bank is a small binary read by the desktop at startup.  Icons are stored
twice, at the exact launcher and dock sizes, so the guest only has to blit
them; no image decoder or scaler is linked into the desktop.

Artwork sources and licences are listed next to the assets in icons/README.md.
"""

from pathlib import Path
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
ICONS = HERE / "icons"
MAGIC = b"LXI1"
GRID = 76
DOCK = 40

# Must match enum LauncherIcon in desktop.c.
ORDER = [
    "folder", "browser", "calc", "terminal", "sheet", "pencil", "store",
    "gear", "chart", "tiles", "play", "calendar", "map", "watch", "photo",
    "mine", "mahjong", "eyes", "agent", "code", "counter", "widgets",
    "number", "radar", "gear-engine", "tank", "yuri", "writer", "pdf",
    "apps", "memory", "disk", "git", "python", "node", "doom", "classicube",
    "freecad", "codex", "physics", "paint", "nfs",
]

# The artwork files use readable names; keep ORDER tied to the enum.
FILES = {
    "folder": "folder.png", "browser": "browser.png", "calc": "calc.png",
    "terminal": "terminal.png", "sheet": "sheet.png", "pencil": "editor.png",
    "store": "store.png", "gear": "settings.png", "chart": "tasks.png",
    "tiles": "tiles.png", "play": "video.png", "calendar": "calendar.png",
    "map": "maps.png", "watch": "clock.png", "photo": "photo.png",
    "mine": "mines.png", "mahjong": "mahjong.png", "eyes": "eyes.png",
    "agent": "deepseek.png", "code": "code.png", "counter": "counter.png",
    "widgets": "widgets.png", "number": "game2048.png", "radar": "radar.png",
    "gear-engine": "engineering.png", "tank": "tank.png", "yuri": "yuri.png",
    "writer": "writer.png", "pdf": "pdf.png", "apps": "apps.png",
    "memory": "memory.png", "disk": "disk.png", "git": "git.png",
    "python": "python.png", "node": "node.png", "doom": "doom.png",
    "classicube": "classicube.png", "freecad": "freecad.png",
    "codex": "codex.png", "physics": "physics.png", "paint": "paint.png",
    "nfs": "nfs.png",
}


def compose(source: Path, size: int) -> Image.Image:
    art = Image.open(source).convert("RGBA")
    box = art.getbbox()
    if box:
        art = art.crop(box)
    margin = 3 if size >= 60 else 2
    art.thumbnail((size - margin * 2, size - margin * 2), Image.LANCZOS)

    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    x = (size - art.width) // 2
    y = (size - art.height) // 2 - 1

    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    shadow.paste(art, (x, y), art)
    shadow = shadow.filter(ImageFilter.GaussianBlur(max(1, size // 22)))
    dark = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    dark.paste((0, 0, 0, 120 if size >= 60 else 95), (0, 0, size, size),
               shadow.getchannel("A"))
    canvas.alpha_composite(dark, (0, max(1, size // 25)))

    rim = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    rim.paste(art, (x, y), art)
    rim = rim.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(0.7))
    white = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    white.paste((255, 255, 255, 70 if size >= 60 else 45), (0, 0, size, size),
                rim.getchannel("A"))
    canvas.alpha_composite(white)

    canvas.alpha_composite(art, (x, y))
    return canvas


def pack(image: Image.Image) -> bytes:
    out = bytearray()
    for r, g, b, a in image.getdata():
        out += struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)
    return bytes(out)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: launcher-icons.py OUTPUT")
    missing = [name for name in ORDER if name not in FILES or not (ICONS / FILES[name]).is_file()]
    if missing:
        raise SystemExit("missing icon artwork: " + ", ".join(missing))
    grid = [compose(ICONS / FILES[name], GRID) for name in ORDER]
    dock = [compose(ICONS / FILES[name], DOCK) for name in ORDER]
    data = bytearray(struct.pack("<4sIII", MAGIC, len(ORDER), GRID, DOCK))
    for image in grid + dock:
        data += pack(image)
    destination = Path(sys.argv[1])
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    print(f"{destination}: {len(ORDER)} icons, {len(data)} bytes", flush=True)


if __name__ == "__main__":
    main()
