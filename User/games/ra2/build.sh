#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/ra2-port"
pkg="$build/package"
mkdir -p "$build/obj" "$pkg/bin" "$pkg/lib/ra2"
checkout() {
    if [[ ! -e "$1/.git" ]]; then git init -q "$1"; fi
    if ! git -C "$1" cat-file -e "$3^{commit}" 2>/dev/null; then git -C "$1" fetch -q --depth=1 "$2" "$3"; fi
    git -C "$1" checkout -q --detach "$3"
}
engine="$build/vanguard"
js="$build/quickjs"
checkout "$engine" https://github.com/harshilmathur/openra2 8ba59f0bcd48ba0c89892c0455eeca7da4408f4c
checkout "$js" https://github.com/bellard/quickjs 25197744030a9659b5a6e6f861ae78981b5fce07
(cd "$engine" && npm ci --ignore-scripts --no-audit --no-fund)
esbuild="$engine/node_modules/.bin/esbuild"
aliases=()
for name in sim formats game; do aliases+=("--alias:@vanguard/$name=$engine/packages/$name/src/index.ts"); done
"$esbuild" "$port/export.mjs" --bundle --format=esm --platform=node "--alias:asset-handler=$engine/apps/client/dev/assetPlugin.ts" "${aliases[@]}" --outfile="$engine/apps/client/dev/lixinos-export.mjs"
mkdir -p "$build/assets"
node "$engine/apps/client/dev/lixinos-export.mjs" "$build/assets" "$pkg/lib/ra2"
python3 "$port/pack.py" "$build/assets" "$pkg/lib/ra2/sprites.bin" "$engine/apps/client/public/hd/props" "$pkg/lib/ra2/match.json"
"$esbuild" "$port/skirmish.ts" --bundle --platform=neutral --format=iife --target=es2020 \
    "--alias:@vanguard/sim=$engine/packages/sim/src/index.ts" "--alias:@vanguard/game=$engine/packages/game/src/bot/bot.ts" \
    "--alias:@vanguard/formats=$port/formats.ts" "--alias:vanguard-ai=$engine/packages/formats/src/ai/aiDb.ts" \
    "--alias:vanguard-ini=$engine/packages/formats/src/ini/iniFile.ts" \
    "--alias:client-sim=$engine/apps/client/src/sim.ts" "--alias:client-net=$engine/apps/client/src/net.ts" --outfile="$pkg/lib/ra2/skirmish.js"
musl="$cache/cpython-port"
LIXINOS_GCC_BUILD="$build/runtime" bash "$root/User/gcc/build.sh" rt
if ! command -v aarch64-none-elf-gcc >/dev/null; then
    tools=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
    export PATH="${tools[0]}/bin:$PATH"
fi
cc="$musl/cc"
objects=()
for unit in quickjs dtoa libregexp libunicode cutils; do
    obj="$build/obj/$unit.o"; objects+=("$obj")
    if [[ ! -f "$obj" || "$js/$unit.c" -nt "$obj" ]]; then
        "$cc" -O2 -D_GNU_SOURCE "-DCONFIG_VERSION=\"$(cat "$js/VERSION")\"" -fwrapv -ffunction-sections -fdata-sections -c "$js/$unit.c" -o "$obj"
    fi
done
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$build/obj/crt1.o"
"$cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" -c "$root/User/cpython/loader.c" -o "$build/obj/crt0.o"
"$cc" -O2 -I"$js" -c "$port/platform.c" -o "$build/obj/platform.o"
"$cc" -static -nostartfiles -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 -Wl,-T,"$root/User/gcc/application.ld" \
    "$build/obj/crt0.o" "$build/obj/crt1.o" "$build/obj/platform.o" "${objects[@]}" -Wl,--start-group "$build/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group -o "$pkg/bin/ra2"
cp "$engine/LICENSE" "$pkg/lib/ra2/VANGUARD-LICENSE"
cp "$js/LICENSE" "$pkg/lib/ra2/QUICKJS-LICENSE"
cp "$engine/docs/ASSETS.md" "$pkg/lib/ra2/ASSETS.md"
echo "$pkg"
