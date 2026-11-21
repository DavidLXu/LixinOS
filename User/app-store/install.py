"""Install the App Store and its menu/agent integration in one offline transaction."""
import argparse
import hashlib
from pathlib import Path
import runpy
import struct
import time

ROOT = Path(__file__).resolve().parents[2]
installer = runpy.run_path(str(ROOT / "User/gcc/install.py"))
BUILD = ROOT / "Kernel/target/user-build"


def extend_source(rows, guest, file, begin, end):
    # Older daily disks may carry other local edits. Insert just this integration.
    path = guest.encode()
    stamp, data = rows[path]
    source = (ROOT / file).read_bytes()
    block = source[source.index(begin):source.index(end, source.index(begin))]
    if block in data:
        return
    if begin in data or data.count(end) != 1:
        raise ValueError(f"could not merge {guest}; its integration point has changed")
    rows[path] = (stamp[:4] + struct.pack("<I", int(time.time())), data.replace(end, block+end, 1))


# Stock workbench files on daily images predating the shared user-space Agent.
# The older worker uses a TCC-only revision syscall when invoked by a Rust kernel.
LEGACY_AGENT = {
    "native.py": "cb0626a266484f5a5bbb69962215502acc1d3ec90bf1f6b67b4731390af0a5d9",
    "desktop.py": "7520047a0aa364dec6f0b235fa3a35f8e60a9bed46a474ba3b188d0f274b8bc3",
    "desktop.md": "8accce35725c5af1c4c4a35c72161f0ee3d6ae385f1a2c7ce22383ef812e0de4",
}


def upgrade_agent(rows):
    for name, old_digest in LEGACY_AGENT.items():
        path = ("/src/agent/" + name).encode()
        stamp, old = rows[path]
        if hashlib.sha256(old).hexdigest() == old_digest:
            payload = (ROOT / "User/agent" / name).read_bytes()
            rows[path] = stamp[:4] + struct.pack("<I", int(time.time())), payload
    if b"def desktop_session(" not in rows[b"/src/agent/native.py"][1]:
        raise ValueError("This disk has a customized legacy Agent. Merge the user-space desktop worker before installing.")


def install(disk):
    store = installer["package_edit"](BUILD / "app-store-package", "app-store")
    desktop = installer["package_edit"](BUILD / "netsurf-port/package", "netsurf")

    def edit(rows):
        upgrade_agent(rows)
        store(rows)
        desktop(rows)
        extend_source(rows, "/src/agent/native.py", "User/agent/native.py",
                      b'    elif mode == "desktop-work" and len(rest) == 3 and rest[:2]',
                      b'    elif mode == "desktop-work" and len(rest) >= 2:')
        extend_source(rows, "/src/agent/desktop.py", "User/agent/desktop.py",
                      b'    if Path.cwd().parent == Path("/work/store")',
                      b'    with tempfile.TemporaryDirectory(dir=path.parent) as folder:')
        extend_source(rows, "/src/desktop/desktop.c", "User/desktop/desktop.c",
                      b'    {"app-store.win3",', b'    {"settings.win3","')
    return installer["edit_files"](disk, edit)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path)
    disk = parser.parse_args().disk
    print(f"{disk}: App Store generation {install(disk)}")
