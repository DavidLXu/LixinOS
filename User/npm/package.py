"""Stage the npm CLI and its bundled dependencies from the Node 26 source release."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

port = Path(__file__).resolve().parent
root = port.parents[1]


def build(source, output):
    npm = source / "deps/npm"
    manifest = json.loads((npm / "package.json").read_text())
    if manifest["name"] != "npm" or manifest["version"] != "11.20.0":
        raise ValueError("expected npm 11.20.0 from Node.js v26.13.1")
    if output.exists():
        raise FileExistsError(output)
    compiler = shutil.which("aarch64-none-elf-gcc")
    if not compiler:
        compiler, = (root / "Kernel/target/user-build").glob("arm-gnu-*/bin/aarch64-none-elf-gcc")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        stage = Path(tmp) / "stage"
        lib = stage / "lib/npm"
        lib.mkdir(parents=True)
        for name in ("bin", "lib", "node_modules", "docs", "man"):
            shutil.copytree(npm / name, lib / name)
        for name in ("index.js", "package.json", "LICENSE", "README.md"):
            shutil.copyfile(npm / name, lib / name)
        shutil.copyfile(port / "npmrc", lib / "npmrc")
        for patch in ("guest-network.patch", "guest-cache.patch"):
            subprocess.run(["patch", "-p1", "-i", port / patch], cwd=lib, check=True)
        (stage / "bin").mkdir()
        for cli in ("npm", "npx"):
            subprocess.run([compiler, "-Os", "-ffreestanding", "-fno-builtin", "-nostdlib",
                            "-mgeneral-regs-only", "-Wl,--build-id=none,-s,-z,max-page-size=4096",
                            "-T", port.parent / "ls/link.ld", f'-DLX_NPM_CLI="{cli}"',
                            port / "launcher.c", "-o", stage / "bin" / cli], check=True)
        stage.rename(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    build(args.source, args.output.resolve())
