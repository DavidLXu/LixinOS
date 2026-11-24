"""Install the Keychain-backed DeepSeek provider on an idle LixinOS volume."""
import argparse
from pathlib import Path
import struct
import sys
import time

PORT = Path(__file__).resolve().parent
sys.path.insert(0, str(PORT.parents[1] / "scripts"))
from disk_image import edit_files


def configure(rows):
    stamp = struct.pack("<I", int(time.time())) * 2
    base = b"/home/lixin/.codex"
    for path in (b"/home", b"/home/lixin", base):
        if rows.setdefault(path, (stamp, None))[1] is not None:
            raise ValueError(f"expected directory at {path.decode()}")
    path = base + b"/config.toml"
    provider = (PORT / "deepseek.toml").read_bytes()
    if path in rows and rows[path][1] != provider:
        raise ValueError("guest already has a custom Codex configuration; merge deepseek.toml into it")
    rows.setdefault(path, (stamp, provider))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path)
    args = parser.parse_args()
    print("Configured DeepSeek on volume generation", edit_files(args.disk.resolve(), configure))
