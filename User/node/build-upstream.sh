#!/bin/sh
# Build Node and its native V8 snapshot generators on AArch64 Linux with musl.
set -eu
port=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=${NODE_WORK:-"$PWD/node-build"}
mkdir -p "$work"
cd "$work"
version=${NODE_VERSION:-22.23.5}
case "$version" in
    22.23.5)
        digest=6cce7c58bf058700c643944903d60a325e8c9ceede1f5c2c088b435f2ada091a
        port_patch=lixinos.patch
        set -- --without-corepack
        ;;
    26.13.1)
        digest=a7f6204ff63b6a2cd274d48903285abb86ed53a99070e532d4c2fd787457d9e6
        port_patch=lixinos26.patch
        # Postmortem metadata references Wasm-only V8 fields in this release.
        export GYP_DEFINES="${GYP_DEFINES:-} v8_postmortem_support=0"
        set -- --v8-disable-temporal-support --without-lief --disable-single-executable-application
        ;;
    *) echo "Unsupported Node.js version: $version" >&2; exit 2 ;;
esac
archive="node-v$version.tar.xz"
if [ ! -f "$archive" ]; then
    curl -fLsS --retry 3 "https://nodejs.org/dist/v$version/$archive" -o "$archive.tmp"
    printf '%s  %s\n' "$digest" "$archive.tmp" | sha256sum -c -
    mv "$archive.tmp" "$archive"
fi
printf '%s  %s\n' "$digest" "$archive" | sha256sum -c -
if [ ! -d "node-v$version" ]; then
    stage=$(mktemp -d "$PWD/extract.XXXXXX")
    trap 'rm -rf "$stage"' EXIT
    tar -xf "$archive" -C "$stage"
    mv "$stage/node-v$version" .
    rmdir "$stage"
    trap - EXIT
fi
cd "node-v$version"
if ! patch --batch --forward -p1 -R --dry-run < "$port/$port_patch" >/dev/null 2>&1; then
    patch --batch --forward -p1 < "$port/$port_patch"
fi
case "${NODE_OPENSSL:-0}" in
    0) set -- "$@" --without-ssl ;;
    1)
        if ! patch --batch --forward -R -p1 --dry-run < "$port/openssl-armcap.patch" >/dev/null 2>&1; then
            patch --batch --forward -p1 < "$port/openssl-armcap.patch"
        fi
        ;;
    *) echo 'NODE_OPENSSL must be 0 or 1' >&2; exit 2 ;;
esac
export CPPFLAGS="${CPPFLAGS:-} -D__lixinos__"
./configure --ninja --fully-static --v8-lite-mode --v8-enable-snapshot-compression \
    "$@" --without-intl --without-sqlite --without-inspector --without-amaro \
    --without-node-snapshot --without-node-code-cache --without-npm \
    --v8-options='--single-threaded --max-old-space-size=32 --max-semi-space-size=1'
ninja -C out/Release -j "${JOBS:-6}" node
