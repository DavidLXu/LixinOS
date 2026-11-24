"""Boot the desktop and exercise the Lite XL window on disposable guest disks."""
from launcher_grid import installed_apps, select_icon
import argparse
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile
import time
from unittest.mock import patch

from disk_image import disk_entries, edit_files, require_local

root = Path(__file__).resolve().parents[1]
here = root / "scripts"
harness = runpy.run_path(str(here / "test-panes.py"))
native = runpy.run_path(str(here / "test-native-kernel.py"))
resize_source = runpy.run_path(str(here / "test-resize.py"))["resize_source"]
package_root = root / "Kernel/target/user-build"
WIDTH, HEIGHT = 2024, 1320
WORK_W, WORK_H = WIDTH // 2, (HEIGHT - 44) // 2
L, T = 76 + (WORK_W - 76 - 720) // 2, (WORK_H - 480) // 2
PROJECT = b"/lite-xl-demo"
PROMPT = b"lixin:" + PROJECT + b"$ "
SOURCE = b"// " + "中文".encode() + b"\nint main(void) { return 0; }\n" + b"// sample line\n" * 45
EDITED = "// 窗口\nint answer(void)\n{\n\treturn 42;\n}\n".encode()
INK = "233c3d"
DOC = "282c30"
TREE = "202428"


def seed(rows):
    stamp = bytes(8)
    rows[PROJECT] = stamp, None
    rows[PROJECT + b"/main.c"] = stamp, SOURCE
    rows[PROJECT + b"/helper.h"] = stamp, b"#define LIMIT 42\n"


def exercise(guest, control, directory, screenshot, clipboard):
    assert "QMP" in json.loads(control.readline())

    def qmp(name, **arguments):
        control.write(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return

    def key(keys):
        qmp("human-monitor-command", **{"command-line": f"sendkey {keys} 30"})

    def point(x, y):
        qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(pixel * 32767 / (size - 1))}}
            for axis, pixel, size in (("x", 2*x+1, WIDTH), ("y", 44+2*y+1, HEIGHT))
        ])

    def button(name, down):
        qmp("input-send-event", events=[
            {"type": "btn", "data": {"button": name, "down": down}}
        ])

    def click(x, y):
        point(x, y)
        time.sleep(.06)
        button("left", True)
        time.sleep(.07)
        button("left", False)

    frame = directory / "lite-xl.ppm"

    def grab():
        qmp("screendump", filename=str(frame))
        _, dims, _, data = frame.read_bytes().split(b"\n", 3)
        return dims, data

    def color(data, x, y):
        offset = ((44 + 2*y) * WIDTH + 2*x) * 3
        return data[offset:offset+3].hex()

    def wait(check, description):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            dims, data = grab()
            if dims == f"{WIDTH} {HEIGHT}".encode() and check(data):
                return data
            time.sleep(.06)
        if screenshot:
            subprocess.run(["sips", "-s", "format", "png", frame, "--out", screenshot], check=True)
        raise AssertionError(f"Desktop did not show {description}")

    qmp("qmp_capabilities")
    Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text(f"{WIDTH}\t{HEIGHT}\n")
    wait(lambda _: True, "resized console")
    guest.expect(b"cd " + PROJECT + b"\r", PROMPT)
    guest.send(b"ls -1 /bin\r")
    output = re.sub(rb"\x1b\[[0-9;]*m", b"", guest.until(PROMPT))
    apps = installed_apps(output)
    index = apps.index(b"lite-xl.win3")
    guest.send(b"desktop\r")
    wait(lambda p: color(p, 2, 2) == "281e2b", "desktop dock")
    click(36, 8 + 7*min(82, (WORK_H-48)//8) + 20)
    wait(lambda p: color(p, 82, 6) == "203f51", "application menu")
    select_icon(click,qmp,WORK_W,WORK_H,index+6,len(apps)+6)
    wait(lambda p: color(p, L+25, T+84) == TREE and color(p, L+500, T+300) == DOC,
         "Lite XL project window")

    click(L+90, T+114)  # main.c in the project tree
    point(L+440, T+180)

    def sample(p):
        return tuple(color(p, x, y) for x in range(L+238, L+448, 4)
                     for y in range(T+82, T+165, 2))

    wait(lambda p: any(color(p, x, T+153) != DOC for x in range(L+275, L+410)),
         "loaded source file")
    time.sleep(.5)
    before = sample(grab()[1])
    if screenshot:
        subprocess.run(["sips", "-s", "format", "png", frame, "--out", screenshot], check=True)
    for _ in range(4):
        button("wheel-down", True)
        button("wheel-down", False)
    wait(lambda p: sum(a != b for a, b in zip(sample(p), before)) > 12,
         "scroll wheel changing source viewport")

    click(L+420, T+166)
    key("ctrl-a")
    time.sleep(.20)
    key("ctrl-c")
    wait(lambda _: clipboard.read_bytes().startswith(SOURCE[:40]),
         "clipboard receiving selected source from the editor")
    selected = sample(grab()[1])
    pasted = clipboard.with_suffix(".read")
    pasted.unlink(missing_ok=True)
    clipboard.write_bytes(EDITED.replace(b"\n", b"\r\n"))
    key("ctrl-shift-v")
    wait(lambda _: pasted.exists(), "clipboard bridge reading the host selection")
    wait(lambda p: sum(a != b for a, b in zip(sample(p), selected)) > 12 and
         color(p, L+500, T+300) == DOC, "editor after pasting")

    # A dirty document must intercept the title bar close action.
    click(L+720-19, T+22)
    wait(lambda p: any(color(p, L+350, y) == "f80000" for y in range(T+45, T+165)),
         "unsaved-changes confirmation")
    key("esc")
    wait(lambda p: color(p, L+500, T+300) == DOC, "dismissed confirmation")
    key("ctrl-s")
    time.sleep(.45)
    click(L+720-51, T+22)
    wait(lambda p: color(p, WORK_W-35, 330) == DOC, "maximized editor")
    click(WORK_W-19, 22)
    wait(lambda p: color(p, L+500, T+300) != DOC, "closed editor")
    key("esc")
    guest.until(PROMPT)
    guest.expect(b"cat main.c\r", PROMPT, b"return 42;")
    native["shutdown"](guest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", choices=("rust", "tcc", "both"), default="both")
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--release", default="latest")
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument("--installed", action="store_true",
                        help="test the desktop and Lite XL already installed on the disk")
    args = parser.parse_args()
    release = args.release
    if release == "latest":
        release = subprocess.check_output(["python3", here/"kernel-release.py", "latest"], text=True).strip()
    if not release:
        parser.error("no paired kernel release is available")
    seed_disk = args.disk.resolve(strict=True)
    require_local(seed_disk, seed_disk.stat())
    original_boot = harness["serial"]["boot"]

    def boot(disk, command):
        return original_boot(disk, command, prompt=b"LixinOS Bash (C shell)")

    kinds = ("rust", "tcc") if args.kernel == "both" else (args.kernel,)
    with tempfile.TemporaryDirectory(prefix="lite-xl-") as tmp:
        clipboard = Path(tmp) / "clipboard"
        clipboard.write_bytes(b"")
        for tool, command in (("pbcopy", 'cat > "$LIXINOS_TEST_CLIPBOARD"'),
                              ("pbpaste", ': > "$LIXINOS_TEST_CLIPBOARD.read"\ncat "$LIXINOS_TEST_CLIPBOARD"')):
            stub = Path(tmp) / tool
            stub.write_text("#!/bin/sh\n" + command + "\n")
            stub.chmod(0o700)
        prepared = Path(tmp) / "prepared.img"
        subprocess.run(["cp", "-c", seed_disk, prepared], check=True)
        if not args.installed:
            for tool, package in (("netsurf", "netsurf-port/package"), ("lite-xl", "lite-xl-port/package")):
                subprocess.run(["python3", root/"User/gcc/install.py", "--toolchain", tool,
                                package_root/package, prepared], check=True)
        edit_files(prepared, seed)
        for kind in kinds:
            disk = Path(tmp) / (kind + ".img")
            subprocess.run(["cp", "-c", prepared, disk], check=True)
            image = subprocess.check_output(["python3", here/"kernel-release.py", "boot-files",
                                             release, "retina" if kind == "rust" else "tcc"], text=True).strip()
            binary = Path(image) / ("rust-retina.elf" if kind == "rust" else "tcc-kernel.elf")
            clipboard.write_bytes(b"")
            with patch.dict(os.environ, {"LIXINOS_KERNEL": str(binary),
                                         "LIXINOS_TEST_CLIPBOARD": str(clipboard),
                                         "PATH": f"{tmp}:{os.environ['PATH']}"}), resize_source():
                with patch.dict(harness["serial"], {"boot": boot}):
                    harness["test"](lambda g, c, d: exercise(g, c, d, args.screenshot, clipboard),
                                    mode="--window", disk=disk)
            rows, _ = disk_entries(disk)
            assert bytes(rows["/lite-xl-demo/main.c"][1]).rstrip(b"\n") == EDITED.rstrip(b"\n")
            print(f"{kind}: Lite XL opened, scrolled, edited, saved and resized its GUI", flush=True)


if __name__ == "__main__":
    main()
