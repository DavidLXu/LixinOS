"""Drive a native build on a dedicated disk and stream its guest serial output."""
import argparse
import os
from pathlib import Path
import re
import runpy
import sys

root = Path(__file__).resolve().parents[2]
guest = runpy.run_path(str(root / "scripts/test-user.py"))


def command(serial, line):
    prompt = guest["SHELL"]
    serial.send(line.encode() + b"\r")
    pending = serial.buffer
    serial.buffer = b""
    while prompt not in pending:
        end = pending.rfind(b"\n") + 1
        sys.stdout.buffer.write(pending[:end])
        sys.stdout.buffer.flush()
        chunk = os.read(serial.vm.stdout.fileno(), 8192)
        if not chunk:
            raise EOFError(pending[end:])
        pending = pending[end:] + chunk
    tail, _, serial.buffer = pending.partition(prompt)
    sys.stdout.buffer.write(tail)
    sys.stdout.buffer.flush()
    serial.send(b"echo $?\r")
    result = serial.until(prompt)
    code = re.search(rb"\n(\d+)\n", result)
    if not code:
        raise ValueError(result)
    return int(code[1])


def shutdown(serial):
    serial.send(b"q\r")
    serial.until(b"Shutting down LixinOS", timeout=600)
    if serial.vm.wait(timeout=60):
        raise RuntimeError("guest shutdown failed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path)
    parser.add_argument("component", choices=("rustc", "cargo", "both", "verify"))
    args = parser.parse_args()
    disk = args.disk.resolve(strict=True)
    with disk.open("rb") as source:
        if source.read(8) != b"LIXBLD01":
            parser.error("a dedicated LIXBLD01 build disk is required")
    launcher = [root / "scripts/run-kernel.sh", "--kernel", "rust", "--serial"]
    components = ("rustc", "cargo", "verify") if args.component == "both" else (args.component,)
    with guest["boot"](disk, launcher) as serial:
        status = command(serial, "rustc /bootstrap/bootstrap.rs -o /scratch/bootstrap")
        for component in components:
            if status:
                break
            status = command(serial, "/scratch/bootstrap " + component)
        shutdown(serial)
    if status == 0 and "verify" in components:
        with guest["boot"](disk, launcher) as serial:
            for line in ("/stage1/rustc -vV", "/stage1/cargo -V",
                         "/scratch/stage1-test-target/debug/stage1_verify",
                         "/stage1/cargo run -v --offline --target-dir /scratch/stage1-cli-target "
                         "--manifest-path /scratch/stage1-verify/Cargo.toml"):
                status = command(serial, line)
                if status:
                    break
            shutdown(serial)
    sys.exit(status)
