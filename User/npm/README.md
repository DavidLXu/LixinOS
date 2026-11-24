# npm on LixinOS

The daily disk contains npm and npx 11.20.0 from the Node.js 26.13.1 source release. Both launchers use `/bin/node26` and load their bundled dependencies from `/lib/npm`. Packages installed in a project go in its `node_modules` directory.

```sh
mkdir demo
cd demo
npm init -y
npm install is-number@7.0.0
node26 -p 'require("is-number")(42)'
npm ls --depth=0
```

The port uses HTTPS with certificate verification and ordinary TCP sockets. npm skips the persistent Node compile cache because writing each cache entry commits a guest filesystem snapshot. The built-in npm configuration disables the update notification and executable symlinks. Local JavaScript packages can be loaded directly with Node. Commands that spawn shell scripts, including `npm run` and some package install scripts, are limited by the guest Node process implementation.

## Build and install

From the Mac repository root, use the extracted, checksum-verified Node 26 source tree and a work disk that already has Node 26 installed. Power off the work disk before installation.

```sh
pkg=Kernel/target/npm-port/package
python3 User/npm/package.py Kernel/target/node26-port/node-v26.13.1 "$pkg"
python3 User/gcc/install.py --toolchain npm "$pkg" "$HOME/Library/Application Support/LixinOS/daily.img"
python3 scripts/test-npm.py --disk "$HOME/Library/Application Support/LixinOS/daily.img"
```

The test boots an APFS copy with the Rust kernel, installs a pinned package from the registry, and boots the same copy with the TCC kernel to check the dependency and lockfile.
