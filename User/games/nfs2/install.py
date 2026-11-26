"""Install immutable game assets and seed writable player data in one snapshot."""
import argparse
from pathlib import Path
import runpy
import struct
import time

ROOT = Path(__file__).resolve().parents[3]
DEFAULT = ROOT / "Kernel/target/user-build/nfs2-port/package"
installer = runpy.run_path(str(ROOT / "User/gcc/install.py"))
PROFILE_DIRS = ("fedata/pc/config", "fedata/pc/save", "fedata/pc/stats",
                "gamedata/prefs", "gamedata/tmptrk")


def package_edit(package):
    assets = installer["package_edit"](package, "nfs2")

    def update(rows):
        assets(rows)
        stamp = struct.pack("<I", int(time.time())) * 2
        profile = Path("/scratch/nfs2")
        game = package / "lib/nfs2/game"

        def seed(path, value=None):
            record = rows.setdefault(str(path).encode(), (stamp, value))
            if (record[1] is None) != (value is None):
                raise ValueError(f"Player file conflicts with directory {path}")

        seed(profile.parent)
        seed(profile)
        for directory in PROFILE_DIRS:
            path = profile
            for part in Path(directory).parts:
                path = path / part
                seed(path)
            source = game / directory
            for item in sorted(source.rglob("*")):
                seed(path / item.relative_to(source), item.read_bytes() if item.is_file() else None)

    return update


def install(package, disk):
    return installer["edit_files"](disk, package_edit(package))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path)
    parser.add_argument("--package", type=Path, default=DEFAULT)
    args = parser.parse_args()
    print("Installed nfs2 in snapshot", install(args.package, args.disk))
