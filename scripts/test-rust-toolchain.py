"""Compile a project with a path dependency and build script on a disposable system disk."""
import argparse
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
test = runpy.run_path(str(root / "scripts/test-native-kernel.py"))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("kernel", choices=("rust", "tcc"))
parser.add_argument("--package", type=Path, default=root / "Kernel/target/rust-port/current-package")
parser.add_argument("--registry", action="store_true", help="also fetch, build and run itoa from crates.io")
parser.add_argument("--seed", type=Path)
args = parser.parse_args()
native = args.kernel == "tcc"
seed = args.seed or root / ("data/tcc-os/system.img" if native else "data/lixinos.img")

with tempfile.TemporaryDirectory(prefix="lx-rust-") as tmp:
    disk = Path(tmp) / "guest.img"
    subprocess.run(["cp", "-c", seed, disk], check=True)
    subprocess.run([sys.executable, root/"User/gcc/install.py", "--toolchain", "rust", args.package, disk], check=True)
    def boot():
        if native:
            return test["native_boot"](root / "data/tcc-os/loader.elf", disk, prompt=test["shell"])
        return test["guest"]["boot"](disk)
    project = f"/tmp/{Path(tmp).name}"
    with boot() as s:
        def cmd(line, expect=b"", status=0):
            s.send(line.encode() + b"\r")
            out = s.until(test["shell"], timeout=360)
            print(out.decode(errors="replace"), flush=True)
            assert expect in out, out
            s.send(b"echo $?\r")
            assert f"\n{status}\n".encode() in s.until(test["shell"]), line
        cmd("rustc -vV", b"Cranelift version")
        cmd("cargo -V", b"cargo 1.100.0")
        cmd(f"cargo new --name demo --vcs none {project}")
        cmd(f"cargo new --lib --name arithmetic --vcs none {project}/arithmetic")
        cmd(f"""echo 'arithmetic = {{ path = "arithmetic" }}' >> {project}/Cargo.toml""")
        cmd(f"""echo 'pub fn sum(a: i32, b: i32) -> i32 {{ a + b }}' > {project}/arithmetic/src/lib.rs""")
        cmd(f"""echo 'fn main() {{ println!("cargo:rustc-env=GUEST_PACKAGE={{}}", std::env::var("CARGO_PKG_NAME").unwrap()); }}' > {project}/build.rs""")
        cmd(f"""echo 'fn main() {{ println!("{{}}={{}}", env!("GUEST_PACKAGE"), arithmetic::sum(40, 2)); }}' > {project}/src/main.rs""")
        cmd(f"cargo run --offline --manifest-path {project}/Cargo.toml", b"\ndemo=42\n")
        cmd(f"""echo 'pub fn sum(a: i32, b: i32) -> i32 {{ a + b + 1 }}' > {project}/arithmetic/src/lib.rs""")
        cmd(f"cargo run --offline --manifest-path {project}/Cargo.toml", b"\ndemo=43\n")
        cmd(f"""echo '#[test] fn worker() {{ assert_eq!(std::thread::spawn(|| arithmetic::sum(40, 2)).join().unwrap(), 43); }}' >> {project}/src/main.rs""")
        cmd(f"cargo test --offline --manifest-path {project}/Cargo.toml", b"1 passed; 0 failed")
        cmd(f"echo 'fn main() {{ nope(); }}' > {project}/broken.rs")
        cmd(f"rustc {project}/broken.rs", b"cannot find function", 1)
        if args.registry:
            cmd(f"""echo 'itoa = "=1.0.19"' >> {project}/Cargo.toml""")
            cmd(f"""echo 'fn main() {{ println!("registry={{}}", itoa::Buffer::new().format(arithmetic::sum(40, 1))); }}' > {project}/src/main.rs""")
            cmd(f"cargo run --manifest-path {project}/Cargo.toml", b"\nregistry=42\n")
        test["shutdown"](s)
    with boot() as s:
        test["command"](s, project+"/target/debug/demo", expected=b"\nregistry=42\n" if args.registry else b"\ndemo=43\n")
        test["shutdown"](s)
