"""Build a separate, bootable LixinOS disk with Tencent's ARM64 WeChat client."""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

from disk_image import BUILD_HEADER, BUILD_SLOT, HISTORY_BYTES, SECTOR, edit_files

ROOT = Path(__file__).resolve().parents[1]
LOADER = runpy.run_path(str(ROOT / "scripts/test-wechat-loader.py"))
GUEST = LOADER["NATIVE"]
PORT = ROOT / "User/linux-run"
CACHE = ROOT / "Kernel/target/user-build"
BUILD = ROOT / "Kernel/target/linux-user"


def native_ui(package, work):
    pack = runpy.run_path(str(ROOT / "User/gcc/package.py"))["tool"]
    relink = runpy.run_path(str(ROOT / "User/gnome/package-mutter.py"))["compositor"]
    cc = CACHE / "cpython-port/cc"
    threads = work / "threads.o"
    subprocess.run([cc, "-Os", "-ffunction-sections", "-fdata-sections", "-DLX_PTHREAD_COOPERATIVE_ONLY",
                    "-c", ROOT / "User/cpython/threads.c", "-o", threads], check=True)
    mutter = relink(CACHE, work / "mutter", threads)
    pack(package, "/lib/wechat-ui/mutter", mutter, library="/lib/wechat-ui", build_dir=work,
         launcher_flag="-DLX_DBUS", deflate=True)
    native_launcher(package, work, pack)


def native_launcher(package, work, pack):
    launcher = work / "wechat"
    subprocess.run([CACHE / "gnome-port/cc", "-Os", "-ffunction-sections", "-fdata-sections",
                    "-I", CACHE / "cpython-port/musl-1.2.5/src/process",
                    PORT / "wechat.c", ROOT / "User/rust/spawn.c", "-o", launcher], check=True)
    pack(package, "/bin/wechat", launcher, library="/lib/wechat-ui",
         build_dir=work, launcher_flag="-DLX_DBUS", deflate=True)


def copy_tree(rows, tree, dest):
    if not tree.exists():
        raise FileNotFoundError(tree)
    for file in (tree, *tree.rglob("*")):
        path = dest / file.relative_to(tree)
        if path == Path("/"):
            continue
        rows[str(path).encode()] = bytes(8), file.read_bytes() if file.is_file() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deb", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--release", default="kernel-2026-11-25.7")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error(f"{output} already exists")
    if LOADER["digest"](args.deb) != LOADER["WECHAT_SHA"]:
        parser.error("expected the official WeChat 4.1.13.29 arm64 .deb")
    subprocess.run(["bash", PORT / "build.sh"], check=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        work = Path(temp)
        package = work / "ui"
        native_ui(package, work)
        disk = work / "wechat.img"
        with disk.open("xb") as f:
            f.write(BUILD_HEADER)
            f.truncate(SECTOR + 2 * BUILD_SLOT + HISTORY_BYTES)
        cmd = [ROOT / "scripts/run-kernel.sh", "--kernel", "rust", "--release", args.release, "--serial"]
        with GUEST["guest"]["boot"](disk, cmd, prompt=b"LixinOS Bash (C shell)") as guest:
            guest.until(b"$ ")
            guest.expect(b"cd /\r", GUEST["shell"])
            GUEST["command"](guest, "mkdir /wx-state")
            GUEST["shutdown"](guest)

        def stage(rows):
            rows.setdefault(b"/scratch", (bytes(8), None))
            for tree in (BUILD / "package", CACHE / "dbus-port/package", package):
                copy_tree(rows, tree, Path("/"))
            base = CACHE / "gnome-port/package/lib/gnome"
            rows[b"/lib/gnome"] = bytes(8), None
            for name in ("xkb", "schemas", "fonts", "fonts.conf"):
                copy_tree(rows, base / name, Path("/lib/gnome") / name)
            LOADER["stage_wechat"](rows, args.deb, gui=True)
            LOADER["stage_debian"](rows, BUILD / "wechat-debs", gui=True)

        edit_files(disk, stage)
        os.link(disk, output)  # Never overwrite an existing work disk.
    print(output)


if __name__ == "__main__":
    main()
