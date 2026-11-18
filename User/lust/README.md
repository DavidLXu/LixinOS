# Lust for LixinOS ARM64

The system installs the combined compiler at /bin/lust and the example at
/src/lust/hello.rs. At the LixinOS shell run:

    lust build /src/lust/hello.rs -o hello
    ./hello

The expected output is "Hello from LixinOS Rust: 42". The compiler checks a
single UTF-8 Rust source file, generates C in memory, and links an ARM64 LXE1
executable with its built-in TinyCC backend. A successful build replaces the
destination atomically. The guest runtime supplies the executable's libc slots.

This port uses the Lust prototype at revision
65215d74458bb677ea65b520efe1529023a8e52f and TinyCC at
f830a1697e159056b75de40a60464bc43ee670a3.
It supports functions, structs, enums, matching, supported String and Vec
operations, and a subset of borrow checking. User-defined generics, traits,
Cargo dependencies and builds of the existing Rust shell are outside this
version's language support. Rust source input is limited to 32 KiB. Saved LXE1
files can occupy up to 1 MiB including their 16-byte header, with at most
512 KiB of code and 860 KiB of initialized and zero-filled data.

The host image build or scripts/build-user.sh creates
Kernel/target/user-build/lixinos-rust-arm64.tar.gz containing the guest
binary, example, and license notices. The binary requires a LixinOS kernel
with the sparse-buffer form of utility call 22, path resolution call 23,
and the expanded LXE1 runtime jump table.
The host can run python3 scripts/test-lust.py for the QEMU integration test.

Upstream sources: https://github.com/t3dotgg/lust-lang and
https://github.com/TinyCC/tinycc.
