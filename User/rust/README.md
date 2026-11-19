# Rust and Cargo on LixinOS

Both kernels use the same user-space package. The upstream Rust 1.100.0 frontend handles Rust 2024 and links the in-tree AArch64 Cranelift backend into the compiler. The sysroot includes the matching Rust standard library. Cargo 1.100.0 runs build scripts, invokes rustc and runs the resulting programs in the guest. The guest GCC driver invokes GNU ld to produce static AArch64 ELF executables.

## Use in the guest

```sh
rustc -vV
cargo -V
cargo new --vcs none demo
cd demo
cargo run
```

Use local path dependencies for offline work. Cargo uses the sparse crates.io registry over HTTPS, the existing CPython CA bundle, and a cache under `/.cargo`. Source archives use the guest filesystem's timestamps and permissions. Cached dependencies and a compatible lockfile can be used with `cargo run --offline`. The launcher selects one build job, disables incremental compilation and omits debug information in the dev profile to fit the system disk.

The target name is `aarch64-unknown-linux-musl` for compatibility with the pinned standard library. The link configuration selects the LixinOS startup code and syscall adapters. Programs that require OS-specific Linux interfaces need porting. The default linker script reserves 2 MiB for code and read-only data and roughly 1.84 MiB for writable data. On a dedicated build volume, `-C link-arg=-T/bootstrap/large.ld` selects a separate program region up to 512 MiB including BSS. Procedural macros run as static guest worker processes. The installation includes rustc and Cargo. Rustdoc, Clippy and rustfmt are outside this package.

## Rebuild and install from the Mac

Start with the CPython port's musl/OpenSSL build and the current [guest GCC toolchain](../gcc/README.md). Install its updated package even on older GCC-equipped disks. Its linker script supplies the thread-local storage alignment used by the Rust standard library. In the repository root run:

```sh
rustup toolchain install 1.100.0 --target aarch64-unknown-linux-musl
RUSTUP_TOOLCHAIN=1.100.0 User/rust/build.sh
python3 User/gcc/install.py --toolchain rust Kernel/target/rust-port/current-package data/lixinos.img
python3 User/gcc/install.py --toolchain rust Kernel/target/rust-port/current-package data/tcc-os/system.img
```

Use initialized, powered-off disks. Boot the current Rust kernel or rebuild the [TCC disk kernel](../../Kernel/native/README.md) from the matching sources before using these tools. The installer checks the previous manifest, disk capacity and new snapshot before publishing it. The package needs approximately 108 MiB in addition to GCC and the other files on the disk.

The initial compiler and Cargo are cross-built on the Mac. On a dedicated build disk they act as stage0 and compile both tools from source under the selected kernel. The resulting stage1 Cargo and rustc can compile and run a Rust 2024 project with a build script. This bootstrap uses the pinned standard library from the distribution and the installed C SDK and OpenSSL libraries. Rebuilding the standard library and performing a further stage2 compiler rebuild are separate steps.

## Verify on disposable disk copies

```sh
python3 scripts/test-rust-toolchain.py rust
python3 scripts/test-rust-toolchain.py tcc
```

The tests compile a project with a local library dependency and a build script, change the dependency, run a threaded unit test, check a compiler diagnostic and run the saved program after reboot. Add `--registry` to fetch and compile the published `itoa` crate over HTTPS and run the result.

## Native compiler and Cargo build

The Rust and TCC kernels can mount a dedicated `LIXBLD01` build volume. The launcher assigns 12 GiB of guest RAM to Rust and 24 GiB to TCC. The sparse image contains two alternating 4 GiB
snapshot slots. The active filesystem holds up to 4 GiB less its 512-byte slot header, including record overhead. The maximum file size is 512 MiB. Changes under
`/scratch` are written back at normal shutdown or when a persistent file is
committed. A forced VM exit discards changes since the last checkpoint.

Start with a powered-off copy of an installed system disk. Prepare the patched
upstream source trees, the current GCC package (including the GNU archive
tools), and the matching stage0 package. Use Python 3.11 or newer to prepare
the offline registry from the two Cargo lockfiles and the host's Cargo cache.

```sh
bash User/gcc/build.sh rt
bash User/gcc/build.sh binutils
bash User/gcc/build.sh gcc
bash User/gcc/build.sh package
RUSTUP_TOOLCHAIN=1.100.0 User/rust/build.sh all
cp -c data/lixinos.img /tmp/lixin-stage0.img
python3 User/gcc/install.py Kernel/target/gcc-port/current-package /tmp/lixin-stage0.img
python3 User/gcc/install.py --toolchain rust Kernel/target/rust-port/current-package /tmp/lixin-stage0.img
python3.13 User/rust/bootstrap-disk.py /tmp/lixin-stage0.img /tmp/lixin-rust-build.img
python3 User/rust/run-bootstrap.py /tmp/lixin-rust-build.img both
```

For a TCC build, start with a powered-off copy of the TCC daily disk and install the current GCC and Rust stage0 packages into that copy as above. Supply the clean TCC boot disk from a build-capable paired release. The preparation step copies its loader, kernel and kernel sources, and writes a matching `.loader.elf` beside the new build disk. The initial image has no stage1 binaries or Cargo target cache.

```sh
python3.13 User/rust/bootstrap-disk.py /tmp/tcc-stage0.img data/tcc-bootstrap.img --tcc-boot-disk /path/to/restored/data/tcc-os/system.img
python3 User/rust/run-bootstrap.py data/tcc-bootstrap.img both --kernel tcc
LIXINOS_TCC_DISK="$PWD/data/tcc-bootstrap.img" ./scripts/run-kernel.sh --kernel tcc --serial
```

The source workspaces, local registry and linker script are under `/bootstrap`.
The runner boots LixinOS and compiles the build driver there. Guest Cargo
invokes guest rustc, GCC, GNU as, ar and ld. Use `rustc` or `cargo` in place
of `both` to resume one component. The driver publishes successful builds
at `/stage1/rustc` and `/stage1/cargo`. The `verify` command explicitly selects
both stage1 binaries, builds a fresh Rust 2024 project with a build script, and checks for `stage1=42`.
The runner then reboots the same image, runs the saved program and invokes
stage1 Cargo directly from the shell. The image's `/.cargo/config.toml` selects
one build job and disables incremental compilation, which uses filesystem
locks that the guest does not provide. Bootstrap uses the seeded standard
library, C SDK and OpenSSL development files.

To boot the prepared disk from the repository root on the Mac:

```sh
LIXINOS_DISK="$PWD/data/rust-bootstrap.img" ./scripts/run-kernel.sh --kernel rust --serial
```

Inside LixinOS, the absolute paths select the guest-built tools. Ordinary
`rustc` and `cargo` still select the seed tools under `/bin`.

```sh
/stage1/rustc -vV
/stage1/cargo -V
/scratch/bootstrap verify
```

Enter `q` at the shell and press Return before opening this image in another VM.
