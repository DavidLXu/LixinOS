"""Verify launcher scrolling, filtering and window restoration on a disposable image."""
import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import time
from unittest.mock import patch

from disk_image import edit_files
from launcher_grid import installed_apps, select_icon

HERE = Path(__file__).resolve().parent
HARNESS = runpy.run_path(str(HERE / "test-panes.py"))
RESIZE = runpy.run_path(str(HERE / "test-resize.py"))["resize_source"]
SHUTDOWN = runpy.run_path(str(HERE / "test-native-kernel.py"))["shutdown"]
WIDTH, HEIGHT = 1584, 968
W, H = WIDTH//2, (HEIGHT-44)//2


def exercise(guest, control, directory, screenshot):
    assert "QMP" in json.loads(control.readline())

    def qmp(command, **arguments):
        control.write(json.dumps(dict(execute=command, arguments=arguments)).encode()+b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return

    def point(x, y):
        qmp("input-send-event", events=[{"type": "abs", "data": {
            "axis": axis, "value": round(pos*32767/(extent-1))}}
            for axis, pos, extent in (("x", 2*x+1, WIDTH), ("y", 44+2*y+1, HEIGHT))])
        time.sleep(.08)

    def button(name, down):
        qmp("input-send-event", events=[{"type":"btn","data":{"button":name,"down":down}}])
        time.sleep(.08)

    def click(x, y):
        point(x, y)
        button("left", True)
        button("left", False)

    frame = directory / "launcher.ppm"

    def wait(x=0, y=0, color=None):
        end = time.monotonic()+20
        while time.monotonic() < end:
            qmp("screendump", filename=str(frame))
            _, size, _, pixels = frame.read_bytes().split(b"\n",3)
            if size == f"{WIDTH} {HEIGHT}".encode():
                offset = ((44+2*y)*WIDTH+2*x)*3
                if color is None or pixels[offset:offset+3].hex() == color:
                    return
            time.sleep(.1)
        raise AssertionError(f"expected {color} at ({x}, {y})")

    qmp("qmp_capabilities")
    Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text(f"{WIDTH}\t{HEIGHT}\n")
    wait()
    guest.until(b"$ ")
    guest.send(b"ls -1 /bin\r")
    names = installed_apps(guest.until(b"$ ", timeout=60))
    guest.send(b"desktop\r")
    wait(2,2,"281e2b")
    dock_y = 8+7*min(82,(H-48)//8)+20
    click(36,dock_y)
    wait(2,2,"203f51")
    # First row, Files. Sample away from the white glyph and rounded corners.
    cell = (W-48)//5
    x = 24+cell//2-28
    wait(x,142,"e48c32")
    if screenshot:
        subprocess.run(["sips","-s","format","png",frame,"--out",screenshot],check=True)
    point(x,240)
    button("wheel-down",True)
    button("wheel-down",False)
    wait(x,229,"526fc3")  # Second row, Text Editor, after 48 pixels of scroll.
    button("wheel-up",True)
    button("wheel-up",False)
    wait(x,142,"e48c32")
    point(x,322)
    button("left",True)
    point(x,186)
    button("left",False)
    wait(x,142,"526fc3")  # A drag over icons scrolls one row without launching.
    click(120,77)           # System filter resets the scroll offset.
    wait(x,142,"e48c32")
    click(52,77)
    select_icon(click,qmp,W,H,names.index(b"zz-launcher.win")+6,len(names)+6)
    left, top = 76+(W-76-360)//2,(H-246)//2
    wait(left+360-19,top+11,"f1d6cc")
    click(left+360-85,top+22)  # Minimize the launched client.
    click(36,dock_y)
    wait(2,2,"203f51")
    click(W-118,30)
    wait(407,10,"233c3d")
    click(150,60)
    wait(left+360-19,top+11,"f1d6cc")
    click(left+360-19,top+22)
    click(36,dock_y)
    wait(2,2,"203f51")
    guest.send(b"\x1b")
    wait(2,2,"281e2b")
    guest.send(b"\x1b")
    guest.until(b"$ ")
    SHUTDOWN(guest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--desktop", type=Path, required=True)
    parser.add_argument("--release", default="latest")
    parser.add_argument("--kernel", choices=("rust","tcc"), default="rust")
    parser.add_argument("--screenshot", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp)/"launcher.img"
        subprocess.run(["cp","-c",args.disk,disk],check=True)
        def install(rows):
            # Enough installed clients to force several screenfuls on both kernels.
            for i in range(24):
                rows[f"/bin/zz-{i:02}.win".encode()] = rows[b"/bin/nxeyes"]
            rows[b"/bin/zz-launcher.win"] = rows[b"/bin/nxeyes"]
            rows[b"/bin/desktop"] = (rows[b"/bin/desktop"][0],args.desktop.read_bytes())
        edit_files(disk,install)
        with patch.dict(os.environ,{"LIXINOS_TCC_DISK":str(disk)}), RESIZE():
            HARNESS["test"](lambda g,c,d: exercise(g,c,d,args.screenshot), mode="--window",disk=disk,
                            prompt=b"LixinOS Bash (C shell)",
                            launcher_args=("--release",args.release,"--kernel",args.kernel))
    print(f"Passed scrolling, drag, filtering, app launch and window restore on {args.kernel}.")


if __name__ == "__main__":
    main()
