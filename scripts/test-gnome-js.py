"""Boot SpiderMonkey in the same LixinOS image on both native kernels."""
import argparse
from pathlib import Path
import runpy
import shutil
import tempfile

from disk_image import edit_files, prepare

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SUPPORT = runpy.run_path(str(HERE / "test-gnome-gi.py"))
GUEST = SUPPORT["GUEST"]


def check(release, with_gjs):
    gnome = SUPPORT["BUILD"] / "gnome-port"
    pack = runpy.run_path(str(ROOT / "User/gcc/package.py"))["tool"]
    with tempfile.TemporaryDirectory(prefix="lixin-mozjs-") as temp:
        temp = Path(temp)
        disk = temp / "js.img"
        package = temp / "package"
        pack(package, "/bin/js", gnome / "mozjs-build/dist/bin/js", library="/lib/gnome",
             build_dir=temp, launcher_flag="-DLX_DBUS", deflate=True)
        if with_gjs:
            pack(package, "/bin/gjs", gnome / "gjs-cross/gjs-lixinos", library="/lib/gnome",
                 build_dir=temp, launcher_flag="-DLX_DBUS", deflate=True)
            data = package / "lib/gnome"
            shutil.copytree(gnome / "gjs-native-gi/typelib", data / "girepository-1.0")
            shutil.copy2(ROOT / "User/gnome/tests/gjs-check.js", data / "gjs-check.js")
        prepare(disk)
        with SUPPORT["boot"](disk, release, "rust") as serial:
            GUEST["check_boot_release"](serial, b"rust")
            GUEST["shutdown"](serial)

        def install(rows):
            for file in package.rglob("*"):
                path = b"/" + file.relative_to(package).as_posix().encode()
                if path not in rows:
                    rows[path] = (bytes(8), file.read_bytes() if file.is_file() else None)

        edit_files(disk, install)
        for kernel in ("rust", "tcc"):
            with SUPPORT["boot"](disk, release, kernel) as serial:
                GUEST["check_boot_release"](serial, kernel.encode())
                GUEST["command"](serial, "js --version", expected=b"JavaScript-C140.0")
                GUEST["command"](serial, "js -e 'print(6 * 7)'", expected=b"\n42\n")
                GUEST["command"](serial, "js -e 'print(new Intl.NumberFormat(\"de-DE\").format(1234.5))'",
                                 expected=b"\n1.234,5\n")
                if with_gjs:
                    GUEST["command"](serial, "gjs --version", expected=b"gjs 1.86.0")
                    GUEST["command"](serial, "gjs -c 'print(6 * 7)'", expected=b"\n42\n")
                    GUEST["command"](serial, "gjs -m /lib/gnome/gjs-check.js",
                                     expected=b"GLib called JavaScript through libffi")
                GUEST["shutdown"](serial)
            detail = "SpiderMonkey and GJS" if with_gjs else "SpiderMonkey"
            print(f"{kernel}: {detail} checks passed", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", default="latest")
    parser.add_argument("--with-gjs", action="store_true")
    options = parser.parse_args()
    check(options.release, options.with_gjs)
