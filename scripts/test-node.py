"""Exercise the installed Node.js package in both released LixinOS kernels."""
import argparse
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile

from disk_image import require_local

root = Path(__file__).resolve().parents[1]
native = runpy.run_path(str(root / "scripts/test-native-kernel.py"))
install = runpy.run_path(str(root / "User/gcc/install.py"))["install"]


def check(kernel, release, seed, package, scratch):
    disk = scratch / f"{kernel}.img"
    # APFS cloning is optional. Plain copies also isolate guest writes.
    if subprocess.run(["cp", "-c", seed, disk], capture_output=True).returncode:
        shutil.copyfile(seed, disk)
    install(package, disk, "node")
    cmd = [root / "scripts/run-kernel.sh", "--kernel", kernel, "--release", release, "--serial"]
    with native["guest"]["boot"](disk, cmd, prompt=b"LixinOS Bash (C shell)") as serial:
        serial.expect(b"cd /\r", native["shell"])
        def command(line, expected=b"", status=0):
            native["command"](serial, line, expected=expected, status=status)
        command("node --version", b"\nv22.23.5\n")
        command("node -p 'process.platform + \"/\" + process.arch'", b"\nlixinos/arm64\n")
        command("node -p '6 * 7'", b"\n42\n")
        command("""node -e 'process.title = "demo"; console.log(process.execPath)'""", b"\n/bin/node\n")
        command("""echo 'console.log(7 * 6)' | node""", b"\n42\n")
        serial.send(b"node\r")
        assert b"Welcome to Node.js v22.23.5" in serial.until(b"> ", timeout=30)
        serial.send(b"7 * 6\r")
        assert b"\n42\n" in serial.until(b"> ", timeout=30)
        serial.expect(b".exit\r", native["shell"])
        directory = "/" + scratch.name
        command("mkdir " + directory)
        prompt = ("lixin:" + directory + "$ ").encode()
        serial.expect(("cd " + directory + "\r").encode(), prompt)
        def in_dir(line, expected=b"", status=0):
            native["command"](serial, line, expected=expected, status=status, prompt=prompt)
        in_dir("echo 'module.exports = n => n * 2' > double.cjs")
        in_dir("""node -p 'require("./double.cjs")(21)'""", b"\n42\n")
        in_dir("""node -e 'require("node:fs").writeFileSync("note", Buffer.from("hello"))'""")
        in_dir("""node -e 'require("node:fs/promises").readFile("note", "utf8").then(x => setTimeout(() => console.log(x), 10))'""", b"\nhello\n")
        in_dir("""echo 'import {readFile} from "node:fs/promises"; console.log(await readFile("note", "utf8"))' > main.mjs""")
        in_dir("node main.mjs", b"\nhello\n")
        in_dir("node -e 'process.exit(7)'", status=7)
        in_dir("""node -e 'require("./absent.cjs")'""", b"MODULE_NOT_FOUND", status=1)
        native["shutdown"](serial)
    print(f"{kernel} {release}: Node CLI, REPL, CommonJS, ESM, filesystem, timers and exit status passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", required=True, type=Path)
    parser.add_argument("--package", type=Path, default=root/"Kernel/target/node-port/package")
    parser.add_argument("--kernel", choices=("both", "rust", "tcc"), default="both")
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    seed = args.disk.resolve(strict=True)
    require_local(seed, seed.stat())
    release = args.release
    if release == "latest":
        release = subprocess.check_output(["python3", root/"scripts/kernel-release.py", "latest"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix="lixin-node-") as tmp:
        for kernel in ("rust", "tcc") if args.kernel == "both" else (args.kernel,):
            check(kernel, release, seed, args.package, Path(tmp))


if __name__ == "__main__":
    main()
