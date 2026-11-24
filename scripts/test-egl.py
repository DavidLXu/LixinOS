"""Verify Mesa GLES shader pixels and the selected renderer on both kernels."""
import argparse
import os
import plistlib
from pathlib import Path
import runpy
import shlex
import subprocess
import tempfile

from disk_image import edit_files, prepare

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
GUEST = runpy.run_path(str(HERE / "test-native-kernel.py"))
BUILD = ROOT / "Kernel/target/user-build"


def boot(disk, release, kernel):
    return GUEST["guest"]["boot"](
        disk, ["env", f"LIXINOS_TCC_DISK={disk}", HERE / "run-kernel.sh",
               "--kernel", kernel, "--release", release, "--serial"], prompt=b"Kernel ")


def metal_time(pid):
    clients = plistlib.loads(subprocess.check_output(["ioreg", "-ar", "-c", "AGXDeviceUserClient"]))
    return sum(int(row["accumulatedGPUTime"]) for client in clients
               if client.get("IOUserClientCreator", "").startswith(f"pid {pid},")
               for row in client.get("AppUsage", ()) if row.get("API") == "Metal")


def check_virgl(temp, disk, release, kernel):
    # Keep the launcher's accelerated Cocoa display. Redirect only the UART
    # and record the exec'd QEMU PID for per-process Metal accounting.
    qemu = Path(os.environ.get("LIXINOS_QEMU", BUILD /
                "virtio-gpu-host/qemu-11.1.0-vee2/bin/qemu-system-aarch64")).resolve()
    if not qemu.is_file():
        raise ValueError("set LIXINOS_QEMU to the GL-enabled QEMU executable")
    wrapper = temp / "qemu-serial"
    wrapper.write_text("""#!/usr/bin/env python3
import os, sys
from pathlib import Path
args = sys.argv[1:]
if '-serial' in args:
    args[args.index('-serial') + 1] = 'stdio'
    Path(os.environ['EGL_TEST_PID']).write_text(str(os.getpid()))
qemu = os.environ['EGL_TEST_QEMU']
os.execv(qemu, [qemu, *args])
""")
    wrapper.chmod(0o700)
    pidfile = temp / "pid"
    launch = ["env", "LIXINOS_GPU=virgl", f"LIXINOS_TCC_DISK={disk}", f"LIXINOS_QEMU={wrapper}",
              f"EGL_TEST_QEMU={qemu}", f"EGL_TEST_PID={pidfile}", HERE / "run-kernel.sh",
              "--window", "--release", release, "--kernel", kernel]
    with GUEST["guest"]["boot"](disk, launch, prompt=b"$ ") as serial:
        pid = int(pidfile.read_text())
        before = metal_time(pid)
        for _ in range(2):
            serial.send(b"lixin-egl-check --headless\r")
            output = serial.until(b"$ ", timeout=120)
            assert b"Mesa EGL and GLES passed (virgl" in output, output
            assert b"GLES pixels: background=" in output, output
            serial.send(b"echo $?\r")
            assert b"\n0\n" in serial.until(b"$ ")
        after = metal_time(pid)
        assert after > before, f"QEMU {pid} Metal time did not increase ({before}, {after})"
        begin = output.index(b"GLES pixels:")
        end = output.index(b"\n", output.index(b"Mesa EGL and GLES passed"))
        print(f"{kernel} QEMU PID {pid}, Metal time {before} -> {after}\n" +
              output[begin:end].decode(), flush=True)
        GUEST["shutdown"](serial)


def check(release, virgl=False):
    pack = runpy.run_path(str(ROOT / "User/gcc/package.py"))["tool"]
    gnome = BUILD / "gnome-port"
    env = dict(os.environ, PKG_CONFIG_PATH="",
               PKG_CONFIG_LIBDIR=os.pathsep.join(str(BUILD / f"{port}/sysroot/lib/pkgconfig")
                                               for port in ("gnome-port", "gtk3-port")))
    flags = subprocess.check_output(["pkg-config", "--static", "--cflags", "--libs", "glesv2"],
                                    env=env, text=True)
    with tempfile.TemporaryDirectory(prefix="lixin-egl-") as temp:
        temp = Path(temp)
        disk = temp / "mesa.img"
        package = temp / "package"
        elf = temp / "lixin-egl-check"
        subprocess.run([gnome / "cc", "-Os", "-I", ROOT / "User/gui", ROOT / "User/gnome/egl-check.c",
                        "-Wl,--start-group", *shlex.split(flags), "-Wl,--end-group", "-o", elf],
                       env=env, check=True)
        pack(package, "/bin/lixin-egl-check", elf,
             library="/lib/mesa", launcher_flag="-DLX_DBUS", build_dir=temp, deflate=True)
        prepare(disk)
        with boot(disk, release, "rust") as serial:
            GUEST["check_boot_release"](serial, b"rust")
            GUEST["shutdown"](serial)

        def install(rows):
            for file in package.rglob("*"):
                path = b"/" + file.relative_to(package).as_posix().encode()
                if path not in rows:
                    rows[path] = (bytes(8), file.read_bytes() if file.is_file() else None)

        edit_files(disk, install)
        for kernel in ("rust", "tcc"):
            if virgl:
                check_virgl(temp, disk, release, kernel)
                continue
            with boot(disk, release, kernel) as serial:
                GUEST["check_boot_release"](serial, kernel.encode())
                GUEST["command"](serial, "lixin-egl-check --headless",
                                 expected=b"Mesa EGL and GLES passed (softpipe)")
                GUEST["shutdown"](serial)
            print(f"{kernel}: Mesa rendered and verified GLES shader pixels", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", default="latest")
    parser.add_argument("--virgl", action="store_true", help="use accelerated Cocoa and check host Metal activity")
    args = parser.parse_args()
    check(args.release, args.virgl)
