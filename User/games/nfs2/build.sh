#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../../.." && pwd)
export RA_SDK="$root/Kernel/target/user-build"
base="$RA_SDK/nfs2-port"
mkdir -p "$base"
fetch() {
    local path=$1 url=$2 revision=$3
    if [[ ! -d $path/.git ]]; then git init -q "$path"; fi
    if ! git -C "$path" cat-file -e "$revision^{commit}" 2>/dev/null; then
        git -C "$path" fetch --depth=1 "$url" "$revision"
    fi
    git -C "$path" checkout -q --detach "$revision"
}
fetch "$base/recomp" https://github.com/motor-dev/nfs-recompiled 03a1c9390176de344b69616db282e0bc5737fa30
fetch "$base/sse2neon" https://github.com/DLTcollab/sse2neon 2272251cc3ad9cba8b8c47c4065811cfb396b802
if ! git -C "$base/recomp" apply --reverse --check "$port/lixinos.patch" 2>/dev/null; then
    git -C "$base/recomp" apply "$port/lixinos.patch"
fi
sdl="$RA_SDK/ra2-engine-port"
[[ -f "$sdl/build/sdl/libSDL2.a" ]] || bash "$port/../ra2-native/build.sh" ra2-engine
cross=("$RA_SDK"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
cmake -S "$port" -B "$base/build" -DCMAKE_TOOLCHAIN_FILE="$port/../ra2-native/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DSOURCE="$base/recomp" -DNEON="$base/sse2neon" \
    -DSDL="$sdl/sdl-src" -DSDL_BUILD="$sdl/build/sdl"
cmake --build "$base/build" -j "${JOBS:-8}"
if [[ $# -eq 1 ]]; then python3 "$port/package.py" "$1"; fi
