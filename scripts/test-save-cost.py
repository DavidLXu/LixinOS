"""Compare 4 KiB replacements on identical guest volumes with and without project data."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import socket
import statistics
import subprocess
import tempfile
import time

from disk_image import edit_files, prepare, snapshot_headers

ROOT = Path(__file__).resolve().parents[1]
boot = runpy.run_path(str(ROOT / "scripts/test-user.py"))["boot"]


def command(image, disk, monitor):
    return ["qemu-system-aarch64", "-machine", "virt", "-accel", "hvf", "-cpu", "host",
            "-smp", "1", "-m", "640M", "-display", "none", "-monitor", "none",
            "-serial", "stdio", "-nic", "none", "-qmp", f"unix:{monitor},server=on,wait=off",
            "-global", "virtio-mmio.force-legacy=false", "-drive",
            f"if=none,id=fs,format=raw,cache=writethrough,file={disk}",
            "-device", "virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0", "-kernel", image]


class Monitor:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.connect(str(path))
        self.stream = self.socket.makefile("rwb", buffering=0)
        self.stream.readline()
        self.call("qmp_capabilities")

    def call(self, name):
        self.stream.write(json.dumps({"execute": name}).encode()+b"\n")
        while True:
            row = json.loads(self.stream.readline())
            if "error" in row:
                raise RuntimeError(row)
            if "return" in row:
                return row["return"]

    def written(self):
        return self.call("query-blockstats")[0]["stats"]["wr_bytes"]

    def close(self):
        self.stream.close()
        self.socket.close()


def seed(work, image):
    small = work / "small.img"
    prepare(small)
    sock = work / "qmp.sock"
    with boot(small, command(image, small, sock), prompt=b"$ ") as vm:
        vm.send(b"shutdown\r")
        assert vm.vm.wait(timeout=15) == 0

    def inputs(rows):
        rows[b"/bench"] = bytes(8), None
        for i in range(9):
            rows[f"/bench/v{i}".encode()] = bytes(8), hashlib.shake_256(bytes([i])).digest(4096)
        for key, value in [(b"a", b"A"), (b"b", b"B"), (b"out", b"O")]:
            rows[b"/bench/"+key] = bytes(8), value*4096
    edit_files(small, inputs)
    large = work / "large.img"
    subprocess.run(["cp", "-c", small, large], check=True)
    def projects(rows):
        for i in range(8):
            directory = f"/project-{i}".encode()
            rows[directory] = bytes(8), None
            for j in range(16):
                name = directory + f"/asset-{j}".encode()
                rows[name] = bytes(8), hashlib.shake_256(name).digest(512 << 10)
    edit_files(large, projects)


def measure(work, image, cut):
    for label in ["small", "large"]:
        disk = work / (label + ".img")
        sock = work / "qmp.sock"
        with disk.open("rb") as f:
            used = snapshot_headers(f)[0][2]
        with boot(disk, command(image, disk, sock), prompt=b"$ ") as vm:
            qmp = Monitor(sock)
            times, sizes = [], []
            for i in range(9):
                path = f"v{i}"
                before = qmp.written()
                start = time.monotonic()
                vm.send(f"cp /bench/{path} /bench/out\r".encode())
                out = vm.until(b"$ ", timeout=60)
                assert b"error" not in out and b"cp:" not in out, out
                elapsed = time.monotonic()-start
                written = qmp.written()-before
                if i >= 2:
                    times.append(elapsed)
                    sizes.append(written)
            print(label, "payload", used, "median_ms", round(statistics.median(times)*1000, 2),
                  "median_written", statistics.median(sizes), flush=True)
            qmp.close()
            if cut:
                vm.expect(b"cp /bench/a /bench/out\r", b"$ ")
                vm.vm.kill()
                vm.vm.wait()
        if cut:
            with boot(disk, command(image, disk, sock), prompt=b"$ ") as vm:
                vm.send(b"cat /bench/out\r")
                assert b"A"*4096 in vm.until(b"$ ")
                # Populate both slot caches in this VM before interrupting an incremental commit.
                vm.expect(b"cp /bench/b /bench/out\r", b"$ ")
                vm.expect(b"cp /bench/a /bench/out\r", b"$ ")
                # Race a new commit against abrupt power loss after its first device write.
                qmp = Monitor(sock)
                baseline = qmp.written()
                vm.send(b"cp /bench/b /bench/out\r")
                until = time.monotonic()+15
                while qmp.written() <= baseline+1024:
                    if time.monotonic()>until:
                        raise RuntimeError("no filesystem write observed")
                vm.vm.kill()
                vm.vm.wait()
                qmp.close()
            with boot(disk, command(image, disk, sock), prompt=b"$ ") as vm:
                vm.send(b"cat /bench/out\r")
                content = vm.until(b"$ ").split(b"\n",1)[1]
                assert (b"A"*4096 in content) != (b"B"*4096 in content), content[:100]
                version = "old" if b"A"*4096 in content else "new"
                print(label, "acknowledged save survived; interrupted save recovered", version, flush=True)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("image", type=Path)
    p.add_argument("--work", type=Path)
    p.add_argument("--reuse", action="store_true")
    p.add_argument("--cut", action="store_true")
    args = p.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        work = args.work or Path(temp)
        if not args.reuse:
            work.mkdir(parents=True, exist_ok=True)
            if (work / "small.img").exists() or (work / "large.img").exists():
                raise SystemExit("bench directory must be empty")
            seed(work, args.image.resolve())
        measure(work, args.image.resolve(), args.cut)
