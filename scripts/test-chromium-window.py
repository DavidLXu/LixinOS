"""Boot a private copy of a Mutter disk and exercise a Chromium window over QMP."""
import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import runpy
import select
import subprocess
import tempfile
import threading
import time

import numpy as np

from disk_image import edit_files

HERE = Path(__file__).resolve().parent
MUTTER = runpy.run_path(str(HERE / "test-mutter.py"))
RUNTIME = HERE.parent / "Kernel/target/linux-user/package"
PAGE = b"""<!doctype html><meta charset="utf-8"><style>
html{background:#1e936f}body{height:300vh;margin:0}input{position:fixed;left:15%;top:15%;
width:60%;height:20%;background:#e95935;border:0}
input:hover{outline:6px solid #297bb8}
</style><input aria-label="Window test"><script>
const field=document.querySelector('input'), root=document.documentElement;
field.onclick=()=>{root.style.background='#315cb1'};
field.oninput=()=>{if(field.value==='a')root.style.background='#9b4085'};
onscroll=()=>{if(scrollY>0)root.style.background='#cfad32'};
onresize=()=>{if(scrollY>0)root.style.background='#30959e'};
const checks=location.hash.slice(1).split(',');
if(checks.includes('profile')) {
  const key='lixinos-window-test-value';
  if(localStorage.getItem(key)==='a') root.style.background='#754891';
  field.addEventListener('input',()=>localStorage.setItem(key,field.value));
}
if(checks.includes('cjk')) {
  const canvas=document.createElement('canvas');
  canvas.width=48; canvas.height=48;
  const ctx=canvas.getContext('2d',{willReadFrequently:true});
  ctx.font='32px sans-serif';
  function glyph(code) {
    ctx.clearRect(0,0,48,48);
    ctx.fillText(String.fromCodePoint(code),2,36);
    return ctx.getImageData(0,0,48,48).data;
  }
  const first=glyph(0x4e2d), second=glyph(0x6587), missing=glyph(0x10ffff);
  const different=(a,b)=>a.some((v,i)=>v!==b[i]);
  if(first.some(Boolean) && second.some(Boolean) && different(first,second) &&
     different(first,missing) && different(second,missing)) {
    glyph(0x6587);
    canvas.style='position:fixed;right:2%;top:5%;background:#53adbb';
    document.body.appendChild(canvas);
  }
}
</script>"""


@contextmanager
def fixture_url(use_http):
    if not use_http:
        yield "file:///home/chromium/window-test.html"
        return

    requested = threading.Event()

    class PageHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != "/window-test.html":
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)
            requested.set()

        def log_message(self, *_):
            pass

    with ThreadingHTTPServer(("127.0.0.1", 0), PageHandler) as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            # QEMU user networking exposes the host's loopback at this guest address.
            yield f"http://10.0.2.2:{server.server_port}/window-test.html"
            if not requested.is_set():
                raise AssertionError("The browser did not fetch the page from the test server")
        finally:
            server.shutdown()
            worker.join()


def check(disk, package, shell, compositor, release, kernel, trace, failure_frame, page_timeout, verbose,
          use_http=False, check_cjk=False, check_profile=False):
    state = "/chromium-state"
    def exercise(guest, control, tmp):
        qmp = MUTTER["qmp_connection"](control)
        qmp("qmp_capabilities")
        size_file = Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"])
        size_file.write_text("1600\t1200\n")
        guest.until(b"$ ")
        guest.expect(b"cd /\r", MUTTER["GUEST"]["shell"])
        MUTTER["GUEST"]["command"](guest, f"export HOME={state}/home XDG_RUNTIME_DIR={state}/run "
                                 "GSETTINGS_SCHEMA_DIR=/lib/gnome/schemas GSETTINGS_BACKEND=memory")
        debug = "--trace " if trace else ""
        log_switch = "--enable-logging=stderr --v=1 " if verbose else ""
        window_args = f"--content-shell-host-window-size=520x360 {url}"
        if package is None:
            launch = f"chromium {log_switch}{window_args}"
        else:
            launch = (f"dbus-run-session -- {compositor} --lixinos-display --wayland-display wayland-0 "
                      f"-- linux-run {debug}--processes --state {state} {shell} {log_switch}"
                      f"--ozone-platform=wayland --disable-gpu --in-process-gpu "
                      f"--disable-gpu-shader-disk-cache {window_args}")
        started = time.monotonic()
        guest.send(launch.encode() + b"\r")
        transcript = bytearray()
        frame = tmp / "screen.ppm"

        def screen_pixels():
            qmp("screendump", filename=str(frame))
            _, dim, _, data = frame.read_bytes().split(b"\n", 3)
            w, h = map(int, dim.split())
            return np.frombuffer(data, dtype=np.uint8).reshape(h, -1)[:, :3*w].reshape(h, w, 3)

        def color(rgb, timeout=60):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                while select.select([guest.vm.stdout], [], [], 0)[0]:
                    chunk = os.read(guest.vm.stdout.fileno(), 65536)
                    if not chunk:
                        raise AssertionError("QEMU serial closed")
                    if trace:
                        trace.write(chunk)
                    transcript.extend(chunk)
                    if b"guest stopped" in transcript or b"FATAL:" in transcript or b"[exit " in transcript:
                        raise AssertionError(transcript[-1800:].decode(errors="replace"))
                    del transcript[:-4096]
                pixels = screen_pixels()
                h, w = pixels.shape[:2]
                rows, cols = np.where(np.all(pixels[::4, ::4] == rgb, axis=2))
                if len(rows) >= 100:
                    return (round(4 * np.median(cols) * 32767 / (w-1)),
                            round(4 * np.median(rows) * 32767 / (h-1)))
                time.sleep(.8 if timeout > 60 else .15)
            raise AssertionError(f"Page color {rgb} missing: {transcript[-1500:]!r}")

        def button(name):
            for down in (True, False):
                qmp("input-send-event", events=[{"type": "btn", "data": {"button": name, "down": down}}])
                time.sleep(.15)

        def move(x, y):
            qmp("input-send-event", events=[{"type": "abs", "data": {"axis": axis, "value": val}}
                                            for axis, val in (("x", x), ("y", y))])
            time.sleep(.2)

        def caption_button(position_from_right, maximized=False):
            # Mutter may already show a resized page while the caption still
            # spans the old buffer. Wait for the matching window geometry.
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                caption = np.all(screen_pixels() == (211, 227, 253), axis=2)
                h, w = caption.shape
                rows = np.flatnonzero(caption.sum(axis=1) > w // 5)
                if rows.size:
                    y = int(np.median(rows))
                    xs = np.flatnonzero(caption[y])
                    if xs.size and (not maximized or xs[-1] - xs[0] > w * 0.9):
                        break
                time.sleep(.2)
            else:
                raise AssertionError("Chromium caption did not reach its expected width")
            # Views puts Close and Maximize at the right end of the caption.
            right = xs[-1]
            x = right - (position_from_right + 0.5) * (rows.max() - rows.min() + 1)
            move(round(x * 32767 / (w - 1)), round(y * 32767 / (h - 1)))
            button("left")

        def close_window(maximized=False):
            caption_button(0, maximized)
            reply = guest.until(MUTTER["GUEST"]["shell"], timeout=360)
            if trace:
                trace.write(reply)
            assert b"[exit " not in reply and b"chromium profile:" not in reply, reply[-1500:]

        try:
            background = color((30, 147, 111), page_timeout)
            print(f"{kernel}: {'HTTP' if use_http else 'local'} page painted "
                  f"after {time.monotonic() - started:.1f} s", flush=True)
            if check_cjk:
                color((83, 173, 187))
                print(f"{kernel}: rendered distinct Chinese glyphs in the page", flush=True)
            # Focus the page before moving into its text field.
            move(*background)
            button("left")
            x, y = color((233, 89, 53))
            move(x, y)
            color((41, 123, 184))
            button("left")
            color((49, 92, 177))
            print(f"{kernel}: click reached the page", flush=True)
            qmp("send-key", keys=[{"type": "qcode", "data": "a"}])
            body = color((155, 64, 133))
            print(f"{kernel}: keyboard input reached the field", flush=True)
            move(*body)
            button("wheel-down")
            color((207, 173, 50))
            print(f"{kernel}: wheel scrolled the document", flush=True)
            caption_button(1)
            color((48, 149, 158))
            print(f"{kernel}: Chromium rendered CSS and responded to pointer, text, scroll and resize")
            if check_profile:
                close_window(maximized=True)
                guest.send(launch.encode() + b"\r")
                color((117, 72, 145), page_timeout)
                close_window()
                print(f"{kernel}: reopened the saved field value and exited cleanly", flush=True)
        except (AssertionError, TimeoutError):
            if failure_frame:
                qmp("screendump", filename=str(failure_frame.resolve()))
            raise

    def stage(rows):
        nonlocal state
        for tree in ((package, RUNTIME) if package else ()):
            for path in tree.rglob("*"):
                key = b"/" + path.relative_to(tree).as_posix().encode()
                rows[key] = bytes(8), path.read_bytes() if path.is_file() else None
        legacy = rows.get(b"/chromium-state", (None, None))[1]
        has_profile = legacy is not None or any(key.startswith(b"/chromium-state/") for key in rows)
        if not has_profile and b"/scratch" in rows and rows[b"/scratch"][1] is None:
            state = "/scratch/chromium-state"
        for suffix in ("", "/home", "/run"):
            rows.setdefault((state + suffix).encode(), (bytes(8), None))
        rows[(state + "/home/window-test.html").encode()] = bytes(8), PAGE
        if package and (shell.encode() not in rows or compositor.encode() not in rows):
            raise ValueError("The selected Chromium executable or Mutter compositor is missing")
        if not package and b"/bin/chromium" not in rows:
            raise ValueError("The guest disk needs the installed Chromium launcher")

    with tempfile.TemporaryDirectory(prefix="lixinos-chromium-window-") as path:
        copy = Path(path) / "chromium.img"
        subprocess.run(["cp", "-c", disk, copy], check=True)
        edit_files(copy, stage)
        with fixture_url(use_http) as page_url:
            checks = [name for name, enabled in (("cjk", check_cjk), ("profile", check_profile)) if enabled]
            url = page_url + ("#" + ",".join(checks) if checks else "")
            MUTTER["with_window"](copy, release, kernel, exercise)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path, help="LixinOS template with Mutter, D-Bus and fonts")
    parser.add_argument("package", nargs="?", type=Path, help="Overlay a staged build. Omit to test the installed launcher")
    parser.add_argument("--shell", default="/opt/chromium/content_shell")
    parser.add_argument("--compositor", default="/bin/mutter")
    parser.add_argument("--release", required=True)
    parser.add_argument("--kernel", choices=("rust", "tcc"), default="rust")
    parser.add_argument("--trace", type=argparse.FileType("wb"))
    parser.add_argument("--failure-frame", type=Path, help="Save the guest display as PPM on a failed assertion")
    parser.add_argument("--page-timeout", default=1200, type=int, help="Seconds to wait for first paint")
    parser.add_argument("--verbose", action="store_true", help="Enable Chromium diagnostic logging")
    parser.add_argument("--check-cjk", action="store_true", help="Check actual Chinese glyph drawing in the window")
    parser.add_argument("--check-profile", action="store_true", help="Close and reopen Chromium and verify localStorage")
    parser.add_argument("--http", action="store_true", help="Fetch the page from a local HTTP fixture via QEMU")
    args = parser.parse_args()
    check(args.disk, args.package, args.shell, args.compositor, args.release, args.kernel, args.trace,
          args.failure_frame, args.page_timeout, args.verbose, args.http, args.check_cjk, args.check_profile)
