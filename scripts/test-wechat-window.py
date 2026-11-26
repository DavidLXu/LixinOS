"""Check a Tencent login QR and the settings button in the QEMU framebuffer."""
import argparse
import os
from pathlib import Path
import runpy
import select
import subprocess
import tempfile
import time
from urllib.parse import urlsplit

import cv2
import numpy as np

HERE = Path(__file__).resolve().parent
MUTTER = runpy.run_path(str(HERE / "test-mutter.py"))


def check(image, release, kernel):
    def exercise(guest, control, tmp):
        qmp = MUTTER["qmp_connection"](control)
        qmp("qmp_capabilities")
        width, height = 1600, 1200
        Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text(f"{width}\t{height}\n")
        guest.until(b"$ ")
        guest.expect(b"cd /\r", MUTTER["GUEST"]["shell"])
        guest.send(b"wechat\r")
        frame = tmp / "screen.ppm"
        detector = cv2.QRCodeDetector()
        transcript = bytearray()

        def screenshot():
            if select.select([guest.vm.stdout], [], [], 0)[0]:
                transcript.extend(os.read(guest.vm.stdout.fileno(), 65536))
                if b"[exit " in transcript or b"guest stopped" in transcript:
                    raise AssertionError(transcript[-1500:].decode(errors="replace"))
                del transcript[:-8192]
            qmp("screendump", filename=str(frame))
            _, size, _, pixels = frame.read_bytes().split(b"\n", 3)
            w, h = map(int, size.split())
            # The TCC ramfb can align every PPM row to four bytes.
            rgb = np.frombuffer(pixels, dtype=np.uint8).reshape(h, -1)[:, :3*w].reshape(h, w, 3)
            return cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)

        def click(x, y):
            qmp("input-send-event", events=[
                {"type": "abs", "data": {"axis": axis, "value": round(v * 32767 / (limit - 1))}}
                for axis, v, limit in (("x", x, width), ("y", y, height))])
            time.sleep(.2)
            for down in (True, False):
                qmp("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": down}}])
                time.sleep(.15)

        deadline = time.monotonic() + (420 if kernel == "tcc" else 150)
        while time.monotonic() < deadline:
            screen = screenshot()
            # The client is centered on this monitor. Decode locally without printing a login token.
            code, corners, _ = detector.detectAndDecode(screen)
            if corners is not None and urlsplit(code).hostname == "weixin.qq.com":
                break
            time.sleep(.5)
        else:
            raise AssertionError(f"No Tencent login QR on {kernel}: {transcript[-1500:]!r}")
        print(f"{kernel}: official WeChat login QR decoded from QEMU", flush=True)
        height, width = screen.shape[:2]
        center_x, center_y = corners[0].mean(axis=0)
        # The gear and Save button stay at fixed offsets inside the Qt login form.
        scale = float(corners[0][1][0] - corners[0][0][0]) / 271
        click(round(center_x + 146*scale), round(center_y - 264*scale))
        deadline = time.monotonic() + 18
        while time.monotonic() < deadline:
            screen = screenshot()
            # The settings form paints a solid green Save button; the login view has a text link.
            region = screen[round(center_y + 338*scale):round(center_y + 398*scale),
                            round(center_x - 92*scale):round(center_x + 88*scale)]
            blue, green, red = cv2.split(region)
            if np.count_nonzero((green > 135) & (red < 65) & (blue < 160)) > 1500 * scale**2:
                print(f"{kernel}: mouse opened the network settings form", flush=True)
                return
            time.sleep(.25)
        raise AssertionError(f"{kernel}: the settings form did not appear")

    with tempfile.TemporaryDirectory(prefix="lixinos-wechat-window-") as tmp:
        disk = Path(tmp) / "wechat.img"
        subprocess.run(["cp", "-c", image, disk], check=True)
        MUTTER["with_window"](disk, release, kernel, exercise)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--kernel", choices=("rust", "tcc", "both"), default="rust")
    parser.add_argument("--release", default="kernel-2026-11-25.7")
    args = parser.parse_args()
    for kernel in (("rust", "tcc") if args.kernel == "both" else (args.kernel,)):
        check(args.image, args.release, kernel)
