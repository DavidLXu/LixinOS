"""Install a registry package on a copy of an npm-equipped daily disk and reboot it."""
import argparse
from pathlib import Path
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
harness = runpy.run_path(str(ROOT / "scripts/test-native-kernel.py"))


def exercise(disk, kernel, release, directory, first):
    command = [ROOT / "scripts/run-kernel.sh", "--kernel", kernel, "--release", release, "--serial"]
    with harness["guest"]["boot"](disk, command, prompt=b"LixinOS Bash (C shell)") as guest:
        guest.until(b"$ ", timeout=120)
        guest.expect(b"cd /\r", harness["shell"])
        if first:
            harness["command"](guest, "npm --version", expected=b"\n11.20.0\n")
            harness["command"](guest, "npx --version", expected=b"\n11.20.0\n")
            harness["command"](guest, f"mkdir /{directory}")
        prompt = f"lixin:/{directory}$ ".encode()
        guest.expect(f"cd /{directory}\r".encode(), prompt)
        if first:
            harness["command"](guest, "npm init -y", prompt=prompt)
            harness["command"](guest, "npm install is-number@7.0.0", expected=b"added 1 package", prompt=prompt)
        harness["command"](guest, """node26 -p 'require("is-number")(42)'""", expected=b"\ntrue\n", prompt=prompt)
        harness["command"](guest, """node26 -p 'require("./package-lock.json").packages["node_modules/is-number"].version'""",
                           expected=b"\n7.0.0\n", prompt=prompt)
        harness["shutdown"](guest)
    print(f"{kernel} verified {'registry install' if first else 'persisted dependency and lockfile'}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="npm-check-") as tmp:
        disk = Path(tmp) / "test.img"
        subprocess.run(["cp", "-c", args.disk, disk], check=True)
        for kernel, first in (("rust", True), ("tcc", False)):
            exercise(disk, kernel, args.release, Path(tmp).name, first)


if __name__ == "__main__":
    main()
