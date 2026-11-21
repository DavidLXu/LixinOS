# Node.js for LixinOS

The optional `/bin/node` package runs the upstream ARM64 Node.js 22.23.5 CLI, V8 and libuv in a guest process. Its core JavaScript modules are embedded in the executable. It uses the guest filesystem, clocks, cooperative threads and pipes.

```sh
node --version
node -p '6 * 7'
node -e 'require("node:fs/promises").readFile("/hello.txt", "utf8").then(console.log)'
node app.mjs
```

V8 uses the interpreter and precompiled builtins. The loader seals executable text, and V8 uses eagerly backed, non-executable memory for its data pages. The guest has no per-page protection inside that heap. libuv uses its POSIX poll backend and pipe wakeups. Console output uses cooked text. This configuration omits OpenSSL (including `node:crypto`, TLS and HTTPS), ICU, WebAssembly, the inspector, SQLite, npm, Corepack and native shared-library addons.

## Build

On the LixinOS workstation, run `bash User/node/prepare-sdk.sh`. This exports the guest startup objects, musl and syscall adapter into `Kernel/target/node-port/sdk`.

Use an AArch64 Linux musl build machine for the upstream generators. On Alpine, install `build-base linux-headers ninja py3-packaging python3 patch curl xz`. Copy the exported SDK and `User/node` there. From the directory that contains `sdk/` and `node/`, run the following commands.

```sh
JOBS=6 sh node/build-upstream.sh
python3 node/link.py node-build/node-v22.23.5 sdk node-lixinos.elf
```

The first step downloads and checks the pinned source archive. It creates a native Linux builder executable as part of the upstream build. The second step uses its archive list to link the LixinOS ELF against the exported guest runtime.

Copy `node-lixinos.elf` back to the workstation. Use an extracted copy of the same upstream source tree for its bundled license and a fresh output directory.

```sh
python3 User/node/package.py node-lixinos.elf /path/to/node-v22.23.5 Kernel/target/node-port/package
python3 scripts/test-node.py --disk "/path/to/powered-off.img"
python3 User/gcc/install.py --toolchain node Kernel/target/node-port/package "/path/to/powered-off.img"
```

The launcher expands its two compressed segments directly into the guest arena and verifies their lengths and checksums before transferring control. The integration test installs the package into disposable copies and boots each kernel.
