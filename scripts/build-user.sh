#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cache="$PWD/Kernel/target/user-build"
mkdir -p "$cache"

# Arm's macOS toolchain includes newlib for freestanding C programs.
release=arm-gnu-toolchain-14.3.rel1-darwin-arm64-aarch64-none-elf
if ! command -v aarch64-none-elf-gcc >/dev/null; then
    if [[ ! -x "$cache/$release/bin/aarch64-none-elf-gcc" ]]; then
        stage=$(mktemp -d "$cache/toolchain.XXXXXX")
        url="https://developer.arm.com/-/media/Files/downloads/gnu/14.3.rel1/binrel/$release.tar.xz"
        curl -fsSL --retry 2 "$url" -o "$stage/toolchain.tar.xz"
        (cd "$stage" && echo '8636ad0e42acab8227974c8180916b114040f5c77f8b7f5480fe58af4911d484  toolchain.tar.xz' | shasum -a 256 -c -)
        tar -xf "$stage/toolchain.tar.xz" -C "$stage"
        mv "$stage/$release" "$cache/$release"
        unlink "$stage/toolchain.tar.xz"
        rmdir "$stage"
    fi
    export PATH="$cache/$release/bin:$PATH"
fi

revision=f498a16c7db6d4b2de200b3e0856528dfe0613c3
if [[ ! -d "$cache/micropython" ]]; then
    stage=$(mktemp -d "$cache/source.XXXXXX")
    git clone -q --depth 1 --branch v1.25.0 https://github.com/micropython/micropython.git "$stage/micropython"
    mv "$stage/micropython" "$cache/micropython"
    rmdir "$stage"
fi
[[ $(git -C "$cache/micropython" rev-parse HEAD) == "$revision" ]] || {
    echo 'MicroPython build cache has an unexpected revision.' >&2; exit 1;
}
# The MicroPython lock also pins the lwIP C sources for the native kernel.
git -C "$cache/micropython" submodule update --init --depth=1 lib/lwip
make -s -C User/python -j6 CROSS_COMPILE=aarch64-none-elf-

tcc_revision=f830a1697e159056b75de40a60464bc43ee670a3
if [[ ! -d "$cache/tinycc" ]]; then
    stage=$(mktemp -d "$cache/tinycc.XXXXXX")
    git init -q "$stage/src"
    git -C "$stage/src" fetch -q --depth 1 https://github.com/TinyCC/tinycc "$tcc_revision"
    git -C "$stage/src" checkout -q --detach FETCH_HEAD
    mv "$stage/src" "$cache/tinycc"
    rmdir "$stage"
fi
[[ $(git -C "$cache/tinycc" rev-parse HEAD) == "$tcc_revision" ]] || exit 1
make -s -C User/tcc
python3 scripts/tcc-sources.py "$cache"
python3 scripts/python-sources.py "$cache"

git_revision=0060d9cf5666f015b1067129bd874c6cc4c9c7ac
if [[ ! -d "$cache/libgit2" ]]; then
    stage=$(mktemp -d "$cache/libgit2.XXXXXX")
    git clone -q --depth 1 --branch v1.9.1 https://github.com/libgit2/libgit2.git "$stage/src"
    mv "$stage/src" "$cache/libgit2"
    rmdir "$stage"
fi
[[ $(git -C "$cache/libgit2" rev-parse HEAD) == "$git_revision" ]] || exit 1
make -s -C User/git -j6

if [[ ! -f "$cache/ls.elf" || User/ls/ls.c -nt "$cache/ls.elf" || User/file-time.h -nt "$cache/ls.elf" || User/console.h -nt "$cache/ls.elf" || User/tcc/api.c -nt "$cache/ls.elf" || User/tcc/lixin.h -nt "$cache/ls.elf" || User/ls/start.c -nt "$cache/ls.elf" || User/ls/link.ld -nt "$cache/ls.elf" ]]; then
    aarch64-none-elf-gcc -Os -ffreestanding -nostdlib -mgeneral-regs-only \
        -Wl,-T,User/ls/link.ld,-s,-z,max-page-size=4096,--build-id=none \
        -IUser/tcc -o "$cache/ls.elf" User/ls/ls.c User/ls/start.c User/tcc/api.c
fi

if [[ ! -f "$cache/agent-run.elf" || User/agent/run.c -nt "$cache/agent-run.elf" || User/agent/start.c -nt "$cache/agent-run.elf" || User/console.h -nt "$cache/agent-run.elf" || User/tcc/lixin.h -nt "$cache/agent-run.elf" || User/tcc/api.c -nt "$cache/agent-run.elf" || User/ls/link.ld -nt "$cache/agent-run.elf" ]]; then
    aarch64-none-elf-gcc -Os -ffreestanding -nostdlib -mgeneral-regs-only -ffunction-sections \
        -Wl,-T,User/ls/link.ld,-s,-z,max-page-size=4096,--build-id=none,--gc-sections \
        -IUser/tcc -o "$cache/agent-run.elf" User/agent/run.c User/agent/start.c User/tcc/api.c -lc -lgcc
fi
make -s -C User/foreground
make -s -C User/core
make -s -C User/desktop font ../../Kernel/target/user-build/nxeyes.elf ../../Kernel/target/user-build/stopwatch.elf

links_version=2.30
links_hash=c4631c6b5a11527cdc3cb7872fc23b7f2b25c2b021d596be410dadb40315f166
if [[ ! -f "$cache/links-$links_version/.verified-$links_hash" ]]; then
    stage=$(mktemp -d "$cache/links.XXXXXX")
    archive=links-$links_version.tar.bz2
    curl -fsSL --retry 2 "https://sources.buildroot.net/links/$archive" -o "$stage/$archive" ||
        curl -fsSL --retry 2 "https://links.twibright.com/download/$archive" -o "$stage/$archive"
    (cd "$stage" && echo "$links_hash  $archive" | shasum -a 256 -c -)
    tar -xjf "$stage/$archive" -C "$stage"
    touch "$stage/links-$links_version/.verified-$links_hash"
    if [[ -d "$cache/links-$links_version" ]]; then
        mv "$cache/links-$links_version" "$stage/previous-cache"
    fi
    mv "$stage/links-$links_version" "$cache/"
    rm -rf "$stage"
fi
bear="$cache/bearssl-0.6"
if [[ ! -f "$bear/.lixin-verified" ]]; then
    stage=$(mktemp -d "$cache/bear.XXXXXX")
    curl -fsSL --retry 2 https://sources.buildroot.net/bearssl/bearssl-0.6.tar.gz -o "$stage/archive"
    (cd "$stage" && echo '6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14  archive' | shasum -a 256 -c -)
    tar -xf "$stage/archive" -C "$cache"
    touch "$bear/.lixin-verified"
    rm -rf "$stage"
fi
make -s -j8 -C "$bear" build/brssl
make -s -j8 -C "$bear" BUILD=arm CC=aarch64-none-elf-gcc AR=aarch64-none-elf-ar CFLAGS="-Os -ffunction-sections -fdata-sections" arm/libbearssl.a
make -s -j6 -C User/links
lockf -k "$cache/cpython-build.lock" bash User/cpython/build.sh "$cache/cpython-port"

make -s -C User/ssh
make -s -C User/sh

# Pin the original single-file Lust pipeline. Host daemons and incremental
# disk caches are unnecessary for the guest's one-shot compiler.
lust_revision=65215d74458bb677ea65b520efe1529023a8e52f
if [[ ! -d "$cache/lust-src/.git" ]]; then
    stage=$(mktemp -d "$cache/lust.XXXXXX")
    git init -q "$stage/src"
    git -C "$stage/src" fetch -q --depth 1 https://github.com/t3dotgg/lust-lang "$lust_revision"
    git -C "$stage/src" checkout -q --detach FETCH_HEAD
    mv "$stage/src" "$cache/lust-src"
    rmdir "$stage"
fi
[[ $(git -C "$cache/lust-src" rev-parse HEAD) == "$lust_revision" ]] || exit 1
make -s -C User/lust
