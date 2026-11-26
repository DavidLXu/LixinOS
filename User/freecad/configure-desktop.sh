#!/usr/bin/env bash
# Configure upstream FreeCAD with Sketcher and Part using only target libraries.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
b="$cache/freecad-port"
py="$cache/cpython-port"
g="$cache/gnome-port"
src="$b/FreeCAD-4fd3bf320d9566a27e60069fc8387448aaa3a094"
qt="${QT_TARGET_PATH:-$b/qt-gl-build}"
host="${QT_HOST_PATH:-$b/qt-host/6.8.3/macos}"
cross=("$cache"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$g/mesa-tools/bin:$b/aqt/bin:$PATH"
export PKG_CONFIG_LIBDIR="$cache/wayland-port/sdk/lib/pkgconfig:$b/sdk/lib/pkgconfig:$g/sysroot/lib/pkgconfig:$cache/gtk3-port/sysroot/lib/pkgconfig:$py/sysroot/lib/pkgconfig"
source "$port/fetch.sh"
for fix in base-stdint.patch app-backtrace.patch guest-python.patch static-workbenches.patch main-guest.patch guest-event-loop.patch guest-resources.patch pinyin-input.patch pinyin-application.patch; do
    if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/$fix" >/dev/null; then
        patch -f -s -p1 -d "$src" < "$port/$fix"
    fi
done
submodule() {
    local path=$1 repo=$2 commit=$3 digest=$4
    [[ -f "$src/$path/CMakeLists.txt" ]] && return
    fetch_archive "$b/$commit.tar.gz" "$digest" "https://codeload.github.com/$repo/tar.gz/$commit"
    mkdir -p "$src/$path"
    tar xf "$b/$commit.tar.gz" -C "$src/$path" --strip-components=1
}
submodule src/Mod/AddonManager FreeCAD/AddonManager 937b6877239dc78ef59eeefe8099e5f14243eda1 70b2fa7f3c58c0ea5be830de90d33369670ee6658f13aeb7684f1ea478528178
submodule src/3rdParty/GSL microsoft/GSL 543d0dd3fe966ddf20e884b44e5fdbf12cb43784 dd06a9190c02f6c04d008366faabc8947c817f33cbc6a838c5bfc9ad79652410
linguist="$host/lib/cmake/Qt6LinguistTools"
if [[ ! -d "$linguist" && -x "$b/qt-host-linguist/bin/lrelease" ]]; then
    linguist="$b/qt-host-linguist/lib/cmake/Qt6LinguistTools"
fi
libpaths="-L$b/occt-desktop/lin64/gcc/lib -L$b/deps/icu-static-76.1-r1/usr/lib -L$b/boost/usr/lib -L$b/sdk/lib -L$g/sysroot/lib"
wlqt="$b/qt-wayland-build/lib/cmake"
"$g/cc" -Os -ffunction-sections -fdata-sections -DLX_PTHREAD_COOPERATIVE_ONLY \
    -c "$root/User/cpython/threads.c" -o "$b/freecad-threads.o"
"$g/cc" -Os -ffunction-sections -fdata-sections -DLX_WAYLAND_INPUT -I"$b/sdk/include/python3.14" \
    -c "$root/User/cpython/interrupt.c" -o "$b/freecad-interrupt.o"
"$g/cc" -c "$port/once-tls.S" -o "$b/freecad-once.o"
"$g/cc" -Os -DLIBSTDCXX_ONCE_TLS -D_POSIX_C_SOURCE=200809L -c "$port/emutls.c" -o "$b/freecad-emutls.o"
"$g/cxx" -Os -I"$b/sdk/include/python3.14" -c "$port/workbench-init.cpp" -o "$b/workbench-init.o"
workbenches=(Materials Measure Part Sketcher MatGui MeasureGui PartGui SketcherGui)
wraps=''
{
    echo 'SECTIONS { .fc_workbench_init : ALIGN(8) {'
    for module in "${workbenches[@]}"; do
        echo "fc_${module}_begin = .; KEEP(*/${module}.a:*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*/${module}.a:*(.init_array)) fc_${module}_end = .;"
        wraps="$wraps -Wl,--wrap=PyInit_$module"
    done
    echo '} } INSERT BEFORE .init_array;'
} > "$b/workbench-init.ld"
pyside="$b/pyside-build"
python3 "$port/static-pyside.py" "$b" "${cross[0]}"
bindings="$b/pivy-build/libpivy.a $b/pyside-sdk/lib/python3.14/site-packages/shiboken6/Shiboken.abi3.a"
for module in QtCore QtGui QtWidgets QtPrintSupport QtUiTools; do
    bindings="$bindings $b/static-bindings/$module.abi3.a"
done
bindings="$bindings $pyside/libpyside/libpyside6.abi3.a $pyside/plugins/uitools/libuiplugin.a $b/static-bindings/libshiboken6.abi3.a"
flags=("-DLIXINOS_QT_PINYIN_SOURCE=$root/User/gui/pinyin-qt-startup.cpp"
       "-DQt6WaylandScannerTools_DIR=$b/wayland-scanner-host/lib/cmake/Qt6WaylandScannerTools"
       "-DWaylandScanner_EXECUTABLE=$cache/wayland-port/sdk/bin/wayland-scanner"
       "-DQt6WaylandClient_DIR=$wlqt/Qt6WaylandClient"
       "-DQt6QWaylandIntegrationPlugin_DIR=$wlqt/Qt6Gui"
       "-DQt6QWaylandXdgShellIntegrationPlugin_DIR=$wlqt/Qt6WaylandClient"
       "-DQt6QWaylandEglClientBufferPlugin_DIR=$wlqt/Qt6WaylandClient"
       "-DCMAKE_SHARED_LINKER_FLAGS=$libpaths" "-DCMAKE_EXE_LINKER_FLAGS=$libpaths $b/freecad-threads.o $b/freecad-interrupt.o $b/freecad-emutls.o $b/freecad-once.o $b/workbench-init.o $wraps -Wl,-T,$b/workbench-init.ld -Wl,--wrap=__emutls_get_address,--start-group"
       "-DCMAKE_CXX_STANDARD_LIBRARIES=$bindings -L$cache/wayland-port/sdk/lib -lwayland-server -lwayland-client -lffi -ldrm -lz -Wl,--end-group")
for mod in ASSEMBLY BIM CAM DRAFT FEM FLAT_MESH IMPORT INSPECTION JTREADER MESH_PART OPENSCAD \
           PART_DESIGN PLOT REVERSEENGINEERING ROBOT SPREADSHEET START SURFACE TECHDRAW TEST TUX WEB; do
    flags+=("-DBUILD_$mod=OFF")
done
if [[ -f "$b/application/_deps/fmt-src/CMakeLists.txt" ]]; then
    flags+=("-DFETCHCONTENT_SOURCE_DIR_FMT=$b/application/_deps/fmt-src")
fi
pybind="${PYBIND11_DIR:-}"
if [[ -z "$pybind" && -x "$b/aqt/bin/python" ]]; then
    pybind=$("$b/aqt/bin/python" -m pybind11 --cmakedir)
fi
[[ -z "$pybind" ]] || flags+=("-Dpybind11_DIR=$pybind")
cmake -S "$src" -B "$b/original-desktop" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$port/lixinos.cmake" \
    -DCMAKE_C_FLAGS="-D_GNU_SOURCE -I$b/sdk/include -I$g/sysroot/include" \
    -DCMAKE_CXX_FLAGS="-D_GNU_SOURCE -I$b/sdk/include -I$g/sysroot/include" \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_GUI=ON -DENABLE_DEVELOPER_TESTS=OFF -DBUILD_MEASURE=ON -DBUILD_MATERIAL=ON -DBUILD_PART=ON -DBUILD_SKETCHER=ON \
    -DBoost_USE_STATIC_LIBS=ON -DEIGEN3_INCLUDE_DIR="$b/eigen/usr/include/eigen3" \
    -DLIXINOS_EXTRA_ROOTS="$b/qt-wayland-build;$cache/wayland-port/sdk;$qt;$b/qt-svg-build;$b/qt-tools-build;$b/eigen/usr;$b/boost/usr;$b/deps/boost1.84-static-1.84.0-r3/usr;$b/deps/icu-dev-76.1-r1/usr;$b/deps/icu-static-76.1-r1/usr" -DCMAKE_PREFIX_PATH="$qt;$b/qt-svg-build;$b/qt-tools-build;$b/sdk" \
    -DCMAKE_MODULE_PATH="$b/qtbase-everywhere-src-6.8.3/cmake" \
    -DICU_I18N_LIBRARY_RELEASE="$b/deps/icu-static-76.1-r1/usr/lib/libicui18n.a" \
    -DICU_UC_LIBRARY_RELEASE="$b/deps/icu-static-76.1-r1/usr/lib/libicuuc.a" \
    -DQT_ADDITIONAL_PACKAGES_PREFIX_PATH="$b/qt-wayland-build;$b/qt-svg-build;$b/qt-tools-build" \
    -DEGL_LIBRARY="$b/wayland-egl-sdk/lib/libEGL.a" \
    -DEGL_STATIC_DEPENDENCIES="$g/sysroot/lib/libdrm.a;$py/sysroot/lib/libz.a" \
    -DQt6SvgWidgets_DIR="$b/qt-svg-build/lib/cmake/Qt6SvgWidgets" -DQt6Svg_DIR="$b/qt-svg-build/lib/cmake/Qt6Svg" \
    -DQt6UiTools_DIR="$b/qt-tools-build/lib/cmake/Qt6UiTools" \
    -DQt6LinguistTools_DIR="$linguist" -DQt6_DIR="$qt/lib/cmake/Qt6" -DQt6GuiTools_DIR="$host/lib/cmake/Qt6GuiTools" -DQT_HOST_PATH="$host" \
    -DPython3_EXECUTABLE="$py/host/python.exe" -DPython3_LIBRARY="$b/sdk/lib/libpython3.14.a" \
    -DPython3_INCLUDE_DIR="$b/sdk/include/python3.14" \
    -DCMAKE_DISABLE_FIND_PACKAGE_OpenCASCADE=ON -DOCC_INCLUDE_DIR="$b/occt-desktop/include/opencascade" \
    -DOCC_LIBRARY="$b/occt-desktop/lin64/gcc/lib/libTKernel.a" -DFREECAD_CHECK_PIVY=OFF \
    "${flags[@]}" "$@"
