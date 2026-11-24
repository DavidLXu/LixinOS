"""Install a native package into an idle LixinOS disk via its inactive snapshot."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"scripts"))
from disk_image import edit_files

PACKAGES = {
    "physics": ("/bin/physics.win3",),
    "video": ("/bin/video.win3",),
    "app-store": ("/bin/app-store.win3",),
    "codex": ("/bin/codex",),
    "classicube": ("/bin/classicube",),
    "doom": ("/bin/doom",),
    "2048": ("/bin/2048.win3",),
    "node": ("/bin/node",),
    "node26": ("/bin/node26",),
    "npm": ("/bin/npm", "/bin/npx"),
    "dsh": ("/bin/dsh",),
    "gcc": ("/bin/gcc", "/bin/as", "/bin/ld"),
    "rust": ("/bin/rustc", "/bin/cargo"),
    "slint-demo": ("/bin/slint-demo", "/bin/slint-editor"),
    "netsurf": ("/bin/netsurf", "/bin/desktop"),
    "settings": ("/bin/settings.win2", "/bin/settings.win3"),
    "nano-x": ("/bin/nano-x",),
    "dte": ("/bin/dte",),
    "lite-xl": ("/bin/lite-xl.win3",),
    "xfce": ("/bin/startxfce4", "/bin/thunar.win3"),
    "mesa": ("/bin/lixin-egl-check",),
    "gnome": ("/bin/startgnome", "/bin/gnome-shell"),
    "wayland": ("/bin/wayland-demo", "/bin/wayland-shm-client"),
    "dbus": ("/bin/dbus-daemon", "/bin/dbus-send", "/bin/dbus-uuidgen", "/bin/dbus-run-session"),
    "gtk3": ("/bin/gtk3-demo.win3", "/bin/mousepad.win3"),
    "tetravex": ("/bin/tetravex.win3",),
    "mahjongg": ("/bin/mahjongg.win3",),
    "mines": ("/bin/mines.win3",),
    "nautilus": ("/bin/nautilus.win3",),
    "ristretto": ("/bin/ristretto.win3",),
}


def package_edit(package, name):
    library = "/lib/" + name
    binaries = PACKAGES[name]
    manifest_path = (library + "/installed.json").encode()

    def tool_path(path):
        return ((path in binaries or path.startswith(library + "/"))
                and ".." not in Path(path).parts and str(Path(path)) == path
                and path.encode() != manifest_path)

    package = package.resolve(strict=True)
    def edit(rows):
        now = struct.pack("<I",int(time.time()))
        manifest = json.loads(rows.get(manifest_path,(None,b"{}"))[1])
        if not isinstance(manifest, dict) or any(not tool_path(key) or not isinstance(value, str)
                                                for key,value in manifest.items()):
            raise ValueError("invalid installed package manifest")
        fresh = {}
        def put(path, value):
            times,old = rows.get(path,(now*2,None))
            if path in rows and old == value:
                return
            digest = hashlib.sha256(old).hexdigest() if old is not None else None
            # Also accept the bundled .5 desktop and the separately installed wallpaper build.
            stock_desktop = (name == "netsurf" and path == b"/bin/desktop" and
                             digest in {"8a641fa9f380c4a6378ee152c5ee298951aa017412892d228a5bb59d778ef55f",
                                        "c3e66c3d453be6d561d2de1f0e61ec835680b5b22486c8a3a76b9cb0995e01d9"})
            if path in rows and path != manifest_path and not stock_desktop and (old is None or manifest.get(path.decode()) != digest):
                raise ValueError(f"local file conflicts with package: {path!r}")
            rows[path] = times[:4]+now,value
        for item in sorted(package.rglob("*")):
            path = "/"+item.relative_to(package).as_posix()
            if path not in ("/lib",library,"/bin","/tmp") and not tool_path(path):
                raise ValueError(f"unexpected package path {path}")
            value = None if item.is_dir() else item.read_bytes()
            put(path.encode(),value)
            if value is not None:
                fresh[path] = hashlib.sha256(value).hexdigest()
        for path in manifest.keys()-fresh.keys():
            prior = rows.get(path.encode())
            if prior and prior[1] is not None and hashlib.sha256(prior[1]).hexdigest()==manifest[path]:
                del rows[path.encode()]
        put(manifest_path,json.dumps(fresh,sort_keys=True).encode())
    return edit


def install(package, disk, name="gcc"):
    generation = edit_files(disk, package_edit(package, name))
    print(f"{disk}: installed {name}, generation {generation}", flush=True)

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package",type=Path)
    parser.add_argument("disk",type=Path)
    parser.add_argument("--toolchain", choices=PACKAGES, default="gcc")
    args=parser.parse_args()
    install(args.package,args.disk,args.toolchain)
