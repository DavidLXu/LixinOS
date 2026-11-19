"""Merge powered-off daily disks into a new shared 4 GiB LixinOS volume."""
import argparse
from pathlib import Path
import runpy
import subprocess
import sys

from disk_image import HEADER, HISTORY, HISTORY_BYTES

ROOT = Path(__file__).resolve().parents[1]
volume = runpy.run_path(str(ROOT / "User/rust/bootstrap-disk.py"))


def merge(rust, tcc, boot, tools, output):
    for dest in (output, output.with_suffix(".loader.elf")):
        if dest.exists():
            raise FileExistsError(f"destination already exists: {dest}")
    sources = [rust, tcc, boot] + ([tools] if tools else [])
    owners = subprocess.run(["lsof", "-t", "--", *map(str, sources)], capture_output=True)
    if owners.returncode not in (0, 1) or owners.stderr:
        raise OSError("could not check whether source images are in use")
    if owners.stdout.strip():
        raise OSError("a source image is in use; shut down its VM before migrating")
    originals = {p: p.stat() for p in sources}
    with rust.open("rb") as src:
        if src.read(len(HEADER)) != HEADER:
            raise ValueError("expected a LIXDISK4 Rust daily disk")
        src.seek(HISTORY)
        history = src.read(HISTORY_BYTES)
    rows, tail = volume["disk_entries"](rust)
    extra, tcc_tail = volume["disk_entries"](tcc)
    if tcc_tail and tcc_tail != tail:
        raise ValueError("TCC disk has independent revision history; merge it before migrating")
    for path, row in extra.items():
        if path in rows:
            existing = rows[path][1]
            incoming = row[1]
            if (existing is None) != (incoming is None):
                raise ValueError(f"file/directory conflict at {path}")
            if not path.startswith(("/bin/", "/lib/", "/boot/", "/src/")) and existing != incoming:
                raise ValueError(f"conflicting user files at {path}; reconcile the two copies first")
        else:
            rows[path] = row
    rows.update(volume["boot_files"](boot))
    if tools:
        built, _ = volume["disk_entries"](tools)
        required = {"/stage1/rustc", "/stage1/cargo", "/bin/rustc", "/bin/cargo",
                    "/bin/gcc", "/bin/ld", "/bin/as", "/bin/ar", "/bin/ranlib",
                    "/lib/rust", "/lib/gcc"}
        missing = required - built.keys()
        if missing:
            raise ValueError(f"toolchain disk lacks {', '.join(sorted(missing))}")
        for path, row in built.items():
            if path in required or path.startswith(("/lib/rust/", "/lib/gcc/")):
                rows[path] = row
            elif path in ("/stage1", "/.cargo", "/.cargo/config.toml", "/scratch", "/scratch/tmp"):
                rows.setdefault(path, row)
    for directory in ("/scratch", "/artifacts"):
        _, data = rows.setdefault(directory, (volume["stamp"], None))
        if data is not None:
            raise ValueError(f"expected a directory at {directory}")
    for path, original in originals.items():
        current = path.stat()
        if (current.st_ino, current.st_mtime_ns) != (original.st_ino, original.st_mtime_ns):
            raise OSError(f"input changed during migration: {path}")
    volume["write_volume"](rows, tail, output, with_loader=True, history=history)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rust", type=Path, default=ROOT / "data/lixinos.img")
    parser.add_argument("--tcc", type=Path, default=ROOT / "data/tcc-os/system.img")
    parser.add_argument("--boot", type=Path, required=True, help="disk with the released TCC kernel and matching loader")
    parser.add_argument("--toolchain", type=Path, help="copy stage1 and its runtime from this build disk")
    parser.add_argument("--output", type=Path, default=ROOT / "data/shared.img")
    args = parser.parse_args()
    try:
        merge(args.rust, args.tcc, args.boot, args.toolchain, args.output)
    except (OSError, ValueError) as exc:
        sys.exit(f"Cannot prepare shared disk: {exc}")
