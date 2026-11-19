"""Exercise the native Rust bootstrap ABI and restart a disposable build disk."""
from pathlib import Path
import argparse
import runpy
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
driver = runpy.run_path(str(root / "User/rust/run-bootstrap.py"))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("seed", type=Path)
args = parser.parse_args()

with args.seed.open("rb") as image:
    if image.read(8) != b"LIXBLD01":
        parser.error("expected a bootstrapped LIXBLD01 build volume")

with tempfile.TemporaryDirectory(prefix="lixin-rust-volume-") as tmp:
    disk = Path(tmp) / "test.img"
    subprocess.run(["cp", "-c", args.seed, disk], check=True)
    launcher = [root / "scripts/run-kernel.sh", "--kernel", "rust", "--serial"]
    def shutdown(serial):
        serial.send(b"q\r")
        serial.until(b"Shutting down LixinOS", timeout=600)
        assert serial.vm.wait(timeout=60) == 0
    with driver["guest"]["boot"](disk, launcher) as s:
        def check(line):
            assert driver["command"](s, line) == 0, line
        check("mkdir /scratch/abi-test")
        prefix = "/scratch/abi-test"
        check(f"""echo 'extern crate proc_macro; use proc_macro::TokenStream; #[proc_macro] pub fn answer(_:TokenStream)->TokenStream {{ "42".parse().unwrap() }}' > {prefix}/m.rs""")
        check(f"rustc --crate-name guest_macro --crate-type proc-macro {prefix}/m.rs -C link-arg=-T/bootstrap/large.ld -o {prefix}/libguest_macro.so")
        check(f"""echo 'static DATA:[u8;3145728]=[42;3145728];fn main() {{ let p=std::env::current_exe().unwrap(); assert!(p.ends_with("abi-test/probe")); let mut v=vec![0u8;700<<20]; *v.last_mut().unwrap()=guest_macro::answer!(); assert_eq!(std::hint::black_box(&DATA)[17],*v.last().unwrap()); println!("macro, large ELF and 700 MiB allocation passed"); }}' > {prefix}/probe.rs""")
        check(f"rustc --edition 2024 {prefix}/probe.rs --extern guest_macro={prefix}/libguest_macro.so -C link-arg=-T/bootstrap/large.ld -o {prefix}/probe")
        check(prefix + "/probe")
        shutdown(s)
    with driver["guest"]["boot"](disk, launcher) as s:
        assert driver["command"](s, prefix + "/probe") == 0
        shutdown(s)
