"""Install a staged Chromium window and its native launcher on a copy of a Mutter disk."""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

from disk_image import BUILD_HEADER, edit_files

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / "Kernel/target/user-build"
RUNTIME = ROOT / "Kernel/target/linux-user/package"


def install(disk, package, output):
    if output.exists():
        raise FileExistsError(output)
    with disk.open("rb") as source:
        build_volume = source.read(len(BUILD_HEADER)) == BUILD_HEADER
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        work = Path(tmp)
        native = work / "chromium"
        pack = runpy.run_path(str(ROOT / "User/gcc/package.py"))["tool"]
        subprocess.run([CACHE / "gnome-port/cc", "-Os", "-ffunction-sections", "-fdata-sections",
                        "-I", CACHE / "cpython-port/musl-1.2.5/src/process",
                        ROOT / "User/chromium/launcher.c", ROOT / "User/rust/spawn.c",
                        "-o", native], check=True)
        launcher = work / "launcher"
        pack(launcher, "/bin/chromium", native, library="/lib/chromium", build_dir=work,
             launcher_flag="-DLX_DBUS", deflate=True)
        clone = work / "chromium.img"
        subprocess.run(["cp", "-c", disk, clone], check=True)

        def overlay(rows):
            old_size = sum(len(data) for _, data in rows.values() if data is not None)
            new_libraries = []
            if build_volume:
                rows.setdefault(b"/scratch", (bytes(8), None))
            # Keep the loader and libc from the same guest runtime build.
            for tree in (package, RUNTIME, launcher):
                for path in tree.rglob("*"):
                    key = b"/" + path.relative_to(tree).as_posix().encode()
                    if tree == RUNTIME and key.startswith(b"/bin/linux-") and key != b"/bin/linux-run":
                        continue
                    if (tree == package and path.parent == package / "opt/chromium" and
                            path.match("lib*.so*") and path.is_file() and key not in rows):
                        new_libraries.append(key)
                    times = rows[key][0] if key in rows else bytes(8)
                    rows[key] = times, path.read_bytes() if path.is_file() else None
            # The dynamic loader also searches the guest's Debian multiarch
            # directory. Only elide a new private copy when all bytes match.
            reused = 0
            for key in new_libraries:
                system = rows.get(b"/lib/aarch64-linux-gnu/" + key.rsplit(b"/", 1)[1])
                if system is not None and system[1] == rows[key][1]:
                    del rows[key]
                    reused += 1
            required = (b"/bin/dbus-run-session", b"/bin/linux-run", b"/opt/chromium/content_shell")
            for key in required:
                if key not in rows:
                    raise ValueError(f"Missing guest program {key.decode()}")
            if not any(key in rows for key in (b"/bin/mutter", b"/lib/wechat-ui/mutter")):
                raise ValueError("The template disk needs a native Mutter compositor")
            new_size = sum(len(data) for _, data in rows.values() if data is not None)
            print(f"Reused {reused} identical system libraries, guest payload change {new_size - old_size:,} bytes")

        edit_files(clone, overlay)
        os.link(clone, output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path)
    parser.add_argument("package", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    install(args.disk, args.package, args.output)
    print(args.output.resolve())
