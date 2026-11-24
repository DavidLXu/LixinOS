"""Boot the upstream GNOME Shell session on the LixinOS kernels."""
import argparse
import os
from pathlib import Path
import re
import runpy
import shutil
import tempfile
import time
from collections import deque
from unittest.mock import patch

from disk_image import edit_files, prepare

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MUTTER = runpy.run_path(str(HERE / "test-mutter.py"))
BUILD = MUTTER["BUILD"]
DBUS = MUTTER["DBUS"]
GUEST = MUTTER["GUEST"]
STAGE = BUILD / "shell-cross"
PACKAGE = runpy.run_path(str(ROOT / "User/gnome/package.py"))
SHELL_STARTED = b"GNOME Shell started at"


def start_session(guest, kernel, display="--virtual-monitor 320x240"):
    runtime = f"/tmp/lx-gnome-{kernel}-virtual"
    MUTTER["session"](guest, runtime)
    system_bus = f"unix:abstract=lx-gnome-system-{kernel}"
    GUEST["command"](guest, f"export DBUS_SYSTEM_BUS_ADDRESS={system_bus} "
                     "XDG_DATA_DIRS=/lib/gnome/share FONTCONFIG_FILE=/lib/gnome/fonts.conf "
                     "LIBGWEATHER_LOCATIONS_PATH=/lib/gnome/Locations.bin "
                     "GSK_RENDERER=cairo XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=GNOME "
                     "XCURSOR_PATH=/lib/gnome/share/icons G_MESSAGES_DEBUG=Clutter")
    GUEST["command"](guest, "dbus-daemon --nofork --config-file=/lib/dbus/session.conf "
                     f"--address={system_bus} &")
    guest.send(f"dbus-run-session -- /bin/gnome-shell {display} --wayland-display wayland-gnome-test\r".encode())


def wait_for_desktop(guest, renderer=None):
    deadline = time.monotonic() + (120 if renderer else 45)
    started = False
    matched = renderer is None
    transcript = deque(maxlen=45)
    try:
        while time.monotonic() < deadline:
            line = guest.until(b"\n", timeout=deadline - time.monotonic())
            transcript.append(line)
            started |= SHELL_STARTED in line
            if renderer and b"LixinOS compositor renderer:" in line:
                assert b"LixinOS compositor renderer: " + renderer in line, line
                matched = True
            if started and matched:
                return b"\n".join(transcript)
            if b"[exit " in line:
                break
    except TimeoutError:
        transcript.append(guest.buffer)
    raise AssertionError(b"\n".join(transcript).decode(errors="replace"))


def virgl_scanout(directory, width, height):
    # Cocoa GL presents a texture. QMP screendump reads a different, black
    # CPU surface. Observe the DMA mapping created by the resize instead.
    trace = (directory / "qmp.trace").read_text()
    maps = set()
    for addr, length in re.findall(r"address_space_map .* addr 0x([0-9a-f]+):([0-9a-f]+) ", trace):
        n = int(length, 16)
        stride, rem = divmod(n, height * 4)
        if not rem and width <= stride <= 4096:
            maps.add((int(addr, 16), stride))
    if len(maps) != 1:
        raise AssertionError(f"Expected one DMA mapping for the resized {width}x{height} scanout, got {maps}")
    return maps.pop()


def read_scanout(qmp, directory, address, stride, width, height):
    backing = directory / "scanout.bgrx"
    qmp("pmemsave", val=address, size=stride * height * 4, filename=str(backing))
    raw = backing.read_bytes()
    pixels = bytearray(width * height * 3)
    for y in range(height):
        row = raw[y * stride * 4:(y * stride + width) * 4]
        start = y * width * 3
        end = start + width * 3
        pixels[start:end:3] = row[2::4]
        pixels[start+1:end:3] = row[1::4]
        pixels[start+2:end:3] = row[0::4]
    return f"P6\n{width} {height}\n255\n".encode() + pixels


def check_window(disk, release, kernel, screenshot_directory, virgl=False):
    def exercise(guest, control, directory):
        qmp = MUTTER["qmp_connection"](control)
        qmp("qmp_capabilities")
        if virgl:
            qmp("trace-event-set-state", name="address_space_map", enable=True)
        Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text("2024\t1320\n")
        guest.until(b"$ ")
        frame = directory / "shell.ppm"
        dimensions = b"P6\n2024 1320\n"
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            qmp("screendump", filename=str(frame))
            if frame.read_bytes().startswith(dimensions):
                break
            time.sleep(.2)
        else:
            raise AssertionError(f"QEMU did not resize the display: {frame.read_bytes()[:30]!r}")

        if virgl:
            qmp("trace-event-set-state", name="address_space_map", enable=False)
            mapping = virgl_scanout(directory, 2024, 1320)
            # Cocoa GL samples the whole texture, including any row padding.
            # Looking only at the guest's visible pixels misses a black strip.
            assert mapping[1] == 2024, f"VirGL texture stride {mapping[1]} exceeds the visible width"
            metal_time = runpy.run_path(str(HERE / "test-egl.py"))["metal_time"]
            pid = int((directory / "qmp.pid").read_text())
            before = metal_time(pid)
        guest.send(b"startgnome\r")
        transcript = wait_for_desktop(guest, b"virgl" if virgl else None)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if virgl:
                screen = read_scanout(qmp, directory, *mapping, 2024, 1320)
                frame.write_bytes(screen)
            else:
                qmp("screendump", filename=str(frame))
                screen = frame.read_bytes()
            if screen.startswith(dimensions) and bytes.fromhex("023c88") * 128 in screen:
                screenshot_directory.mkdir(parents=True, exist_ok=True)
                shutil.copy2(frame, screenshot_directory / f"gnome-shell-{kernel}.ppm")
                print(f"{kernel}: startgnome displayed the GNOME workspaces", flush=True)
                break
            time.sleep(.2)
        else:
            screenshot_directory.mkdir(parents=True, exist_ok=True)
            shutil.copy2(frame, screenshot_directory / f"gnome-shell-{kernel}-unverified.ppm")
            raise AssertionError(transcript.decode(errors="replace"))

        if virgl:
            after = metal_time(pid)
            assert after > before, (pid, before, after)
            print(f"{kernel}: compositor GL_RENDERER=virgl, QEMU PID {pid}, Metal time {before} -> {after}", flush=True)

        if virgl:
            # Allow the Shell's startup animation to finish before opening search.
            time.sleep(6)
        for key in ("l", "o", "g", "spc", "o", "u", "t"):
            qmp("human-monitor-command", **{"command-line": f"sendkey {key} 40"})
            time.sleep(.30 if virgl else .12)
        if virgl:
            # Search results replace the blue workspace overview asynchronously.
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline:
                search_frame = read_scanout(qmp, directory, *mapping, 2024, 1320)
                if bytes.fromhex("023c88") * 128 not in search_frame:
                    break
                time.sleep(.5)
            (screenshot_directory / f"gnome-search-{kernel}.ppm").write_bytes(search_frame)
        time.sleep(1 if virgl else .6)
        qmp("human-monitor-command", **{"command-line": "sendkey ret 40"})
        logout = guest.until(GUEST["shell"], timeout=45 if virgl else 15)
        assert b"[exit " not in logout, logout
        print(f"{kernel}: GNOME logged out to the terminal", flush=True)

    with patch.dict(os.environ, {"LIXINOS_GPU": "virgl"} if virgl else {}):
        MUTTER["with_window"](disk, release, kernel, exercise)


def check(release, kernels, version_only, screenshot_directory, with_xfce, virgl=False):
    with tempfile.TemporaryDirectory(prefix="lixin-gnome-") as temporary:
        temporary = Path(temporary)
        disk = temporary / "gnome.img"
        package = temporary / "package"
        prepare(disk)
        with DBUS["boot"](disk, release, "rust") as guest:
            GUEST["check_boot_release"](guest, b"rust")
            GUEST["shutdown"](guest)
        if version_only:
            pack = runpy.run_path(str(ROOT / "User/gcc/package.py"))["tool"]
            pack(package, "/bin/gnome-shell", STAGE / "src/gnome-shell", library="/lib/gnome",
                 build_dir=temporary, launcher_flag="-DLX_DBUS", deflate=True)
        else:
            PACKAGE["build_package"](package)
            DBUS["INSTALL"](DBUS["PACKAGE"], disk, "dbus")

        def install(rows):
            for file in package.rglob("*"):
                path = b"/" + file.relative_to(package).as_posix().encode()
                rows[path] = (bytes(8), file.read_bytes() if file.is_file() else None)

        if version_only:
            edit_files(disk, install)
        else:
            DBUS["INSTALL"](package, disk, "gnome")
        if with_xfce:
            wayland = runpy.run_path(str(HERE / "test-wayland.py"))
            wayland["install"](disk, True)
        for kernel in kernels:
            with DBUS["boot"](disk, release, kernel) as guest:
                GUEST["check_boot_release"](guest, kernel.encode())
                GUEST["command"](guest, "gnome-shell --version", expected=b"GNOME Shell 49.0")
                print(f"{kernel}: GNOME Shell 49.0 executable ran", flush=True)
                if not version_only:
                    start_session(guest, kernel)
                    wait_for_desktop(guest)
                    print(f"{kernel}: GNOME Shell reported desktop startup complete", flush=True)
            if screenshot_directory:
                check_window(disk, release, kernel, screenshot_directory, virgl)
            if with_xfce:
                wayland["check_graphics"](disk, release, kernel, True)
                print(f"{kernel}: Xfce and Desktop started from the GNOME test disk", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", default="latest")
    parser.add_argument("--kernel", choices=("rust", "tcc"), action="append")
    parser.add_argument("--version-only", action="store_true")
    parser.add_argument("--screenshots", type=Path, help="verify the QEMU display and save PPM captures")
    parser.add_argument("--with-xfce", action="store_true", help="also run Xfce and Desktop from the test disk")
    parser.add_argument("--virgl", action="store_true", help="verify real GNOME composition on the accelerated Cocoa display")
    args = parser.parse_args()
    if args.virgl and not args.screenshots:
        args.screenshots = BUILD / "virgl-session-screenshots"
    if args.version_only and (args.screenshots or args.with_xfce):
        parser.error("--version-only excludes session and graphics checks")
    check(args.release, args.kernel or ("rust", "tcc"), args.version_only,
          args.screenshots, args.with_xfce, args.virgl)
