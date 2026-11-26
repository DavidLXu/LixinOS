#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
c="$root/Kernel/target/user-build"
b="$c/code-port"
g="$c/gnome-port"
cross=("$c"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$PATH"

f="$c/freecad-port"
q="$f/qt-gl-build"
out="$f/qt-wayland-build"
w="$c/wayland-port/sdk"
mkdir -p "$b"
source "$root/User/freecad/fetch.sh"
fetch_archive "$b/monaco.tgz" c280cdcf0b0c13d1a2bf01af958d4387ed06d7f6c918401d00c4adcae1bc72b6 https://registry.npmjs.org/monaco-editor/-/monaco-editor-0.52.2.tgz
fetch_archive "$b/quickjs.tar.xz" 996c6b5018fc955ad4d06426d0e9cb713685a00c825aa5c0418bd53f7df8b0b4 https://bellard.org/quickjs/quickjs-2025-09-13-2.tar.xz
fetch_archive "$b/JetBrainsMono-Regular.ttf" a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f https://raw.githubusercontent.com/JetBrains/JetBrainsMono/v2.304/fonts/ttf/JetBrainsMono-Regular.ttf
fetch_archive "$b/font-LICENSE" 30f0c136e3c88e422d0791acd97238870f9054a9729bc34cf2ff0d4ed8cac4ad https://raw.githubusercontent.com/JetBrains/JetBrainsMono/v2.304/OFL.txt
fetch_archive "$b/pinyin.tgz" 46f37114a7929ecc01003a236803c8b1e5198382e6a21f83fae036604a6b08bf https://codeload.github.com/rime/rime-pinyin-simp/tar.gz/0c6861ef7420ee780270ca6d993d18d4101049d0
python3 "$port/pinyin-dictionary.py" "$b/pinyin.tgz" "$b"
if [[ ! -f "$g/glib-cross/build.ninja" ]]; then
    bash "$root/User/gnome/build-deps.sh"
fi
# Refresh an existing desktop SDK built before guest clipboard pipes were supported.
glib="$g/glib-2.86.0"
fix="$root/User/gnome/glib-pollable.patch"
if ! patch -f -Rs --dry-run -p1 -d "$glib" < "$fix" >/dev/null; then
    patch -f -s -p1 -d "$glib" < "$fix"
fi
"$c/gtk3-port/tools/bin/ninja" -C "$g/glib-cross" gio/libgio-2.0.a
if ! cmp -s "$g/glib-cross/gio/libgio-2.0.a" "$g/sysroot/lib/libgio-2.0.a"; then
    install -m 644 "$g/glib-cross/gio/libgio-2.0.a" "$g/sysroot/lib/libgio-2.0.a"
fi
if [[ ! -f "$g/mutter-cross/src/mutter" || "$root/User/gnome/mutter-display.c" -nt "$g/mutter-cross/src/mutter"
      || "$root/User/gnome/mutter-keyboard-order.patch" -nt "$g/mutter-cross/src/mutter"
      || "$root/User/gnome/mutter-single-client.patch" -nt "$g/mutter-cross/src/mutter" ]]; then
    bash "$root/User/gnome/build-mutter.sh"
fi
tar xzf "$b/monaco.tgz" -C "$b" package/esm package/LICENSE package/ThirdPartyNotices.txt
tar xf "$b/quickjs.tar.xz" -C "$b"
js="$b/quickjs-2025-09-13"
npx --yes esbuild@0.25.12 "$port/buffer.js" --alias:monaco-editor="$b/package" --bundle --format=iife --outfile="$b/buffer.js"
# The compositor and editor together can exceed the shared scheduler slot limit.
# Select the existing userspace pthread implementation for these two processes.
"$g/cc" -Os -ffunction-sections -fdata-sections -DLX_PTHREAD_COOPERATIVE_ONLY \
    -c "$root/User/cpython/threads.c" -o "$b/threads.o"
"$g/cc" -Os -I"$c/cpython-port/musl-1.2.5/src/process" -c "$root/User/rust/spawn.c" -o "$b/spawn.o"
objs=("$b/threads.o" "$b/spawn.o")
for name in quickjs dtoa libregexp libunicode cutils; do
    obj="$b/$name.o"
    if [[ ! -f "$obj" || "$b/quickjs.tar.xz" -nt "$obj" ]]; then
        "$g/cc" -Os -D_GNU_SOURCE '-DCONFIG_VERSION="2025-09-13"' -fwrapv -c "$js/$name.c" -o "$obj"
    fi
    objs+=("$obj")
done
[[ -f "$out/plugins/platforms/libqwayland-generic.a" ]] || bash "$root/User/freecad/build-qt-wayland.sh"
"$g/cxx" -Os -std=c++20 -D_GNU_SOURCE -iquote "$js" -I"$f/sdk/include" -I"$q/include" -I"$q/include/QtCore" -I"$q/include/QtGui" -I"$q/include/QtWidgets" \
    "$port/editor.cpp" "${objs[@]}" "$f/qt-emutls.o" -Wl,--wrap=__emutls_get_address \
    -L"$q/lib" -L"$out/lib" -L"$f/sdk/lib" -L"$w/lib" -L"$g/sysroot/lib" -L"$c/gtk3-port/sysroot/lib" -L"$c/dbus-port/sdk/lib" \
    -Wl,--start-group "$out/plugins/platforms/libqwayland-generic.a" "$out/plugins/wayland-shell-integration/libxdg-shell.a" \
    -lQt6WaylandClient -lQt6Widgets -lQt6Gui -lQt6DBus -lQt6Core -lQt6BundledPcre2 -lQt6BundledFreetype \
    -lQt6BundledLibpng -lharfbuzz -lxkbcommon -lpcre2-8 -lwayland-client -lwayland-cursor -lffi -lGL -lEGL -ldrm -ldbus-1 -lz -Wl,--end-group -o "$b/editor"
"$g/cc" -Os -ffunction-sections -fdata-sections -DAPP_SINGLE_CLIENT '-DAPP_NAME="code"' '-DAPP_COMPOSITOR="/lib/code/mutter"' '-DAPP_DISPLAY="wayland-code"' '-DAPP_PROGRAM="/lib/code/editor"' \
    -I"$c/cpython-port/musl-1.2.5/src/process" "$root/User/freecad/startfreecad.c" "$root/User/rust/spawn.c" -o "$b/startcode"
"$g/cc" -Os "$port/clip.c" -o "$b/clip"
python3 "$port/package.py"
