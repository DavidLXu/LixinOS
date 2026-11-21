#!/bin/sh
# Build Node and its native V8 snapshot generators on AArch64 Linux with musl.
set -eu
port=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=${NODE_WORK:-"$PWD/node-build"}
mkdir -p "$work"
cd "$work"
version=22.23.5
digest=6cce7c58bf058700c643944903d60a325e8c9ceede1f5c2c088b435f2ada091a
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
if ! patch --batch --forward -p1 -R --dry-run < "$port/lixinos.patch" >/dev/null 2>&1; then
    patch --batch --forward -p1 < "$port/lixinos.patch"
fi
export CPPFLAGS="${CPPFLAGS:-} -D__lixinos__"
./configure --ninja --fully-static --v8-lite-mode --v8-enable-snapshot-compression \
    --without-ssl --without-intl --without-sqlite --without-inspector --without-amaro \
    --without-node-snapshot --without-node-code-cache --without-npm --without-corepack \
    --v8-options='--single-threaded --max-old-space-size=32 --max-semi-space-size=1'
ninja -C out/Release -j "${JOBS:-6}" node
