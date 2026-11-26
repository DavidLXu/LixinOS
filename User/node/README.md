# Node.js for LixinOS

The guest packages provide two independent upstream ARM64 runtimes. Run `node` for Node.js 22.23.5 or `node26` for Node.js 26.13.1 LTS. The daily disk installs each as a position-independent executable linked with the guest musl runtime. The CLI, V8 and libuv execute in a guest process. Its core JavaScript modules are embedded in the executable. It uses the guest filesystem, clocks, kernel-scheduled threads and pipes.

```sh
node --version
node26 --version
node -p '6 * 7'
node -e 'require("node:fs/promises").readFile("/hello.txt", "utf8").then(console.log)'
node app.mjs
```

V8 uses precompiled builtins and non-executable data pages. Both daily builds can load AArch64 Node-API addons linked for the guest ABI. Node 26 runs WebAssembly with V8's DrumBrake interpreter. Node 22 uses V8 Lite. libuv uses the POSIX poll backend and pipe wakeups. OpenSSL is an optional build setting. The ports omit ICU, the inspector, SQLite and Corepack. The daily disk installs [npm and npx](../npm/README.md) as a separate package that runs on Node 26.

## Build

On the LixinOS workstation, run `bash User/node/prepare-sdk.sh`. This exports the guest startup objects, musl and syscall adapter into `Kernel/target/node-port/sdk`.

Use an AArch64 Linux musl build machine for the upstream generators. On Alpine, install `build-base linux-headers ninja py3-packaging python3 patch curl xz`. Copy the exported SDK and `User/node` there. From the directory that contains `sdk/` and `node/`, run the following commands.

```sh
JOBS=6 sh node/build-upstream.sh
python3 node/link.py node-build/node-v22.23.5 sdk node-lixinos.elf
```

For DSH and other HTTPS clients, set `NODE_OPENSSL=1` on the build command above and install Perl for the OpenSSL build tools. The guest uses the CPU capabilities supplied by its ELF loader. Hostname lookups with `AI_ADDRCONFIG` use the guest IPv4 stack.

The first step downloads and checks the pinned source archive. It creates a native Linux builder executable as part of the upstream build. The second step uses its archive list to link the LixinOS ELF against the exported guest runtime.

Copy `node-lixinos.elf` back to the workstation. Use an extracted copy of the same upstream source tree for its bundled license and a fresh output directory.

```sh
python3 User/node/package.py node-lixinos.elf /path/to/node-v22.23.5 Kernel/target/node-port/package
python3 scripts/test-node.py --disk "/path/to/powered-off.img"
python3 User/gcc/install.py --toolchain node Kernel/target/node-port/package "/path/to/powered-off.img"
```

For the static package, the launcher expands its two compressed segments directly into the guest arena and verifies their lengths and checksums before transferring control. The integration test installs the package into disposable copies and boots each kernel.

## Side-by-side Node 26 build

Run these commands on the Alpine builder with the exported SDK. This example
includes OpenSSL and needs Perl. A separate work directory preserves a Node 22
build on the same machine.

```sh
NODE_PAGED_VM=1 NODE_WORK="$PWD/node26-build" NODE_VERSION=26.13.1 NODE_OPENSSL=1 JOBS=6 sh node/build-upstream.sh
python3 node/link.py --dynamic node26-build/node-v26.13.1 sdk node26-lixinos.elf
```

Use the daily build-volume image (`LIXBLD01`) for Node 26. V8 reserves
256 MiB for its JavaScript dispatch table. On the small desktop test volume
with a 128 MiB process arena, `node26 --version` works but starting a
JavaScript isolate fails. The daily launch profile provides the larger arena.

Export the dynamic SDK with `NODE_PAGED_VM=1 bash User/node/prepare-sdk.sh` before copying it to the builder. Install the dynlink and matching C++ runtime packages on the guest. This build also omits
Temporal, single-executable application generation and V8 postmortem metadata.
After copying the guest ELF back to the Mac, package it under the versioned
name. Use a disk that already contains the Node 22 package for the coexistence
test. Power off the guest before installing into its working disk.

```sh
pkg=Kernel/target/node26-port/package
python3 User/node/package.py --dynamic --command node26 node26-lixinos.elf /path/to/node-v26.13.1 "$pkg"
python3 scripts/test-node.py --disk "/path/to/powered-off.img" --command node26 --openssl
python3 User/gcc/install.py --toolchain node26 "$pkg" "/path/to/powered-off.img"
```
