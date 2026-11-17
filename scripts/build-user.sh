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
