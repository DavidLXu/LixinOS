"""Create the account database and install the PAM tools on an idle guest disk."""
import argparse
import runpy
import secrets
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from disk_image import edit_files
package_edit = runpy.run_path(str(ROOT / "User/gcc/install.py"))["package_edit"]


def bootstrap(rows):
    # Passwords are provisioned from the guest console. Preserve the existing
    # database, including passwords, when other components are upgraded.
    stamp = struct.pack("<I", int(time.time())) * 2
    defaults = {
        "/etc": None, "/tmp": None, "/root": None, "/home": None, "/home/lixin": None,
        "/etc/passwd": b"root:x:0:0:System Administrator:/root:/bin/bash\nlixin:x:1000:1000:Lixin:/home/lixin:/bin/bash\n",
        "/etc/group": b"root:x:0:\nlixin:x:1000:\n",
        "/etc/shadow": b"root:!:::::::\nlixin:!:::::::\n",
        "/etc/machine-id": (secrets.token_hex(16) + "\n").encode(),
    }
    for path, data in defaults.items():
        key = path.encode()
        if key in rows and (rows[key][1] is None) != (data is None):
            raise ValueError(f"unexpected entry type at {path}")
        rows.setdefault(key, (stamp, data))
    for key, entry in ((b"/etc/group", b"gdm:!:119:"),
                       (b"/etc/passwd", b"gdm:!:119:119:GNOME greeter:/tmp:/bin/false")):
        times, raw = rows[key]
        lines = bytes(raw).splitlines()
        existing = [line.split(b":") for line in lines if line.split(b":",1)[0] == b"gdm"]
        ids = slice(2, 4 if key == b"/etc/passwd" else 3)
        if existing:
            if len(existing) != 1 or existing[0][ids] != entry.split(b":")[ids]:
                raise ValueError("existing gdm account has conflicting UID or GID")
            continue
        if any(line.split(b":")[2:3] == [b"119"] for line in lines):
            raise ValueError("UID or GID 119 already belongs to another account")
        rows[key] = times[:4] + stamp[4:], bytes(raw).rstrip(b"\n") + b"\n" + entry + b"\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path)
    args = parser.parse_args()
    package = ROOT / "Kernel/target/user-build/pam-port/package"

    def install(rows):
        bootstrap(rows)
        package_edit(package, "pam")(rows)
    print(edit_files(args.disk, install))
