"""Compile Rust inside a fresh guest and run its saved ARM64 executable after reboot."""
from pathlib import Path
import runpy
import tempfile

ROOT = Path(__file__).resolve().parents[1]
guest = runpy.run_path(str(ROOT / "scripts/test-user.py"))


def command(serial, line):
    serial.send(line.encode() + b"\r")
    return serial.until(guest["SHELL"]).split(b"\n", 1)[1]


def large_images(s):
    def save_source(name, source):
        assert not command(s, f"echo '{source}' > {name}")

    # Exercise RX bytes past the old immediate-run buffer and touch the final
    # byte of BSS. Derive linker overhead rather than hard-coding code size.
    def build_padded(data_bytes):
        save_source("large.c", f'asm(".space 262160"); char data[{data_bytes}]; '
                    'int main(void) { data[sizeof(data)-1]=42; return data[sizeof(data)-1]; }')
        return command(s, "tcc large.c -o large")

    initial_data = 640 * 1024
    assert not build_padded(initial_data)
    initial_size = int(command(s, "ls -l large").split()[1])
    max_data = initial_data + 1024 * 1024 - initial_size
    assert not build_padded(max_data)
    assert int(command(s, "ls -l large").split()[1]) == 1024 * 1024
    assert b"exceeds 1 MiB" in build_padded(max_data + 16)
    assert command(s, "./large") == b"[exit 42]\n"

    # Generate a Rust literal inside the guest, without a 31 KB serial line.
    save_source("gen.c", "#include <stdio.h>")
    generator = (r'int main(void){printf("fn main() { println!(\"{}\", \"");'
                 r'for(int i=0;i<31000;i++) putchar(97); puts("\".len()); }"); return 0;}')
    assert not command(s, f"echo '{generator}' >> gen.c")
    assert not command(s, "tcc gen.c -o gen")
    assert not command(s, "./gen > large.rs")
    assert not command(s, "lust build large.rs -o large-rust")
    assert int(command(s, "ls -l large-rust").split()[1]) > 32 * 1024
    assert command(s, "./large-rust") == b"31000\n"


def test(disk):
    with guest["boot"](disk) as s:
        assert not command(s, "lust build /src/lust/hello.rs -o hello")
        assert command(s, "./hello") == b"Hello from LixinOS Rust: 42\n"
        source = 'fn main() { let mut v = vec![3, 4]; v.push(5); println!("{}", v[0] + v[2]); }'
        command(s, f"echo '{source}' > vec.rs")
        assert not command(s, "lust build vec.rs -o vec")
        assert command(s, "./vec") == b"8\n"
        command(s, "echo 'fn main() { let n: bool = 42; }' > bad.rs")
        assert b"[exit 1]" in command(s, "lust build bad.rs -o hello")
        assert command(s, "echo $?") == b"1\n"
        assert b"overwrite source" in command(s, "lust build vec.rs -o ./vec.rs")
        assert source.encode() in command(s, "cat vec.rs")
        command(s, """echo 'fn main() { let a = String::from("x"); let b = a; println!("{}", a); }' > moved.rs""")
        assert b"[exit 1]" in command(s, "lust build moved.rs -o hello")
        large_images(s)
        s.send(b"q\r")
        assert s.vm.wait(timeout=10) == 0
    with guest["boot"](disk) as s:
        assert command(s, "./hello") == b"Hello from LixinOS Rust: 42\n"
        assert command(s, "./vec") == b"8\n"
        assert command(s, "./large") == b"[exit 42]\n"
        assert command(s, "./large-rust") == b"31000\n"


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        test(Path(tmp) / "guest.img")
    print("Native Lust compile, diagnostics and reboot checks passed.")
