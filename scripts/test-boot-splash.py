"""Observe moving boot pixels and the interactive shell using a disposable QEMU overlay."""
import argparse
from pathlib import Path
import runpy
import shutil
import socket
import subprocess
import tempfile
import time

from disk_image import snapshot_slots

SCRIPTS = Path(__file__).resolve().parent
Serial = runpy.run_path(str(SCRIPTS / "test-user.py"))["Serial"]
qmp_connection = runpy.run_path(str(SCRIPTS / "test-mutter.py"))["qmp_connection"]


def light_position(qmp, frame):
    qmp("screendump", filename=str(frame))
    with frame.open("rb") as source:
        assert source.readline() == b"P6\n"
        width, height = map(int, source.readline().split())
        assert source.readline().strip() == b"255"
        start = source.tell()
        stride = (frame.stat().st_size - start) // height
        if width < 246 or height < 70:
            return -1
        left, top = (width - 246) // 2, (height - 70) // 2
        source.seek(start + top * stride + left * 3)
        if source.read(3) != bytes.fromhex("e4efe9"):
            return -1
        source.seek(start + (top + 67) * stride + left * 3)
        return source.read(246 * 3).find(bytes.fromhex("75d6c6") * 8) // 3


def check(image, disk, gpu, ram, unthrottled):
    with tempfile.TemporaryDirectory(dir="/tmp") as temp:
        work = Path(temp)
        if not unthrottled:
            # Exercise a cold mount even if the seed already has a fast lookup index.
            seed = work / "cold.img"
            subprocess.run(["cp", "-c", disk, seed], check=True)
            with seed.open("r+b") as volume:
                for offset, _ in snapshot_slots(volume):
                    volume.seek(offset + 28)
                    volume.write(bytes(8))
            disk = seed
        endpoint = work / "qmp"
        frame = work / "boot.ppm"
        device = "ramfb" if gpu == "ramfb" else "virtio-gpu-device,bus=virtio-mmio-bus.7"
        # Slow test reads make fast mounts observable between screenshots.
        throttle = "" if unthrottled else "bps_rd=16777216,iops_rd=200,"
        args = [shutil.which("qemu-system-aarch64"), "-machine", "virt", "-accel", "hvf",
                "-cpu", "host", "-smp", "2", "-m", ram, "-S", "-display", "none",
                "-qmp", f"unix:{endpoint},server=on,wait=off", "-monitor", "none", "-serial", "stdio",
                "-global", "virtio-mmio.force-legacy=false", "-device", device,
                "-device", "virtio-keyboard-device,bus=virtio-mmio-bus.1",
                "-device", "virtio-tablet-device,bus=virtio-mmio-bus.3",
                "-object", "rng-random,id=rng,filename=/dev/urandom",
                "-device", "virtio-rng-device,rng=rng,bus=virtio-mmio-bus.5",
                "-netdev", "user,id=net", "-device", "virtio-net-device,netdev=net,bus=virtio-mmio-bus.4",
                "-drive", f"if=none,id=fs,snapshot=on,format=raw,{throttle}file={str(disk).replace(',', ',,')}",
                "-device", "virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0", "-kernel", str(image)]
        with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE) as vm:
            try:
                deadline = time.monotonic() + 20
                while not endpoint.exists() and time.monotonic() < deadline and vm.poll() is None:
                    time.sleep(.02)
                with socket.socket(socket.AF_UNIX) as sock:
                    sock.settimeout(10)
                    sock.connect(str(endpoint))
                    with sock.makefile("rwb") as control:
                        qmp = qmp_connection(control)
                        qmp("qmp_capabilities")
                        qmp("cont")
                        positions = set()
                        deadline = time.monotonic() + 60
                        while len(positions) < 2 and time.monotonic() < deadline:
                            position = light_position(qmp, frame)
                            if position >= 0:
                                positions.add(position)
                            time.sleep(.04)
                        assert len(positions) >= 2, f"expected a moving light, saw {positions}"
                        guest = Serial(vm)
                        guest.until(b"$ ", timeout=240)
                        guest.send(b"echo boot-ready\r")
                        assert b"\nboot-ready\n" in guest.until(b"$ ", timeout=30)
                        assert light_position(qmp, frame) == -1, "splash still visible after login"
                        print(image.name, gpu, "moving splash and interactive shell verified", flush=True)
            finally:
                vm.terminate()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--gpu", choices=("ramfb", "virtio"), default="ramfb")
    parser.add_argument("--ram", default="1536M")
    parser.add_argument("--unthrottled", action="store_true", help="observe normal disk speed")
    args = parser.parse_args()
    check(args.image, args.disk.resolve(strict=True), args.gpu, args.ram, args.unthrottled)
