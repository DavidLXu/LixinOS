"""Exercise the Files window on a disposable copy of an installed desktop disk."""
import argparse
import json
import struct
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import time

here = Path(__file__).resolve().parent
from disk_image import disk_entries, edit_files

panes = runpy.run_path(str(here / "test-panes.py"))


# Use distinct metadata orders so an accidental name sort cannot pass.
samples = [(f"item-{i:02d}.{('txt', 'bin', 'csv')[i % 3]}", (7*i) % 13,
            1704067200 + 86400*((5*i) % 13)) for i in range(13)]


def install_sort_fixture(rows):
    stamp = struct.pack("<II", 1704067200, 1704067200)
    rows[b"/sort-qa"] = stamp, None
    for name, size, modified in samples:
        rows[("/sort-qa/" + name).encode()] = struct.pack("<II", 1704067200, modified), bytes(size)


def exercise(guest, control, directory, screenshot, rebooted=False):
    def qmp(command, **args):
        control.write(json.dumps(dict(execute=command, arguments=args)).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return

    assert "QMP" in json.loads(control.readline())
    qmp("qmp_capabilities")
    prompt = panes["serial"]["SHELL"]
    guest.expect(b"cd /\r", prompt)
    if not rebooted:
        guest.expect(b"mkdir -p /files-qa/source/nested /files-qa/dest\r", prompt)
        guest.expect(b"echo original > /files-qa/source/nested/content\r", prompt)
        guest.expect(b"mkdir -p /nav-qa/work/deep\r",prompt)
        guest.expect(b"mkdir -p /batch-qa/from /batch-qa/copy /batch-qa/moved /batch-qa/conflict\r",prompt)
        for ch in "abcd":
            guest.expect(f"echo {ch} > /batch-qa/from/grp{ch}.txt\r".encode(),prompt)
        guest.expect(b"echo keep > /batch-qa/conflict/grpb.txt\r",prompt)
    guest.send(b"desktop\r")
    frame = directory / "screen.ppm"
    left, top = 326, 145

    def color(x, y):
        qmp("screendump", filename=str(frame))
        data = frame.read_bytes().split(b"\n", 3)[3]
        start = (44 + y*2)*(len(data)//1408) + x*6
        return data[start:start+3].hex()

    def wait(x, y, expected):
        end = time.monotonic() + 10
        while time.monotonic() < end:
            observed = color(x, y)
            if observed == expected:
                return
            time.sleep(.10)
        raise AssertionError((x, y, expected, observed))

    def point(x, y):
        qmp("input-send-event", events=[
            dict(type="abs", data=dict(axis=a, value=round(v*32767/(n-1))))
            for a, v, n in (("x", 2*x+1, 2354), ("y", 44+2*y+1, 1408))])
    def button(which):
        for down in (True, False):
            qmp("input-send-event", events=[dict(type="btn", data=dict(button=which, down=down))])
            if down and which in ("left","right"): time.sleep(.09)

    def click(x, y):
        point(x, y)
        button("left")
        time.sleep(.09)

    def keys(sequence):
        guest.send(sequence)
        time.sleep(.65 if b"\r" in sequence else .25)

    def location(path):
        # Wait for the location dialog and its dismissal before sending pointer
        # events. A filesystem commit may still be running on the guest.
        x,y=left+173,top+226
        keys(b"\x0c")
        wait(x,y,"d4e4d9")
        keys(b"\x15"+path.encode()+b"\r")
        deadline=time.monotonic()+10
        while color(x,y)=="d4e4d9":
            time.sleep(.08)
            assert time.monotonic()<deadline, ("location dialog",path)

    def click_until(x,y,check_x,check_y,expected):
        for attempt in range(3):
            click(x,y)
            end=time.monotonic()+1.5
            while time.monotonic()<end:
                if color(check_x,check_y)==expected: return
                time.sleep(.08)
        raise AssertionError(("click",x,y,expected,color(check_x,check_y)))

    def action(index):
        click_until(left+35,top+370,left+25,top+53,"e8eee8")
        click(left+28+(index % 3)*192, top+62+(index//3)*30)

    def select(index):
        click_until(left+170,top+93+index*30,left+420,top+81+index*30,"cde5df")

    wait(75, 2, "5b4050")
    click(38,35)
    wait(left+5, top+52, "ecf1eb")
    if rebooted:
        location("/Trash")
        keys(b"\x06grpd.txt\r")
        select(0)
        action(15)
        location("/batch-qa/from")
        select(0)
        keys(b"\r")
        wait(6,6,"233b43")
        keys(b"\x1b[F" + "。".encode() + b"\x13\x14")
        wait(6,6,"f3f5ef")
        # Save with the basename preselected after jumping to a pinned folder.
        click(62,97)
        keys("nav-你好.txt".encode()+b"\r")
        wait(6,6,"233b43")
        keys(b"\x0f")
        wait(6,6,"f3f5ef")
        click(110,26)  # Root breadcrumb.
        click(62,97)   # The same favorite in the Open dialog.
        keys("nav-你好.txt".encode()+b"\r")
        wait(6,6,"233b43")
        keys(b"\x1b[F!\x13\x1b")
        wait(left+5,top+52,"ecf1eb")
        keys(b"\x1b")
        guest.until(prompt)
        guest.expect(b"cat /batch-qa/from/grpd.txt\r", prompt, b"\nd\n")
        guest.expect(b"cat /batch-qa/from/grpa.txt\r", prompt, "明天十点半开会。".encode())
        guest.send(b"q\r")
        assert guest.vm.wait(timeout=15) == 0
        return
    location("/sort-qa")
    point(left+125, top+202)

    def visible_names():
        qmp("screendump", filename=str(frame))
        data = frame.read_bytes().split(b"\n", 3)[3]
        stride = len(data)//1408
        names = []
        for row in range(9):
            x = (left+112+42)*6
            y = 44 + 2*(top+83+30*row)
            names.append(b"".join(data[line*stride+x:line*stride+x+560]
                                  for line in range(y, y+44, 2)))
        return names

    def wheel(direction):
        button("wheel-" + direction)
        time.sleep(.12)

    glyphs = visible_names()
    assert len(set(glyphs)) == 9
    for _ in range(4):
        wheel("down")
        after = visible_names()
        assert after[:-1] == glyphs[-8:], "wheel must advance exactly one row"
        glyphs.append(after[-1])
    wheel("down")
    assert visible_names() == glyphs[-9:], "stop at the end"
    wheel("up")
    assert visible_names() == glyphs[-10:-1], "scroll back one row"
    select(2)
    point(left+28,top+30)
    time.sleep(.15)
    saved=visible_names()
    location("/nav-qa/work/deep")
    click(left+143,top+22)  # The work component, between nav-qa and deep.
    wait(left+128,top+89,"bc873f")  # The deep folder in work.
    favorite_y=top+80+5*30+12
    for color_value in ("d4e4d9","ecf1eb","d4e4d9"):
        keys(b"\x04")
        wait(left+9,favorite_y,color_value)
    click_until(left+100,favorite_y,left+9,favorite_y,"ecf1eb")
    click_until(left+76,top+60,left+9,favorite_y,"d4e4d9")
    keys(b"\x0eby-crumb\r")
    wait(left+128,top+119,"bc873f")  # Both directories are visible after commit.
    keys(b"\x1b[D")
    keys(b"\x1b[D")
    wait(left+420,top+141,"cde5df")
    assert visible_names()==saved, "Back must restore scroll position and selection"
    location("/files-qa")
    location("/sort-qa")
    assert len(set(glyphs)) == len(samples)
    by_name = dict(zip((name for name, _, _ in samples), glyphs))
    for x, order in [
        (left+112+30, lambda e: e[0]),
        (left+112+235, lambda e: (e[0].split(".")[1], e[0])),
        (left+112+311, lambda e: e[1]),
        (left+112+404, lambda e: e[2]),
    ]:
        # Name is already the active ascending column on first open.
        for reverse in ((True, False) if x == left+142 else (False, True)):
            click(x, top+60)
            expected = sorted(samples, key=order, reverse=reverse)
            assert visible_names() == [by_name[e[0]] for e in expected[:9]], (x, reverse)
    if screenshot:
        qmp("screendump", filename=str(frame))
        magic, dims, depth, data = frame.read_bytes().split(b"\n", 3)
        stride = len(data)//1408
        frame.write_bytes(b"\n".join((magic, dims, depth, b"".join(
            data[y*stride:y*stride+2354*3] for y in range(1408)))))
        subprocess.run(["sips", "-s", "format", "png", frame, "--out", screenshot], check=True)
    click(left+142, top+60)  # restore name order for the operations below
    location("/files-qa")
    # Open the nested source by double-click, then revisit its parent through history.
    click(left+170, top+123)
    click(left+170, top+123)
    select(0)
    keys(b"\x1b[D")
    select(1)  # source, after dest
    keys(b"\x03")
    location("/files-qa/dest")
    keys(b"\x16")
    # Refuse a second paste into the occupied destination.
    keys(b"\x16")
    select(0)
    keys(b"\x12\x15renamed\r")
    keys(b"\x0enew-folder\r")
    action(2)
    keys(b"notes.txt\r")
    keys(b"\x06notes\r")
    select(0)
    action(7)
    location("/Trash")
    keys(b"\x06notes.txt\r")
    select(0)
    keys(b"\x18")
    location("/files-qa")
    keys(b"\x16")
    select(1)
    # Choose the graphical editor and write through its Save As and Open pickers.
    action(18)
    wait(6,6,"233b43")
    keys(b"widget-save")
    keys(b"\x14")
    wait(6,6,"f3f5ef")
    keys(b"/files-qa/chooser.txt\r")
    wait(6,6,"233b43")
    keys(b"\x0f")
    wait(6,6,"f3f5ef")
    keys(b"/files-qa/notes.txt\r")
    wait(6,6,"233b43")
    keys(b"opened-original\x13\x1b")
    wait(left+5,top+52,"ecf1eb")
    # The next ordinary file now opens with the selected application.
    select(2)
    keys(b"\r")
    wait(6,6,"233b43")
    keys(b"\x1b")
    wait(left+5,top+52,"ecf1eb")
    def modified_click(modifier,index):
        def modifier_key(down):
            qmp("input-send-event",events=[dict(type="key",data=dict(down=down,
                key=dict(type="qcode",data=modifier)))])
        modifier_key(True)
        time.sleep(.12)
        click(left+170,top+93+index*30)
        modifier_key(False)
        time.sleep(.1)

    location("/batch-qa/from")
    select(0)
    modified_click("shift",2)
    for row in range(3): wait(left+420,top+81+30*row,"cde5df")
    keys(b"\x18")
    location("/batch-qa/moved")
    keys(b"\x16\x1a")
    location("/batch-qa/from")
    keys(b"\x01\x03")
    location("/batch-qa/copy")
    keys(b"\x16")
    location("/batch-qa/conflict")
    keys(b"\x16")
    location("/batch-qa/from")
    select(0)
    modified_click("ctrl",2)
    wait(left+420,top+81,"cde5df")
    wait(left+420,top+141,"cde5df")
    point(left+170,top+153)
    button("right")
    time.sleep(.15)
    click(left+28+192,top+62+2*30)  # Trash through the row context menu.
    action(2)
    keys(b"grpa.txt\r")
    location("/Trash")
    keys(b"\x06grp\r\x01")
    action(15)
    keys(b"recovered.txt\r")
    location("/batch-qa/from")
    select(1)
    keys(b"\x12renamed.txt\r\x1a")
    # Restart the desktop and use its persisted text-file association.
    keys(b"\x1b")
    guest.until(prompt)
    guest.send(b"desktop\r")
    wait(75,2,"5b4050")
    click(38,35)
    location("/batch-qa/from")
    select(0)
    keys(b"\r")
    wait(6,6,"233b43")
    keys("明天十点开会".encode())
    keys(b"\x1b[D\x1b[D")
    keys("半".encode()+b"\x7f"+"半".encode())
    keys(b"\x13\x1b")
    wait(left+5,top+52,"ecf1eb")
    location("/batch-qa/from")
    select(3)
    action(7)
    keys(b"\x1b")
    guest.until(prompt)
    guest.expect(b"cat /batch-qa/from/grpa.txt\r",prompt,"明天十点半开会".encode())
    guest.expect(b"cat /batch-qa/from/recovered.txt /batch-qa/from/grpb.txt /batch-qa/from/grpc.txt\r",prompt,b"\na\nb\nc\n")
    guest.expect(b"ls -1 /batch-qa/copy | cat\r",prompt,b"grpa.txt\ngrpb.txt\ngrpc.txt\ngrpd.txt")
    guest.expect(b"ls -1 /batch-qa/conflict | wc -l\r",prompt,b"1")
    guest.expect(b"cat /batch-qa/conflict/grpb.txt\r",prompt,b"keep")
    guest.expect(b"cat /files-qa/chooser.txt\r", prompt, b"widget-save")
    guest.expect(b"cat /files-qa/notes.txt\r", prompt, b"opened-original")
    for path in ("source/nested/content", "dest/renamed/nested/content"):
        guest.expect(f"cat /files-qa/{path}\r".encode(), prompt, b"\noriginal\n")
    guest.expect(b"ls -1 /files-qa/dest | cat\r", prompt, b"\nnew-folder\nrenamed\n")
    guest.expect(b"ls -1 /files-qa | cat\r", prompt, b"\nchooser.txt\ndest\nnotes.txt\nsource\n")
    guest.send(b"q\r")
    assert guest.vm.wait(timeout=15) == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path)
    parser.add_argument("--kernel", choices=("rust", "tcc"), default="rust")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--screenshot", type=Path)
    args = parser.parse_args()
    os.environ["LIXINOS_TEST_SEED"] = str(args.disk.resolve(strict=True))
    boot = panes["serial"]["boot"]
    def boot_files(disk, cmd):
        if not disk.exists():
            panes["serial"]["seed_test_disk"](disk)
            edit_files(disk, install_sort_fixture)
        return boot(disk, [*cmd, "--kernel", args.kernel, "--release", args.release], prompt=b"$ ")
    panes["serial"]["boot"] = boot_files
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp) / "files.img"
        for rebooted in (False, True):
            panes["test"](lambda g,c,d: exercise(g,c,d,args.screenshot,rebooted), "--window", disk=disk)
        entries,_=disk_entries(disk)
        assert entries["/nav-qa/work/by-crumb"][1] is None
        assert bytes(entries["/nav-qa/work/nav-你好.txt"][1])=="明天十点半开会。!".encode()
    print(f"Files operations passed on {args.kernel}")
