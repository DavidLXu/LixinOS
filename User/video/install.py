"""Install the video player and its desktop file association on an idle disk."""
import argparse
import hashlib
from pathlib import Path
import runpy
import struct
import time

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "Kernel/target/user-build"
installer = runpy.run_path(str(ROOT / "User/gcc/install.py"))

# The versions shipped immediately before video file associations were added.
PREVIOUS = {
    "desktop.c": "c03eb8e03237198eca535d74b6ebb4369a864c86ef75de57a0922c52387e005d",
    "window-client-host.h": "f4e1de8468830ec94655c29162d8dde496ba406859578109b742d4c8f6195e76",
}


def install(disk):
    player = installer["package_edit"](BUILD / "video-port/package", "video")
    desktop = installer["package_edit"](BUILD / "netsurf-port/package", "netsurf")

    def edit(rows):
        player(rows)
        desktop(rows)
        for name, previous in PREVIOUS.items():
            key = ("/src/desktop/" + name).encode()
            if key not in rows:
                continue
            times, old = rows[key]
            new = (ROOT / "User/desktop" / name).read_bytes()
            if old == new:
                continue
            if hashlib.sha256(old).hexdigest() != previous:
                print("Preserving customized guest source:", key.decode())
                continue
            rows[key] = times[:4] + struct.pack("<I", int(time.time())), new

    return installer["edit_files"](disk, edit)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path)
    print("Installed snapshot generation", install(parser.parse_args().disk))
