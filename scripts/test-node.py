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


VERSIONS = {"node": "v22.23.5", "node26": "v26.13.1"}


def check(kernel, release, seed, package, scratch, binary, openssl, wasm, addon):
    version = VERSIONS[binary]
    disk = scratch / f"{kernel}.img"
    # APFS cloning is optional. Plain copies also isolate guest writes.
    if subprocess.run(["cp", "-c", seed, disk], capture_output=True).returncode:
        shutil.copyfile(seed, disk)
    install(package, disk, binary)
    if addon:
        from disk_image import edit_files
        edit_files(disk, lambda rows: rows.update({b"/probe.node": (bytes(8), addon.read_bytes())}))
    cmd = [root / "scripts/run-kernel.sh", "--kernel", kernel, "--release", release, "--serial"]
    with native["guest"]["boot"](disk, cmd, prompt=b"LixinOS Bash (C shell)") as serial:
        serial.until(b"$ ", timeout=90)
        serial.expect(b"cd /\r", native["shell"])
        def command(line, expected=b"", status=0):
            native["command"](serial, line, expected=expected, status=status)
        if binary == "node26":
            command("node --version", b"\nv22.23.5\n")
        command(f"{binary} --version", f"\n{version}\n".encode())
        command(f"{binary} -p 'process.platform + \"/\" + process.arch'", b"\nlixinos/arm64\n")
        command(f"{binary} -p '6 * 7'", b"\n42\n")
        command(f"""{binary} -e 'process.title = "demo"; console.log(process.execPath)'""", f"\n/bin/{binary}\n".encode())
        command(f"""echo 'console.log(7 * 6)' | {binary}""", b"\n42\n")
        if addon:
            command(f"""{binary} -p 'require("/probe.node")(17,25)'""", b"\n42\n")
        if wasm:
            module = "0061736d0100000001070160027f7f017f030201000707010373756d00000a09010700200020016a0b"
            command(f"""{binary} -e 'WebAssembly.instantiate(Buffer.from("{module}","hex")).then(x=>console.log(x.instance.exports.sum(17,25)))'""", b"\n42\n")
        if openssl:
            command(f"""{binary} -p 'require("node:crypto").createHash("sha256").update("abc").digest("hex")'""",
                    b"\nba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n")
        serial.send((binary + "\r").encode())
        assert f"Welcome to Node.js {version}".encode() in serial.until(b"> ", timeout=30)
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
        in_dir(f"""{binary} -p 'require("./double.cjs")(21)'""", b"\n42\n")
        in_dir(f"""{binary} -e 'require("node:fs").writeFileSync("note", Buffer.from("hello"))'""")
        in_dir(f"""{binary} -e 'require("node:fs/promises").readFile("note", "utf8").then(x => setTimeout(() => console.log(x), 10))'""", b"\nhello\n")
        in_dir("""echo 'import {readFile} from "node:fs/promises"; console.log(await readFile("note", "utf8"))' > main.mjs""")
        in_dir(f"{binary} main.mjs", b"\nhello\n")
        in_dir(f"{binary} -e 'process.exit(7)'", status=7)
        in_dir(f"""{binary} -e 'require("./absent.cjs")'""", b"MODULE_NOT_FOUND", status=1)
        native["shutdown"](serial)
    print(f"{kernel} {release}: Node CLI, REPL, CommonJS, ESM, filesystem, timers and exit status passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", required=True, type=Path)
    parser.add_argument("--command", choices=VERSIONS, default="node")
    parser.add_argument("--package", type=Path)
    parser.add_argument("--wasm", action="store_true", help="run a WebAssembly module in V8")
    parser.add_argument("--addon", type=Path, help="guest Node-API addon exporting add(a,b)")
    parser.add_argument("--openssl", action="store_true", help="also test the optional OpenSSL build")
    parser.add_argument("--kernel", choices=("both", "rust", "tcc"), default="both")
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    package = args.package or root / f"Kernel/target/{args.command}-port/package"
    seed = args.disk.resolve(strict=True)
    require_local(seed, seed.stat())
    release = args.release
    if release == "latest":
        release = subprocess.check_output(["python3", root/"scripts/kernel-release.py", "latest"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix="lixin-node-") as tmp:
        for kernel in ("rust", "tcc") if args.kernel == "both" else (args.kernel,):
            check(kernel, release, seed, package, Path(tmp), args.command, args.openssl, args.wasm, args.addon)


if __name__ == "__main__":
    main()
