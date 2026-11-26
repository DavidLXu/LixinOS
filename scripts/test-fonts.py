"""Boot a disposable copy of an installed volume and exercise its native font stack."""
import argparse
import runpy
import subprocess
import tempfile
import time
from pathlib import Path

scripts = Path(__file__).resolve().parent
mutter = runpy.run_path(str(scripts / "test-mutter.py"))

def exercise(guest, control, directory):
    guest.until(b"$ ")
    def command(line):
        guest.send(line.encode() + b"\r")
        return guest.until(b"$ ", timeout=150)
    def success(line):
        output = command(line)
        assert b"[exit " not in output, output
        return output
    listing = success("fonts list")
    for family in (b"Fira Sans", b"JetBrains Mono", b"Noto Sans Mono CJK SC"):
        assert family in listing, listing
    latin = success("fonts match monospace")
    chinese = success("fonts match 'sans-serif:charset=4e2d'")
    assert b"JetBrainsMono-Regular.ttf" in latin, latin
    assert b"NotoSansMonoCJKsc-Regular.otf" in chinese, chinese
    success("fonts install /usr/share/fonts/FiraSans-Regular.ttf")
    success("cmp /usr/share/fonts/FiraSans-Regular.ttf /usr/share/fonts/user-FiraSans-Regular.ttf")
    success("echo invalid > /tmp/FiraSans-Regular.ttf")
    assert b"invalid" in command("fonts install /tmp/FiraSans-Regular.ttf")
    success("cmp /usr/share/fonts/FiraSans-Regular.ttf /usr/share/fonts/user-FiraSans-Regular.ttf")
    success("fonts console /usr/share/fonts/JetBrainsMono-Regular.ttf")
    assert b"91960" in success("wc -c /lib/fonts/console.gray")
    # Render known UTF-8 glyphs using the public SDK and the loaded console atlas.
    source = [
        '#include <lixin-gui.h>',
        'static struct lx_font f; static unsigned pixels[45*22];',
        'int main(void){struct lx_canvas c={45,22,pixels};',
        'if(lx_gui_font(&f))return 2;',
        'lx_gui_text(&c,&f,0,0,5,"A\\xe4\\xb8\\xad\\xe6\\x96\\x87",0xffffff);',
        'int a=0,b=0,d=0;for(int y=0;y<22;y++)for(int x=0;x<45;x++){',
        'int on=!!pixels[y*45+x];if(x<9)a+=on;else if(x<27)b+=on;else d+=on;}',
        'return !(a&&b&&d&&b!=d);}',
    ]
    for i,line in enumerate(source):
        success(("printf '%s\\n' " + "'" + line + "'" + (" > " if i==0 else " >> ") + "/tmp/font-demo.c"))
    success("tcc /tmp/font-demo.c -o /tmp/font-demo")
    success("/tmp/font-demo")

    print("Font discovery, Latin/CJK matching, atomic installation and native canvas glyphs passed", flush=True)

def capture_title(guest, control, directory, reset):
    guest.until(b"$ ")
    qmp = mutter["qmp_connection"](control)
    qmp("qmp_capabilities")
    # Allow the status sampler to repaint after the filesystem has mounted.
    time.sleep(1.5)
    frame = directory / "fonts.ppm"
    # Both kernels draw their static title in the first ten 22x44 cells.
    for _ in range(30):
        qmp("screendump", filename=str(frame))
        _, dimensions, _, pixels = frame.read_bytes().split(b"\n", 3)
        width, height = map(int, dimensions.split())
        stride, rest = divmod(len(pixels), height)
        assert not rest and stride >= width * 3
        crop = b"".join(pixels[y*stride:y*stride+220*3] for y in range(44))
        if len(set(crop)) > 8:
            break
        time.sleep(0.1)
    else:
        raise AssertionError("console title was not drawn")
    if reset:
        guest.send(b"wc -c /lib/fonts/console.gray\r")
        assert b"91960" in guest.until(b"$ ")
        guest.send(b"fonts console --reset\r")
        assert b"[exit " not in guest.until(b"$ ")
    return crop


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--kernel", choices=("rust","tcc"), default="rust")
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp) / "fonts.img"
        subprocess.run(["cp","-c",args.disk,disk],check=True)
        boot = mutter["with_window"]
        boot(disk, args.release, args.kernel, exercise)
        titles = []
        for reset in (True, False):
            def screenshot(g, control, directory):
                titles.append(capture_title(g, control, directory, reset))
            boot(disk, args.release, args.kernel, screenshot)
        changed = sum(a != b for a, b in zip(*titles))
        assert changed > 300, f"console typeface did not change after reboot ({changed} channels)"
        print(f"{args.kernel}: selected and restored console faces differ in {changed} RGB channels")
