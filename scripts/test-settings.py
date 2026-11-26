"""Open Settings from Apps on disposable desktop disks and check the real consumers."""
from launcher_grid import installed_apps
import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import time

from disk_image import boot_preferences

here = Path(__file__).resolve().parent
panes = runpy.run_path(str(here / "test-panes.py"))


def exercise(guest, control, directory):
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
    prompt = b"$ "
    guest.expect(b"rm -f /.desktop-boot /agent-policy/settings.py\r", prompt)
    guest.expect(b"echo 'export EDITOR=nano  # keep this comment' > /home/lixin/.profile\r", prompt)
    guest.expect(b"echo 'export SAMPLE=unchanged' >> /home/lixin/.profile\r", prompt)
    guest.send(b"ls -1 /bin | cat\r")
    names = installed_apps(guest.until(prompt))
    assert b"settings.win3" in names
    guest.send(b"desktop\r")
    frame = directory / "screen.ppm"
    width, height = 720, 436
    left, body = 266, 145

    def point(x, y):
        qmp("input-send-event", events=[
            dict(type="abs", data=dict(axis=a, value=round(v*32767/(n-1))))
            for a, v, n in (("x", 2*x+1, 2354), ("y", 44+2*y+1, 1408))])
    def button(which, down):
        qmp("input-send-event", events=[dict(type="btn", data=dict(button=which, down=down))])

    def click(x,y):
        point(x,y)
        for down in (True, False):
            button("left",down)
        time.sleep(.15)

    def wait(x, y, color):
        deadline = time.monotonic() + 18
        while time.monotonic() < deadline:
            qmp("screendump", filename=str(frame))
            data = frame.read_bytes().split(b"\n", 3)[3]
            pos = (44+2*y)*(len(data)//1408) + 6*x
            if data[pos:pos+3].hex() == color:
                return
            time.sleep(.05)
        raise AssertionError((x, y, color, data[pos:pos+3].hex(), guest.buffer[-500:]))

    def category(index):
        click(left+84, body+104+44*index)

    def row(index):
        click(left+270, body+146+48*index)

    def back():
        click(left+212, body+29)

    def saved(index=0):
        wait(left+192, body+146+48*index, "b84418")

    def edit():
        wait(left+width-24, body+221, "b84418")

    wait(75, 2, "5b4050")
    guest.send(b"\x07")
    wait(82, 6, "203f51")
    # Select from System so an unrelated installed CLI never shifts the target.
    click(120,77)
    qmp("screendump", filename=str(frame))
    data = frame.read_bytes().split(b"\n", 3)[3]
    for index in range(18):
        x, y = 118+(index%6)*188, 125+(index//6)*136
        pos = (44+2*y)*(len(data)//1408) + 6*x
        if data[pos:pos+3].hex() == "647c8c":
            click(x,y+20)
            break
    else:
        raise AssertionError("Settings icon missing from System")
    saved()
    # Wheel scrolling moves the viewport while preserving the selected row.
    category(1)
    saved()
    point(left+200,body+100)
    button("wheel-down",True)
    button("wheel-down",False)
    wait(left+192,body+146,"f8fcf8")
    button("wheel-up",True)
    button("wheel-up",False)
    saved()
    category(0)
    row(0)
    edit()
    guest.send(b"kilo")
    point(left+width-60,body+221)
    button("left",True)
    point(left+width-130,body+221)
    button("left",False)
    edit()
    guest.send(b"\r")
    saved()
    category(2)
    row(0)
    edit()
    guest.send(b"61\r")
    edit()
    guest.send(b"\x1537\r")
    saved()
    category(3)
    row(0)
    edit()
    guest.send(b"\t \t\r")
    saved()
    row(1)
    edit()
    click(left+234, body+221)
    click(left+width-60, body+221)
    saved(1)
    for index, keys in ((2, b"\t \t\r"), (3, b"2233\r"), (4, b"test-model\r")):
        row(index)
        edit()
        guest.send(keys)
        saved(index)
    category(4)
    row(0)
    back()
    # Maximize the focused settings window and return to its original layout.
    click(left+width-52,body-22)
    wait(76+172+(1177-76-172-700)//2+2,44+146,"b84418")
    click(1177-52,22)
    saved()
    # Shrink the window and navigate through the compact category list.
    point(left+width-3,body+height-3)
    button("left",True)
    point(left+420-3,body+height-3)
    button("left",False)
    wait(left+20,body+146,"b84418")
    click(left+30,body+29)
    wait(left+20,body+146+4*48,"b84418")
    click(left+110,body+146)
    wait(left+20,body+146,"b84418")
    guest.send(b"\x1b")
    time.sleep(.3)
    guest.send(b"\x1b")
    guest.until(prompt)
    guest.expect(b"cat /home/lixin/.profile\r", prompt,
                 b"export EDITOR=kilo  # keep this comment\nexport SAMPLE=unchanged\n")
    guest.expect(b"shell new\r", prompt)
    guest.expect(b"echo $EDITOR $SAMPLE\r", prompt, b"\nkilo unchanged\n")
    guest.expect(b"cat /agent-policy/settings.py\r", prompt, b'print("37 512")')
    guest.expect(b"cat /.desktop-boot\r", prompt, b"split UTC tcc 2233 test-model")
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=20) == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path, help="installed desktop disk with settings.win3")
    parser.add_argument("--kernel", choices=("rust", "tcc"), required=True)
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    release = args.release
    if release == "latest":
        release = subprocess.check_output(["python3", here / "kernel-release.py", "latest"], text=True).strip()
    def verify(guest, control, directory):
        exercise(guest, control, directory)
        assert boot_preferences(disk) == "split UTC tcc 2233 test-model"
        # Inspect the effective QEMU invocation of the normal launcher after shutdown.
        stub = directory / "qemu-system-aarch64"
        stub.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" \"$DEEPSEEK_MODEL\"\n")
        env = dict(os.environ, PATH=f"{directory}:{os.environ['PATH']}",
                   LIXINOS_DISK=str(disk), LIXINOS_QEMU=str(stub), DEEPSEEK_API_KEY="")
        for name in ("DEEPSEEK_MODEL", "LIXINOS_TIMEZONE"):
            env.pop(name, None)
        command = [here / "run-kernel.sh", "--kernel", "rust", "--release", release]
        def arguments(extra, environment):
            return subprocess.check_output([*command, *extra], env=environment, text=True).splitlines()
        env["LIXINOS_GPU"] = "ramfb"
        default = arguments([], env)
        assert "virt,gic-version=3" in default and "name=opt/lixinos/clock-offset,string=+0000" in default
        assert "test-model" in default
        forced = arguments(["--window"], dict(env, LIXINOS_TIMEZONE="Asia/Tokyo", DEEPSEEK_MODEL="override"))
        assert "virt" in forced and "name=opt/lixinos/clock-offset,string=+0900" in forced
        assert "override" in forced

    with tempfile.TemporaryDirectory(dir="/tmp") as temp:
        disk = Path(temp) / "disk.img"
        subprocess.run(["cp", "-c", args.disk.resolve(strict=True), disk], check=True)
        panes["test"](verify, "--window", disk, ("--kernel", args.kernel, "--release", release), prompt=b"$ ")
    print(f"Settings UI, persistence and boot preferences passed on {args.kernel}, {release}.")
