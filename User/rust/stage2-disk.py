"""Create a fresh stage2 workspace from the selected kernel's powered-off stage1 disk."""
import argparse
from pathlib import Path
import runpy
import subprocess

port = Path(__file__).resolve().parent
volume = runpy.run_path(str(port / "bootstrap-disk.py"))


def prepare(source, output):
    if output.exists():
        raise FileExistsError(output)
    before = source.stat()
    owners = subprocess.run(["lsof", "-t", "--", str(source)], capture_output=True)
    if owners.returncode != 1 or owners.stdout or owners.stderr:
        raise ValueError(f"cannot confirm {source} is idle; shut down its VM first")
    rows, tail = volume["disk_entries"](source)
    after = source.stat()
    if (before.st_ino, before.st_mtime_ns) != (after.st_ino, after.st_mtime_ns):
        raise ValueError(f"source changed during preparation: {source}")
    for path in ("/stage1/rustc", "/stage1/cargo", "/bootstrap/rust/Cargo.lock"):
        if path not in rows or rows[path][1] is None:
            raise ValueError(f"stage1 disk lacks {path}")
    for path in ("/stage1/rustc", "/stage1/cargo"):
        if rows[path][1][:4] != b"\x7fELF":
            raise ValueError(f"expected a native executable at {path}")
    if "/stage2" in rows:
        raise ValueError("source already contains a stage2 directory")
    # Start with an empty Cargo target and cache. Sources and archives come
    # from the locked offline registry on this kernel's stage1 disk.
    rows = {k: v for k, v in rows.items() if not k.startswith("/scratch/")}
    rows["/bootstrap/bootstrap.rs"] = volume["stamp"], port / "bootstrap.rs"
    volume["write_volume"](rows, tail, output, with_loader="/boot/loader.elf" in rows)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    prepare(args.source, args.output)
