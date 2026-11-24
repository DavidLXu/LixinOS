"""Play, pause, seek and replay the bundled H.264 clip in the actual desktop."""
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

HERE = Path(__file__).resolve().parent
HARNESS = runpy.run_path(str(HERE / "test-panes.py"))
RESIZE = runpy.run_path(str(HERE / "test-resize.py"))["resize_source"]
W, H = 2024, 1320
LEFT = 76 + (W // 2 - 76 - 720) // 2
TOP = ((H - 44) // 2 - 480) // 2


def exercise(guest, channel, folder):
    assert "QMP" in json.loads(channel.readline())

    def qmp(command, **args):
        channel.write(json.dumps({"execute": command, "arguments": args}).encode() + b"\n")
        channel.flush()
        while True:
            reply = json.loads(channel.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return

    def click(x, y):
        events = [{"type": "abs", "data": {"axis": axis, "value": round(v * 32767 / (limit - 1))}}
                  for axis, v, limit in (("x", 2*x + 1, W), ("y", 44 + 2*y + 1, H))]
        qmp("input-send-event", events=events)
        for down in (True, False):
            qmp("input-send-event", events=[{"type":"btn","data":{"button":"left","down":down}}])
            time.sleep(.09)

    def key(name):
        qmp("human-monitor-command", **{"command-line": "sendkey " + name + " 40"})
        time.sleep(.09)

    frame = folder / "frame.ppm"

    def wait(check, title, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            qmp("screendump", filename=str(frame))
            _, size, _, pixels = frame.read_bytes().split(b"\n", 3)
            if size == f"{W} {H}".encode():
                result = check(pixels)
                if result:
                    return result
            time.sleep(.12)
        raise AssertionError(f"Timed out waiting for {title}")

    def pixel(p, x, y):
        start = ((44+2*y)*W+2*x)*3
        return p[start:start+3]

    # The original demo moves a red-orange balloon from left to right over blue hills.
    def balloon(p):
        xs = []
        for y in range(TOP+120,TOP+310,5):
            for x in range(LEFT+10,LEFT+710,5):
                r,g,b = pixel(p,x,y)
                if r>155 and r>g*2 and g<120 and b<100:
                    xs.append(x)
        return sum(xs)//len(xs) if len(xs)>30 else 0

    qmp("qmp_capabilities")
    Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text(f"{W}\t{H}\n")
    wait(lambda p: True, "screen resize")
    guest.until(b"$ ")
    # A corrupt file distinguishes the Files argument from the default demo.
    guest.send(b"echo invalid > '/bad clip.MP4'\r")
    guest.until(b"$ ", timeout=120)
    guest.send(b"ls -1 /bin\r")
    apps = installed_apps(guest.until(b"$ "))
    guest.send(b"desktop\r")
    wait(lambda p: pixel(p,2,2)==bytes.fromhex("281e2b"), "desktop")
    click(36, 8 + 7*min(82, (((H-44)//2)-48)//8) + 20)
    wait(lambda p: pixel(p,82,6)==bytes.fromhex("203f51"), "Apps")
    select_icon(click,qmp,W//2,(H-44)//2,apps.index(b"video.win3")+6,len(apps)+6)
    first = wait(balloon,"first decoded frame")
    wait(lambda p: balloon(p)>first+12,"advancing H.264 frames")
    key("spc")
    time.sleep(.35)
    paused = wait(balloon,"pause")
    time.sleep(.6)
    assert wait(balloon,"paused frame")==paused
    key("right")
    ahead = wait(lambda p: balloon(p) if balloon(p)>paused+80 else 0, "five-second seek")
    key("left")
    wait(lambda p: 0<balloon(p)<ahead-80, "backward seek")
    click(LEFT+708,TOP+480-40)
    key("spc")
    time.sleep(1.2)
    key("spc")
    wait(lambda p: 0<balloon(p)<ahead-50, "replay from the beginning")
    click(LEFT+720-19,TOP+22)
    time.sleep(.4)
    click(36,30)
    key("ctrl-f")
    for ch in "bad clip":
        key("spc" if ch==" " else ch)
    key("ret")
    file_top = ((H-44)//2-391)//2
    click(500,file_top+95)
    click(500,file_top+95)
    def error_frame(p):
        return (not balloon(p) and sum(1 for y in range(TOP+450,TOP+476,2)
                for x in range(LEFT+75,LEFT+300,2)
                if pixel(p,x,y)[0]>235 and 145<pixel(p,x,y)[1]<210 and pixel(p,x,y)[2]<120)>8)
    wait(error_frame,"file association and decoder error for a corrupt .MP4")
    key("ctrl-o")
    for ch in "/lib/video/demo.mp4":
        key({"/":"slash",".":"dot"}.get(ch,ch))
    key("ret")
    wait(balloon,"opening a valid file after a decoder error")
    click(LEFT+720-51,TOP+22)
    # A maximized window has its path field near the top of the workspace.
    wait(lambda p: pixel(p,230,55)==bytes.fromhex("f8fcf8"), "resized player")
    click(W//2-19,22)
    time.sleep(.3)
    file_left = 76+(W//2-76-600)//2
    click(file_left+600-19,file_top+22)
    time.sleep(.3)
    key("esc")
    guest.until(b"$ ")
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=90)==0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk",required=True,type=Path)
    parser.add_argument("--kernel",choices=("rust","tcc"),required=True)
    parser.add_argument("--release",default="latest")
    args=parser.parse_args()
    original=HARNESS["serial"]["boot"]
    def boot(disk,command):
        return original(disk,command+["--release",args.release,"--kernel",args.kernel],
                        prompt=b"LixinOS Bash (C shell)")
    with tempfile.TemporaryDirectory(prefix="video-test-") as tmp:
        disk=Path(tmp)/"test.img"
        subprocess.run(["cp","-c",args.disk,disk],check=True)
        with RESIZE(),patch.dict(HARNESS["serial"],{"boot":boot}):
            HARNESS["test"](exercise,mode="--window",disk=disk)
    print(f"{args.kernel}: H.264 desktop playback, pause, seek and replay passed")


if __name__=="__main__":
    main()
