"""Build a graphical LXE1 in the guest, click it and run it again after reboot."""
import argparse
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import time

harness = runpy.run_path(str(Path(__file__).with_name("test-panes.py")))


def exercise(guest, control, directory, build, screenshots):
    def qmp(name, **arguments):
        control.write(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return reply["return"]

    assert "QMP" in json.loads(control.readline())
    qmp("qmp_capabilities")
    prompt = harness["serial"]["SHELL"]
    command = b"/bin/button-demo"
    if build:
        guest.expect(b"tcc /src/gui/button.c -o " + command + b"\r", prompt)
    guest.expect(b"ls /bin/button-demo\r", prompt, b"button-demo")
    guest.expect(command + b" | cat\r", prompt, b"requires a foreground QEMU canvas")
    guest.send(command + b"\r")

    frame = directory / "button.ppm"
    width, height = 2354, 1408
    x, y = (width // 2 - 144) // 2, ((height - 44) // 2 - 94) // 2
    pixels = [(x + 8, y + 8), (x + 15, y + 85)]

    def wait_colors(expected, screenshot=None):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            qmp("screendump", filename=str(frame))
            _, size, _, rgb = frame.read_bytes().split(b"\n", 3)
            assert size == f"{width} {height}".encode(), size
            stride = len(rgb) // height
            colors = []
            for px, py in pixels:
                offset = (44 + 2 * py) * stride + 6 * px
                colors.append(rgb[offset:offset + 3].hex())
            if colors == expected:
                if screenshot and screenshots:
                    screenshots.mkdir(parents=True, exist_ok=True)
                    subprocess.run(["sips", "-s", "format", "png", frame, "--out",
                                    screenshots / screenshot], check=True, stdout=subprocess.DEVNULL)
                return
            time.sleep(0.05)
        raise AssertionError(f"Expected {expected}, saw {colors}")

    wait_colors(["246c71", "364958"], "button-before.png" if build else None)
    qmp("input-send-event", events=[
        {"type": "abs", "data": {"axis": axis, "value": round(position * 32767 / (limit - 1))}}
        for axis, position, limit in (("x", 2 * x + 40, width), ("y", 44 + 2 * y + 40, height))
    ])
    for down in (True, False):
        qmp("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": down}}])
    wait_colors(["a05228", "eab775"], "button-clicked.png" if build else "button-after-reboot.png")
    qmp("human-monitor-command", **{"command-line": "sendkey esc"})
    guest.until(prompt)
    guest.expect(b"echo console-restored\r", prompt, b"\nconsole-restored\n")
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=10) == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--screenshots", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as folder:
        disk = Path(folder) / "disk.img"
        for build in (True, False):
            harness["test"](lambda guest, control, directory: exercise(
                guest, control, directory, build, args.screenshots), mode="--window", disk=disk)
            print("Built and clicked in LixinOS." if build else "Rebooted and clicked the saved app.", flush=True)
