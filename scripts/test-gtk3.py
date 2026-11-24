"""Exercise GTK 3's native window on a disposable LixinOS disk."""
from launcher_grid import installed_apps, select_icon
import argparse
import json
import os
from pathlib import Path
import re
import runpy
import struct
import subprocess
import tempfile
import time
import zlib
from unittest.mock import patch

from disk_image import disk_entries, edit_files, require_local

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
HARNESS = runpy.run_path(str(HERE / "test-panes.py"))
RESIZE = runpy.run_path(str(HERE / "test-resize.py"))["resize_source"]
SHUTDOWN = runpy.run_path(str(HERE / "test-native-kernel.py"))["shutdown"]
WIDTH, HEIGHT = 2024, 1320
LEFT = 76 + (WIDTH // 2 - 76 - 720) // 2
TOP = ((HEIGHT - 44) // 2 - 480) // 2


def color_fixture(rgb):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", 160, 120, 8, 2, 0, 0, 0)
    pixels = zlib.compress((b"\0" + bytes(rgb) * 160) * 120)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", pixels) + chunk(b"IEND", b"")


def exercise(guest, control, directory, screenshot=None, app="gtk3-demo"):
    assert "QMP" in json.loads(control.readline())

    def qmp(command, **arguments):
        control.write(json.dumps({"execute": command, "arguments": arguments}).encode() + b"\n")
        control.flush()
        while True:
            response = json.loads(control.readline())
            if "event" not in response:
                assert "return" in response, response
                return

    def key(name):
        qmp("human-monitor-command", **{"command-line": f"sendkey {name} 40"})

    def move(x, y):
        qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(pixel * 32767 / (extent - 1))}}
            for axis, pixel, extent in (("x", 2*x+1, WIDTH), ("y", 44+2*y+1, HEIGHT))
        ])

    def button(down, name="left"):
        qmp("input-send-event", events=[
            {"type": "btn", "data": {"button": name, "down": down}}
        ])

    def click(x, y, name="left"):
        move(x, y)
        time.sleep(.06)
        for down in (True, False):
            button(down, name)
            time.sleep(.07)

    frame = directory / "gtk3.ppm"

    def sample(data, x, y):
        offset = ((44 + 2*y) * WIDTH + 2*x) * 3
        return data[offset:offset+3].hex()

    def wait(check, description, timeout=14):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            qmp("screendump", filename=str(frame))
            _, dims, _, data = frame.read_bytes().split(b"\n", 3)
            if dims == f"{WIDTH} {HEIGHT}".encode() and check(data):
                return data
            time.sleep(.12)
        if screenshot:
            subprocess.run(["sips", "-s", "format", "png", frame, "--out", screenshot], check=True)
        raise AssertionError(f"Desktop did not show {description}")

    qmp("qmp_capabilities")
    Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text(f"{WIDTH}\t{HEIGHT}\n")
    wait(lambda _: True, "resized console")
    if app == "stalled-client":
        guest.until(b"$ ")
        guest.expect(b"echo 'int main(void) { for (;;) {} }' > stalled.c\r", b"$ ")
        guest.expect(b"tcc stalled.c -o /bin/stalled-client.win3\r", b"$ ")
    if app == "mousepad":
        guest.expect(b"cd /gtk3-test\r", b"lixin:/gtk3-test$ ")
    guest.send(b"ls -1 /bin\r")
    out = re.sub(rb"\x1b\[[0-9;]*m", b"", guest.until(b"$ "))
    binary = f"{app}.win3".encode()
    if binary not in out:
        out += re.sub(rb"\x1b\[[0-9;]*m", b"", guest.until(b"$ "))
    apps = installed_apps(out)
    index = apps.index(binary)
    guest.send(b"desktop\r")
    desktop = wait(lambda p: sample(p, 2, 2) == "281e2b", "desktop")
    title = LEFT+382, TOP+22
    wallpaper = sample(desktop, *title)
    click(36, 8 + 7*min(82, (((HEIGHT-44)//2)-48)//8) + 20)
    wait(lambda p: sample(p, 82, 6) == "203f51", "Apps")
    select_icon(click,qmp,WIDTH//2,(HEIGHT-44)//2,index+6,len(apps)+6)
    if app == "stalled-client":
        wait(lambda p: sample(p, *title) != wallpaper, "stalled client window")
        close = LEFT + 720 - 19, TOP + 22
        click(*close)
        # The client never reads CONFIGURE or replies to CLOSE. The menu must
        # still open and dismiss, and the unacknowledged close must be recoverable.
        click(36, 8 + 7*min(82, (((HEIGHT-44)//2)-48)//8) + 20)
        wait(lambda p: sample(p, 82, 6) == "203f51", "menu while client is stalled")
        click(WIDTH//2-48,30)
        wait(lambda p: sample(p, 82, 6) != "203f51", "dismissed Apps menu")
        time.sleep(2)
        click(*close)
        wait(lambda p: sample(p, *title) != wallpaper and
             sample(p, 10, (HEIGHT-44)//2-20) == "233c3d", "force-close confirmation")
        click(*close)
        wait(lambda p: sample(p, *title) == wallpaper, "stalled window removed")
        key("esc")
        guest.until(b"$ ")
        SHUTDOWN(guest)
        return
    def content(p):
        return tuple(sample(p, x, y) for x in range(LEFT+8, LEFT+485, 3)
                     for y in range(TOP+46, TOP+155, 2))
    wait(lambda p: len(set(content(p))) >= 12, "GTK widgets and text", timeout=45)
    if screenshot:
        subprocess.run(["sips", "-s", "format", "png", frame, "--out", screenshot], check=True)

    def middle(p):
        return tuple(sample(p, x, y) for x in range(LEFT+130, LEFT+590, 4)
                     for y in range(TOP+135, TOP+360, 4))

    base = middle(wait(lambda _: True, "GTK window"))
    close = LEFT+720-19, TOP+22
    if app == "gtk3-demo":
        click(LEFT+50, TOP+104)
        text = content(wait(lambda _: True, "focused text view"))
        key("a")
        wait(lambda p: sum(a != b for a, b in zip(content(p), text)) > 12, "typed text")
        click(LEFT+668, TOP+447)
        wait(lambda p: sum(a != b for a, b in zip(middle(p), base)) > 150, "GTK modal dialog")
        key("esc")
        wait(lambda p: sum(a != b for a, b in zip(middle(p), base)) < 100, "dismissed dialog")
        click(LEFT+720-51, TOP+22)
        wait(lambda p: int(sample(p, WIDTH//2-40, 350)[:2], 16) >= 0xc0, "maximized GTK window")
        close = WIDTH//2-19, 22
    elif app == "mousepad":
        click(LEFT+70, TOP+175)
        key("ctrl-o")
        wait(lambda p: sum(a != b for a, b in zip(middle(p), base)) > 150, "Mousepad file chooser")
        key("ctrl-l")
        time.sleep(.3)
        for ch in "/gtk3-test/main.c":
            key({"/": "slash", "-": "minus", ".": "dot"}.get(ch, ch))
            time.sleep(.08)
        key("ret")
        time.sleep(.35)
        key("ret")
        wait(lambda p: sum(a != b for a, b in zip(middle(p), base)) < 100, "opened source file")
        time.sleep(.4)
        if screenshot:
            wait(lambda _: True, "opened Mousepad document")
            subprocess.run(["sips", "-s", "format", "png", frame, "--out", screenshot], check=True)
        click(LEFT+70, TOP+175)
        key("ctrl-a")
        time.sleep(.15)
        for ch in "updated":
            key(ch)
            time.sleep(.08)
        key("ctrl-s")
        time.sleep(1.2)
    elif app == "nautilus":
        def type_keys(text):
            for ch in text:
                key({"/": "slash", "-": "minus"}.get(ch, ch))
                time.sleep(.07)

        key("ctrl-l")
        time.sleep(.2)
        type_keys("/nautilus-test")
        key("ret")
        wait(lambda p: sum(a != b for a, b in zip(middle(p), base)) > 35, "local directory")
        time.sleep(.6)
        listing = wait(lambda _: True, "directory listing")
        browser = middle(listing)
        click(LEFT+514, TOP+75)
        wait(lambda p: sum(a != b for a, b in zip(middle(p), browser)) > 60, "Nautilus action menu")
        click(LEFT+476, TOP+112)
        wait(lambda p: sum(a != b for a, b in zip(middle(p), browser)) > 100, "new folder dialog")
        type_keys("new-folder")
        key("ret")
        wait(lambda p: sum(a != b for a, b in zip(middle(p), browser)) < 120, "new folder created")
        wait(lambda p: sum(sample(p, x, y) != sample(listing, x, y)
                           for x in range(LEFT+270, LEFT+340, 4)
                           for y in range(TOP+100, TOP+165, 4)) > 20,
             "directory listing with the new folder")
        if screenshot:
            subprocess.run(["sips", "-s", "format", "png", frame, "--out", screenshot], check=True)
    elif app == "tetravex":
        key("ctrl-p")
        wait(lambda p: sum(a != b for a, b in zip(middle(p), base)) > 150, "paused puzzle")
        key("ctrl-p")
        wait(lambda p: sum(a != b for a, b in zip(middle(p), base)) < 100, "resumed puzzle")
        piece = LEFT+327, TOP+172
        target = LEFT+91, TOP+172
        board = wait(lambda _: True, "puzzle board")
        move(*piece)
        time.sleep(.15)
        button(True)
        for x in range(piece[0]-20, target[0], -20):
            move(x, target[1])
            time.sleep(.12)
        move(*target)
        time.sleep(.15)
        button(False)
        wait(lambda p: sample(p, target[0]+15, target[1]) != sample(board, target[0]+15, target[1]),
             "tile dropped into the empty board")
    elif app == "mahjongg":
        def tiles(p):
            return tuple(sample(p, x, y) for x in range(LEFT+55, LEFT+535, 3)
                         for y in range(TOP+95, TOP+445, 3))
        original = tiles(wait(lambda _: True, "Mahjongg tiles"))
        click(LEFT+536, TOP+73)
        wait(lambda p: sum(a != b for a, b in zip(tiles(p), original)) > 32, "highlighted Mahjongg hint")
    elif app == "mines":
        click(LEFT+160, TOP+198, "right")
        wait(lambda p: sum(a != b for a, b in zip(middle(p), base)) > 10, "flagged a mine square")
    elif app == "ristretto":
        key("ctrl-o")
        wait(lambda p: sum(a != b for a, b in zip(middle(p), base)) > 150, "Ristretto file chooser")
        key("ctrl-l")
        time.sleep(.3)
        for ch in "/gtk3-test/red.png":
            key({"/": "slash", "-": "minus", ".": "dot"}.get(ch, ch))
            time.sleep(.08)
        key("ret")
        time.sleep(.35)
        key("ret")
        wait(lambda p: middle(p).count("f80000") > 100, "displayed the red PNG")
        if screenshot:
            subprocess.run(["sips", "-s", "format", "png", frame, "--out", screenshot], check=True)
        key("right")
        wait(lambda p: sum(color.startswith("00") and int(color[2:4], 16) >= 0xe0
                           for color in middle(p)) > 100, "navigated to the green JPEG")
    click(*close)
    wait(lambda p: sample(p, *title) == wallpaper, "closed GTK window", timeout=35)
    key("esc")
    guest.until(b"$ ")
    SHUTDOWN(guest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--release", default="latest")
    parser.add_argument("--kernel", choices=("rust", "tcc"), default="rust")
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument("--desktop", type=Path, help="desktop executable to install on the test copy")
    extra = ("tetravex", "mahjongg", "mines", "ristretto")
    parser.add_argument("--app", choices=("gtk3-demo", "mousepad", "nautilus", "stalled-client", *extra), default="gtk3-demo")
    args = parser.parse_args()
    source = args.disk.resolve(strict=True)
    require_local(source, source.stat())
    with tempfile.TemporaryDirectory(prefix="gtk3-") as tmp:
        disk = Path(tmp) / "test.img"
        subprocess.run(["cp", "-c", source, disk], check=True)
        build = ROOT/"Kernel/target/user-build/gtk3-port"
        name = args.app if args.app in (*extra, "nautilus") else "gtk3"
        if args.app == "nautilus":
            package = build/"nautilus-port/package"
        else:
            package = build/f"extra/{name}-package" if args.app in extra else build/"package"
        subprocess.run(["python3", ROOT/"User/gcc/install.py", "--toolchain", name,
                        package, disk], check=True)
        if args.desktop:
            edit_files(disk, lambda rows: rows.update({
                b"/bin/desktop": (rows[b"/bin/desktop"][0], args.desktop.read_bytes())
            }))
        if args.app == "mousepad":
            edit_files(disk, lambda rows: rows.update({
                b"/gtk3-test": (bytes(8), None),
                b"/gtk3-test/main.c": (bytes(8), b"int main(void) { return 0; }\n")
            }))
        if args.app == "nautilus":
            edit_files(disk, lambda rows: rows.update({
                b"/nautilus-test": (bytes(8), None),
                b"/nautilus-test/sample.txt": (bytes(8), b"Portable file browser\n"),
            }))
        if args.app == "ristretto":
            image = Path(tmp) / "second.png"
            jpeg = Path(tmp) / "second.jpg"
            image.write_bytes(color_fixture((0, 255, 0)))
            subprocess.run(["sips", "-s", "format", "jpeg", image, "--out", jpeg], check=True)
            edit_files(disk, lambda rows: rows.update({
                b"/gtk3-test": (bytes(8), None),
                b"/gtk3-test/red.png": (bytes(8), color_fixture((255, 0, 0))),
                b"/gtk3-test/second.jpg": (bytes(8), jpeg.read_bytes()),
            }))
        with patch.dict(os.environ, {"LIXINOS_TCC_DISK": str(disk)}), RESIZE():
            HARNESS["test"](lambda g, c, d: exercise(g, c, d, args.screenshot, args.app),
                            mode="--window", disk=disk, prompt=b"LixinOS Bash (C shell)",
                            launcher_args=("--release", args.release, "--kernel", args.kernel))
        if args.app == "mousepad":
            rows, _ = disk_entries(disk)
            assert bytes(rows["/gtk3-test/main.c"][1]).rstrip(b"\n") == b"updated"
        if args.app == "nautilus":
            rows, _ = disk_entries(disk)
            assert rows["/nautilus-test/new-folder"][1] is None
    print(f"{args.kernel}: {args.app} passed its window and dialog test")


if __name__ == "__main__":
    main()
