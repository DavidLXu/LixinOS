"""Create the installable LixinOS GNOME Shell package."""
import argparse
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import tempfile

PORT = Path(__file__).resolve().parent
ROOT = PORT.parent.parent
CACHE = ROOT / "Kernel/target/user-build"
BUILD = CACHE / "gnome-port"
STAGE = BUILD / "shell-cross"


def package_session(package):
    data = package / "lib/gnome"
    schemas = data / "schemas"
    schemas.mkdir(parents=True)
    for source in (BUILD / "sysroot/share/glib-2.0/schemas", BUILD / "mutter-cross/data",
                   BUILD / "gdm-49.0/data", STAGE / "data"):
        for file in source.glob("*.xml"):
            if file.name.endswith((".gschema.xml", ".enums.xml")):
                shutil.copy2(file, schemas / file.name)
    for name in ("libgweather4-4.4.4-r1", "gnome-settings-daemon-48.1-r2"):
        apk = BUILD / f"{name}.apk"
        for path in subprocess.check_output(["tar", "-tf", apk], text=True).splitlines():
            if (path.startswith("usr/share/glib-2.0/schemas/") and
                    path.endswith((".gschema.xml", ".enums.xml"))):
                with (schemas / Path(path).name).open("wb") as output:
                    subprocess.run(["tar", "-xOf", apk, path], stdout=output, check=True)
    subprocess.run(["glib-compile-schemas", "--strict", schemas], check=True)
    shutil.copytree(BUILD / "xkb-data/share/xkeyboard-config-2", data / "xkb")
    shutil.copytree(BUILD / "shell-native-gi/typelib", data / "girepository-1.0")
    resources = data / "share/gnome-shell"
    resources.mkdir(parents=True)
    for name in ("dbus-interfaces", "icons", "osk-layouts", "theme"):
        file = STAGE / f"data/gnome-shell-{name}.gresource"
        shutil.copy2(file, resources / file.name)
    iso_codes = data / "share/xml/iso-codes"
    iso_codes.mkdir(parents=True)
    for name in ("iso_3166.xml", "iso_639_3.xml"):
        with (iso_codes / name).open("wb") as output:
            subprocess.run(["tar", "-xOf", BUILD / "iso-codes-4.19.0-r0.apk",
                            f"usr/share/xml/iso-codes/{name}"], stdout=output, check=True)
    with (data / "Locations.bin").open("wb") as output:
        subprocess.run(["tar", "-xOf", BUILD / "libgweather4-4.4.4-r1.apk",
                        "usr/lib/libgweather-4/Locations.bin"], stdout=output, check=True)
    fonts = data / "fonts"
    fonts.mkdir()
    with (fonts / "Cantarell-VF.otf").open("wb") as output:
        subprocess.run(["tar", "-xOf", BUILD / "font-cantarell-0.303.1-r3.apk",
                        "usr/share/fonts/cantarell/Cantarell-VF.otf"], stdout=output, check=True)
    shutil.copy2(ROOT / "User/gnome/fonts.conf", data / "fonts.conf")
    with tempfile.TemporaryDirectory(prefix="lixin-icons-") as directory:
        icons = "usr/share/icons/Adwaita"
        subprocess.run(["tar", "-xf", BUILD / "adwaita-icon-theme-49.0-r0.apk", "-C", directory,
                        f"{icons}/index.theme", f"{icons}/symbolic", f"{icons}/cursors"], check=True)
        shutil.copytree(Path(directory) / icons, data / "share/icons/Adwaita")


def build_package(package):
    compiler, = CACHE.glob("arm-gnu-toolchain-*-aarch64-none-elf")
    os.environ["PATH"] = f"{compiler / 'bin'}:{os.environ['PATH']}"
    pack = runpy.run_path(str(ROOT / "User/gcc/package.py"))["tool"]
    with tempfile.TemporaryDirectory(prefix="lixin-gnome-build-") as temporary:
        temporary = Path(temporary)
        payload = temporary / "package"
        launcher = temporary / "startgnome"
        subprocess.run([BUILD / "cc", "-Os", "-ffunction-sections", "-fdata-sections",
                        "-I", CACHE / "cpython-port/musl-1.2.5/src/process",
                        PORT / "startgnome.c", ROOT / "User/rust/spawn.c", "-o", launcher], check=True)
        for name, binary in (("startgnome", launcher),
                             ("gnome-shell", STAGE / "src/gnome-shell-lixinos")):
            pack(payload, f"/bin/{name}", binary, library="/lib/gnome", build_dir=temporary,
                 launcher_flag="-DLX_DBUS", deflate=True)
        package_session(payload)
        shutil.copytree(payload, package, dirs_exist_ok=True)
        images = Path("lib/gnome/images")
        for file in (package / images).iterdir():
            if re.fullmatch("[0-9a-f]{24}", file.name) and not (payload / images / file.name).exists():
                file.unlink()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    args = parser.parse_args()
    build_package(args.package)
