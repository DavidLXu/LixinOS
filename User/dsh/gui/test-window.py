"""Guest side of the integration test. Drive the same Win3 wire protocol as Desktop."""

import json
import os
from pathlib import Path
import struct
import subprocess
import time

turns = Path(os.environ["DSH_HOME"]) / "lixinos-gui/turns"


class Window:
    def __init__(self):
        self.child = subprocess.Popen(
            ["/bin/dsh-gui.win3"], stdin=subprocess.PIPE, stdout=subprocess.PIPE
        )
        self.frame = None
        self.opened = []
        self.event(2, 720, 436, 16)  # Desktop advertises URL opening.
        for _ in range(4):
            self.event(3)
        assert self.frame

    def event(self, kind, x=0, y=0, value=0, extra=b""):
        self.child.stdin.write(struct.pack("<4h", kind, x, y, value) + extra)
        self.child.stdin.flush()
        head = self.child.stdout.read(16)
        if len(head) != 16:
            raise AssertionError(("GUI exited", self.child.poll(), head))
        _, _, w, h, _, clipboard, compressed = struct.unpack("<6HI", head)
        open_url = compressed & (1 << 27)
        compressed &= ~(1 << 27)
        payload = self.child.stdout.read(compressed or w * h * 2)
        trailer = self.child.stdout.read(clipboard)
        if open_url:
            self.opened.append(trailer.decode())
        if w and h:
            self.frame = (w, h, compressed, payload)

    def click(self, x, y):
        for value in (0, 1, 0):
            self.event(0, x, y, value)
        self.event(3)

    def capture(self, name):
        w, h, compressed, pixels = self.frame
        Path("/" + name + ".rgb").write_bytes(
            struct.pack("<3I", w, h, compressed) + pixels
        )

    def paste(self, text):
        data = text.encode()
        self.event(6, value=len(data), extra=data)
        self.event(3)

    def submit(self, text, number, stop=False):
        self.click(390, 326)
        self.paste(text)
        self.event(1, value=19)  # Ctrl+S
        self.event(3)
        if stop:
            self.click(670, 380)  # Stop, while the engine is starting
        until = time.monotonic() + 150
        while time.monotonic() < until:
            self.event(3)
            files = sorted(turns.glob("*.json"))
            if len(files) == number:
                data = json.loads(files[-1].read_text())
                if data.get("closed"):
                    print(
                        "turn",
                        number,
                        [(e["type"], e.get("tool", "")) for e in data["events"]],
                        flush=True,
                    )
                    errors = [
                        e["message"] for e in data["events"] if e["type"] == "error"
                    ]
                    assert not errors, errors
                    return data
            time.sleep(0.07)
        self.capture("timeout")
        raise AssertionError(("turn timeout", list(turns.glob("*"))))

    def close(self):
        self.child.stdin.close()
        self.child.stdout.close()
        self.child.wait(timeout=10)


window = Window()
window.capture("welcome")
first = window.submit("Read witness.txt and update result.txt.", 1)
assert first["events"][-1]["type"] == "final"
window.click(678, 20)
window.capture("trajectory")
window.click(176, 77)  # first timeline segment
for _ in range(8):
    time.sleep(0.06)
    window.event(3)
window.capture("first-event")
window.close()
window = Window()
window.click(76, 127)  # most recent session
second = window.submit("Resume and run project test.", 2)
assert second["session"] == first["session"] and second["thread"] == first["thread"]
assert second["events"][-1]["text"].startswith("passed-")
window.capture("chat")
cancelled = window.submit("Stop this turn.", 3, stop=True)
assert any(e["type"] == "stop_accepted" for e in cancelled["events"])
resumed = window.submit("Resume and run project test.", 4)
assert resumed["session"] == first["session"] and resumed["events"][-1][
    "text"
].startswith("passed-")
window.close()
sample = turns / "zz-markdown.json"
sample.write_text(json.dumps({
    "thread": "markup", "session": "markup", "title": "Markdown", "cwd": "/work",
    "prompt": "Sources", "closed": True,
    "events": [{"type": "final", "text":
        "## Sources\n\n| Source | Format |\n| --- | --- |\n"
        "| [**原始来源**][one] | `primary` |\n\nhttps://example.org/second\n\n"
        "`https://example.org/code`\n\n[one]: https://example.org/first?q=1&amp;lang=zh"}],
}))
window = Window()
window.event(2, 900, 600, 16)
window.click(72, 140)
for _ in range(5):
    window.event(3)
window.capture("markdown")
w, h, compressed, pixels = window.frame
if compressed:
    pixels = b"".join(struct.pack("<H", color) * n for n, color in struct.iter_unpack("<HH", pixels))
# Click inside the long, continuous underline of each link, clear of glyph edges.
blue = struct.pack("<H", (0x24 >> 3) << 11 | (0x5b >> 2) << 5 | (0xc2 >> 3))
last_y = 0
for y in range(50, h - 130):
    row = pixels[2 * (y * w + 165):2 * ((y + 1) * w)]
    x = row.find(blue * 24)
    if x >= 0 and not x % 2 and y - last_y > 6:
        before = len(window.opened)
        window.click(177 + x // 2, y - 4)
        assert len(window.opened) <= before + 1, "A single click opened multiple pages"
        last_y = y
assert window.opened == [
    "https://example.org/first?q=1&lang=zh", "https://example.org/second"
], window.opened
window.close()
print("DSH GUI passed", flush=True)
