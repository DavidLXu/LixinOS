#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
export RA_SDK="$root/Kernel/target/user-build"
work="$RA_SDK/pdfview"
gtk="$RA_SDK/gtk3-port"
ft="$RA_SDK/lite-xl-port"
src="$work/xpdf-4.05"
build="$work/build"
pkg="$work/package"
mkdir -p "$work" "$pkg/lib/pdfview"
if [[ ! -f "$work/xpdf-4.05.tar.gz" ]]; then
    curl -fLsS --retry 2 https://distfiles.macports.org/xpdf/xpdf-4.05.tar.gz -o "$work/xpdf-4.05.tar.gz.part"
    mv "$work/xpdf-4.05.tar.gz.part" "$work/xpdf-4.05.tar.gz"
fi
echo '92707ed5acb6584fbd73f34091fda91365654ded1f31ba72f0970022cf2a5cea  xpdf-4.05.tar.gz' | (cd "$work" && shasum -a 256 -c -)
[[ -f "$src/xpdf/PDFDoc.cc" ]] || tar xzf "$work/xpdf-4.05.tar.gz" -C "$work"
[[ -f "$gtk/sysroot/lib/pkgconfig/gtk+-3.0.pc" ]] || bash "$root/User/gtk3/build.sh"
[[ -x "$RA_SDK/gnome-port/cxx" ]] || bash "$root/User/gnome/build-deps.sh"
cross=("$RA_SDK"/arm-gnu-toolchain-*-aarch64-none-elf/bin)
export PATH="$gtk/tools/bin:${cross[0]}:$PATH" PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$gtk/sysroot/lib/pkgconfig"
cmake -G Ninja -S "$src" -B "$build" -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$RA_SDK/gnome-port/cc" -DCMAKE_CXX_COMPILER="$RA_SDK/gnome-port/cxx" \
    -DCMAKE_AR="$(command -v aarch64-none-elf-ar)" -DCMAKE_RANLIB="$(command -v aarch64-none-elf-ranlib)" \
    -DCMAKE_FIND_ROOT_PATH="$RA_SDK/cpython-port/sysroot" -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DCMAKE_CXX_FLAGS="-DLOAD_FONTS_FROM_MEM=1 -ffunction-sections -fdata-sections" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_BUILD_TYPE=MinSizeRel -DMULTITHREADED=OFF -DNO_FONTCONFIG=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_Qt6Widgets=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_Qt5Widgets=TRUE \
    -DFREETYPE_INCLUDE_DIR_ft2build="$ft/freetype-VER-2-14-1/include" -DFREETYPE_INCLUDE_DIR_freetype_freetype="$ft/freetype-VER-2-14-1/include" \
    -DFREETYPE_LIBRARY="$ft/freetype-build/libfreetype.a"
cmake --build "$build" --parallel "${JOBS:-8}" --target xpdf_objs goo fofi splash \
    xpdf/CMakeFiles/pdftoppm.dir/SplashOutputDev.cc.o xpdf/CMakeFiles/pdftoppm.dir/ShadingImage.cc.o
read -r -a headers <<< "$(pkg-config --cflags --static gtk+-3.0 | sed 's/-pthread//g')"
read -r -a libs <<< "$(pkg-config --libs --static gtk+-3.0 | sed 's/-pthread//g')"
cc="$RA_SDK/cpython-port/cc"
cxx="$RA_SDK/gnome-port/cxx"
flags=(-Os -ffunction-sections -fdata-sections -DLOAD_FONTS_FROM_MEM=1)
"$cc" "${flags[@]}" "${headers[@]}" -c "$port/viewer.c" -o "$build/viewer.o"
"$cxx" "${flags[@]}" "${headers[@]}" -I"$build" -I"$src/xpdf" -I"$src/goo" -I"$src/fofi" -I"$src/splash" -c "$port/pdf.cpp" -o "$build/renderer.o"
libgcc=$(aarch64-none-elf-gcc -print-libgcc-file-name)
"$cc" -nostdlib -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 -Wl,-T,"$root/User/cpython/program.ld" \
    "$RA_SDK/cpython-port/sysroot/lib/crt1.o" "$(dirname "$libgcc")/crtbegin.o" "$build/viewer.o" "$build/renderer.o" \
    "$build/xpdf/CMakeFiles/xpdf_objs.dir/"*.o "$build/xpdf/CMakeFiles/pdftoppm.dir/"{SplashOutputDev,ShadingImage}.cc.o \
    -Wl,--start-group "$build"/{goo/libgoo,fofi/libfofi,splash/libsplash}.a "${libs[@]}" \
    "$RA_SDK/gnome-port/alpine-sdk/usr/lib/libstdc++.a" "$gtk/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group \
    "$(dirname "$libgcc")/crtend.o" -o "$build/pdfview.elf"
python3 - "$root" "$build" "$pkg" <<'PY'
from pathlib import Path
import runpy,sys,tempfile,shutil
root,build,pkg=map(Path,sys.argv[1:])
pack=runpy.run_path(str(root/'User/gcc/package.py'))
with tempfile.TemporaryDirectory(dir=build) as stage:
    stage = Path(stage)
    pack['tool'](stage,'/bin/pdfview.win3',build/'pdfview.elf','/lib/pdfview','-DNDEBUG',build,deflate=True)
    shutil.copytree(stage, pkg, dirs_exist_ok=True)
    for cached in (pkg/'lib/pdfview/images').iterdir():
        if not (stage/cached.relative_to(pkg)).exists():
            cached.unlink()
PY
cp "$src/COPYING3" "$pkg/lib/pdfview/COPYING3-Xpdf"
cp "$src/COPYING" "$pkg/lib/pdfview/COPYING-Xpdf"
cp "$port/xpdfrc" "$pkg/lib/pdfview/xpdfrc"
