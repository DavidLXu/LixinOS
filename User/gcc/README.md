# GNU C on LixinOS

The Rust and TCC kernels run the same native AArch64 toolchain. Start a shell in either system and try:

```sh
echo '#include <stdio.h>' > hello.c
echo 'int main(void) { puts("Hello from GCC"); return 0; }' >> hello.c
gcc -std=c11 -O2 -Wall hello.c -o hello
./hello
```

The upstream GCC 4.8.5 driver runs its C frontend, GNU assembler, and GNU linker as guest processes. The driver supports separate compilation, `-E`, `-S`, `-c`, `-pipe`, compiler diagnostics, and linking multiple objects. GNU Binutils is version 2.43.1. The C headers, musl static library, LixinOS adapters, startup objects, and linker script are installed in `/lib/gcc`. The assembler and linker use BFD's seekable memory stream and publish each completed output file through the guest filesystem.

The output is a statically linked LixinOS ELF executable. Its code and read-only data share the 2 MiB executable bank. Writable static data and BSS have a separate bank ending at the utility mailbox. Stack and dynamic allocations use the private process arena. The utility startup ABI passes the arguments following the program name. The C runtime supplies an empty string for `argv[0]` when the name is unavailable.

This distribution builds the GCC 4.8.5 C frontend with its C11 feature set. For example, thread-local variables use the GNU spelling `__thread` in this release. C++, Fortran, shared libraries and building GCC itself inside the guest are outside the tested build. The `aarch64-none-elf` target uses LixinOS-specific defaults. GCC and its dependencies are cross-built on macOS for the initial installation. Compiling C programs after installation works offline inside LixinOS.

## Build and install

Run these commands from the repository root on the Mac. The build uses the same Arm GNU cross compiler and adapted musl sysroot as the CPython port. Release archives are downloaded and SHA-256 checked in `Kernel/target/gcc-port`.

```sh
User/gcc/build.sh
python3 User/gcc/install.py Kernel/target/gcc-port/current-package data/lixinos.img
python3 User/gcc/install.py Kernel/target/gcc-port/current-package data/tcc-os/system.img
```

Install onto an initialized, powered-off disk. The installer verifies its active filesystem snapshot and the new snapshot before committing to the inactive slot. It checks an installed-file manifest on subsequent upgrades. The normal Rust launcher builds the kernel with support for the new ELF layout.

The end-to-end tests use disposable copies of the system disks. They check preprocessor output, assembly, piped compilation, linking two translation units, diagnostics, argument passing, exit codes, and execution after reboot.

```sh
python3 scripts/test-gcc.py rust
python3 scripts/test-gcc.py tcc
```
