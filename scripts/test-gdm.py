"""Exercise GNOME's on-screen login dialog and check the user's desktop."""
import argparse
import os
from pathlib import Path
import runpy
import secrets
import shutil
import subprocess
import tempfile
import time

from disk_image import edit_files


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MUTTER = runpy.run_path(str(HERE / "test-mutter.py"))
ACCOUNTS = runpy.run_path(str(ROOT / "User/accounts/install.py"))
BUILD = ROOT / "Kernel/target/user-build"
PACKAGES = {
    "dbus": "dbus-port/package", "gnome": "gnome-port/package",
    "gdm": "gnome-port/gdm-backend/package", "ibus": "gnome-port/ibus-package",
    "pam": "pam-port/package", "dynlink": "dynlink-port/package",
}


def stage(rows):
    ACCOUNTS["bootstrap"](rows)
    for name, path in PACKAGES.items():
        ACCOUNTS["package_edit"](BUILD / path, name)(rows)


def check(kernel, screenshots, wrong):
    def exercise(guest, control, directory):
        qmp = MUTTER["qmp_connection"](control)
        qmp("qmp_capabilities")
        Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text("2024\t1320\n")
        guest.until(b"$ ", timeout=120)
        password = secrets.token_hex(5)
        guest.send(b"passwd lixin\r")
        guest.until(b"New password: ")
        guest.send(password.encode() + b"\r")
        guest.until(b"Retype new password: ")
        guest.send(password.encode() + b"\r")
        guest.until(b"$ ", timeout=240)
        guest.send(b"export LIXINOS_GDM_TRACE=1\r")
        guest.until(b"$ ")
        guest.send(b"gdm\r")
        guest.until(b"LixinOS compositor renderer:", timeout=180)

        def keys(sequence):
            for key in sequence:
                qmp("human-monitor-command", **{"command-line": f"sendkey {key} 50"})
                time.sleep(.15)

        def capture(name):
            file = directory / "frame.ppm"
            qmp("screendump", filename=str(file))
            if screenshots:
                screenshots.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(file, screenshots / f"gdm-{kernel}-{name}.ppm")
            return file.read_bytes()

        time.sleep(2)
        capture("username")
        keys([*"lixin", "ret"])
        guest.until(b"GDM password requested", timeout=20)
        time.sleep(.5)
        capture("password")
        if wrong:
            keys([*"invalid", "ret"])
            guest.until(b"GDM authentication denied", timeout=20)
            print(f"{kernel}: graphical greeter rejected wrong password", flush=True)
            return
        keys([*password, "ret"])
        guest.until(b"Starting session for lixin (uid 1000)", timeout=45)
        print(f"{kernel}: graphical PAM login started uid 1000", flush=True)
        guest.until(b"LixinOS compositor renderer:", timeout=180)
        deadline = time.monotonic() + 30
        while bytes.fromhex("023c88") * 128 not in capture("desktop"):
            if time.monotonic() >= deadline:
                raise AssertionError("GNOME workspace was not visible")
            time.sleep(.5)
        print(f"{kernel}: authenticated desktop visible", flush=True)
        time.sleep(6)
        keys(["l", "o", "g", "spc", "o", "u", "t"])
        deadline = time.monotonic() + 40
        while bytes.fromhex("023c88") * 128 in capture("search"):
            if time.monotonic() >= deadline:
                raise AssertionError("GNOME did not open search")
            time.sleep(.5)
        time.sleep(2)
        capture("search")
        keys(["ret"])
        try:
            out = guest.until(b"$ ", timeout=150)
        except TimeoutError:
            capture("logout-timeout")
            raise
        assert b"[exit " not in out, out
        print(f"{kernel}: logged out", flush=True)
    return exercise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", required=True, type=Path, help="powered-off seed, cloned for each run")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--kernel", choices=("both", "rust", "tcc"), default="both")
    parser.add_argument("--case", choices=("both", "accept", "reject"), default="both")
    parser.add_argument("--screenshots", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp) / "base.img"
        subprocess.run(["cp", "-c", args.disk, base], check=True)
        edit_files(base, stage)
        for profile in (("rust", "tcc") if args.kernel == "both" else (args.kernel,)):
            for wrong in ((True, False) if args.case == "both" else (args.case == "reject",)):
                disk = Path(tmp) / f"{profile}-{wrong}.img"
                subprocess.run(["cp", "-c", base, disk], check=True)
                MUTTER["with_window"](disk, args.release, profile, check(profile, args.screenshots, wrong))
