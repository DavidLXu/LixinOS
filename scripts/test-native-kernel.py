"""Build the C/assembly kernel inside LixinOS and rebuild it under its own EL1."""
import argparse
import os
from pathlib import Path, PurePosixPath
import runpy
import shutil
import socket
import subprocess
import tempfile

from disk_image import HISTORY, HISTORY_BYTES, checksum, snapshot_headers

root = Path(__file__).resolve().parents[1]
guest = runpy.run_path(str(root / "scripts/test-user.py"))
decode = runpy.run_path(str(root / "scripts/export-git.py"))["decode"]
shell = guest["SHELL"]


def command(serial, line, status=0, expected=b"", prompt=shell):
    serial.send(line.encode() + b"\r")
    output = serial.until(prompt, timeout=180)
    assert expected in output, output
    serial.send(b"echo $?\r")
    result = serial.until(prompt)
    assert f"\n{status}\n".encode() in result, (output, result)


def shutdown(serial):
    serial.send(b"q\r")
    serial.until(b"Shutting down LixinOS")
    assert serial.vm.wait(timeout=15) == 0


def read_file(disk, path):
    with disk.open("rb") as image:
        _, base, size, digest, header = snapshot_headers(image)[0]
        image.seek(base + 512)
        data = image.read(size)
        assert checksum(data) == digest
        return decode(data, header[:8])[PurePosixPath(path)]


def native_boot(image, disk, prompt=b"LixinOS Bash (C shell)"):
    args = ["qemu-system-aarch64", "-machine", "virt", "-accel", "hvf", "-cpu", "host",
            "-m", "1536M", "-display", "none", "-monitor", "none", "-serial", "stdio",
            "-global", "virtio-mmio.force-legacy=false", "-drive",
            f"if=none,id=vol,format=raw,cache=writethrough,file={disk}",
            "-device", "virtio-blk-device,drive=vol,bus=virtio-mmio-bus.0",
            "-object", "rng-random,id=rng,filename=/dev/urandom",
            "-device", "virtio-rng-device,rng=rng,bus=virtio-mmio-bus.5",
            "-netdev", "user,id=eth", "-device", "virtio-net-device,netdev=eth,bus=virtio-mmio-bus.4",
            "-kernel", str(image)]
    return guest["boot"](disk, args, prompt=prompt)


def check_boot_release(serial, flavor):
    release = serial.until(b"\n").strip()
    assert release.endswith(b"-" + flavor), release
    serial.until(shell)
    command(serial, "uname -r", expected=b"\n" + release + b"\n")
    summary = b"LixinOS " + release + b" aarch64 (standalone; non-Linux)"
    command(serial, "uname -a", expected=summary)
    command(serial, "cat /proc/version", expected=summary)


def verify(disk, image, loader):
    with guest["boot"](disk, prompt=b"Kernel ") as serial:
        check_boot_release(serial, b"rust")
        command(serial, "tcc /src/tinycc/bootstrap.c -o /bin/tcc")
        command(serial, "tcc --bootloader @/src/kernel/loader-sources -o /boot/loader.elf")
        command(serial, "tcc --kernel @/src/kernel/sources -o /boot/kernel.elf")
        command(serial, "echo 'extern int puts(char*); void _start(void){puts(\"bad\");}' > bad.c")
        command(serial, "tcc --kernel bad.c -o /boot/kernel.elf", 1)
        shutdown(serial)
    image.write_bytes(read_file(disk, "/boot/kernel.elf"))
    loader.write_bytes(read_file(disk, "/boot/loader.elf"))
    with native_boot(loader, disk, prompt=b"Kernel ") as serial:
        check_boot_release(serial, b"tcc")
        command(serial, "tcc /src/cecho/cecho.c -o /bin/cecho")
        command(serial, "netinfo", expected=b"IPv4 10.0.2.15/24")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(5)
            command(serial, "tcc /src/net/tcpcheck.c -o /tcpcheck")
            command(serial, f"/tcpcheck 10.0.2.2 {listener.getsockname()[1]}", expected=b"TCP connected")
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(5)
                assert connection.recv(1) == b""
        command(serial, "echo tcc-native-login", expected=b"\ntcc-native-login\n")
        command(serial, "cat /src/kernel/sources", expected=b"/src/kernel/el1.S")
        command(serial, "ls -1 /src/kernel", expected=b"boot.S")
        command(serial, "echo six | cat | wc -c", expected=b"\n4\n")
        command(serial, "echo native-write > note")
        command(serial, "cat < note", expected=b"\nnative-write\n")
        command(serial, "mkdir -p project/src")
        command(serial, "echo hello > project/src/note")
        command(serial, "cp -r project/src project/copy")
        command(serial, "mv project/copy project/moved")
        command(serial, "cat project/moved/note", expected=b"hello")
        command(serial, "rm -r project/moved")
        project_prompt = b"lixin:/project$ "
        command(serial, "cd project", prompt=project_prompt)
        command(serial, "git init", expected=b"Git repository ready", prompt=project_prompt)
        command(serial, "git add src/note", prompt=project_prompt)
        command(serial, "git commit -m native", prompt=project_prompt)
        command(serial, "git log", expected=b"native", prompt=project_prompt)
        command(serial, "cd /")
        serial.expect(b"kilo note\r", b"Ctrl-F = find")
        serial.expect(b"X\x13", b"bytes written on disk")
        serial.expect(b"\x11", shell)
        command(serial, "cat note", expected=b"Xnative-write")
        serial.expect(b"htop\r", b"Tasks: ")
        serial.expect(b"M", b"Sort [MEM%]")
        serial.expect(b"/zzzz", b"No matching processes")
        serial.expect(b"\x1b", b"F1 Help")
        serial.expect(b"?", b"Toggle process tree")
        serial.expect(b"\r", b"F1 Help")
        serial.expect(b"q", shell)
        command(serial, "cat /proc/cpuinfo", expected=b"LixinOS aarch64 vCPU")
        command(serial, "cat /proc/meminfo", expected=b"MemTotal:")
        command(serial, "ps", expected=b"PID PPID STATE MODE")
        command(serial, "df", expected=b"Filesystem Capacity Used Available")
        command(serial, "sysinfo", expected=b"heap_used_bytes=")
        command(serial, "sleep 1 &", expected=b"[pid ")
        command(serial, "tcc /src/tinycc/bootstrap.c -o /tcc-stage2")
        command(serial, "/tcc-stage2 --bootloader @/src/kernel/loader-sources -o /boot/loader.elf")
        command(serial, "/tcc-stage2 --kernel @/src/kernel/sources -o /boot/kernel.elf")
        command(serial, "tcc -DNDEBUG -I/src/micropython -I/src/tinycc/sdk @/src/micropython/sources.txt -o /bin/python")
        command(serial, "echo 'print(sum(i*i for i in range(20)))' > test.py")
        command(serial, "python test.py", expected=b"2470")
        guest["check_python_system"](serial)
        shutdown(serial)
    export = runpy.run_path(str(root / "scripts/export-git.py"))["export"]
    with tempfile.TemporaryDirectory() as directory:
        repo = Path(directory) / "project"
        export(disk, PurePosixPath("/project"), repo)
        subprocess.run(["git", "-C", str(repo), "fsck", "--full"], check=True)
        assert subprocess.check_output(["git", "-C", str(repo), "show", "HEAD:src/note"]) == b"hello\n"
    assert read_file(disk, "/boot/loader.elf") == loader.read_bytes(), "two generations of bootloader ELF differ"
    second = read_file(disk, "/boot/kernel.elf")
    assert second == image.read_bytes(), "two generations of kernel ELF differ"
    image.write_bytes(second)
    with native_boot(loader, disk) as serial:
        serial.until(shell)
        command(serial, "cat note", expected=b"\nXnative-write\n")
        command(serial, "history", expected=b"tcc-stage2 --kernel")
        command(serial, "pwd", expected=b"\n/\n")
        command(serial, """tcc --kernel '-DKERNEL_BANNER="LixinOS booted the updated ELF"' @/src/kernel/sources -o /boot/kernel.elf""")
        shutdown(serial)
    with native_boot(loader, disk, prompt=b"LixinOS booted the updated ELF") as serial:
        serial.until(shell)
        command(serial, "tcc --kernel @/src/kernel/sources -o /boot/kernel.elf")
        command(serial, "rm -r project py-system")
        command(serial, "rm note bad.c test.py /tcpcheck /tcc-stage2")
        shutdown(serial)
    assert read_file(disk, "/boot/kernel.elf") == image.read_bytes()

    # Exercise the production window launcher with the guest-built ELF. The
    # existing GUI suite observes framebuffer pixels and sends real tablet events.
    gui = runpy.run_path(str(root / "scripts/test-gui.py"))
    from unittest.mock import patch
    with patch.dict(os.environ, {"LIXINOS_KERNEL": str(loader)}):
        gui["harness"]["test"](
            lambda console, control, tmp: gui["exercise"](console, control, tmp, True, None),
            mode="--window", disk=disk,
        )
    # The delivered system starts with an empty interactive command history.
    # Only the disposable test disk reaches this point, after QEMU has exited.
    with disk.open("r+b") as volume:
        volume.seek(HISTORY)
        volume.write(bytes(HISTORY_BYTES))



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--save-elf", type=Path)
    parser.add_argument("--save-bootloader", type=Path)
    parser.add_argument("--save-disk", type=Path)
    args = parser.parse_args()
    for dest in (args.save_elf, args.save_disk, args.save_bootloader):
        if dest and dest.exists():
            parser.error(f"destination already exists: {dest}")
    with tempfile.TemporaryDirectory() as directory:
        disk = Path(directory) / "guest.img"
        image = Path(directory) / "kernel.elf"
        loader = Path(directory) / "loader.elf"
        verify(disk, image, loader)
        for src, dest in ((image,args.save_elf),(disk,args.save_disk),(loader,args.save_bootloader)):
            if dest:
                with src.open("rb") as data, dest.open("xb") as target:
                    shutil.copyfileobj(data,target)
    print("Verified two identical kernel generations, guest-built TCC, EL0 Bash, disk I/O and reboot.")


if __name__ == "__main__":
    main()
