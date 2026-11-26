#!/usr/bin/env bash
# Prepare the target XML, YAML, Python development files and C++ dependencies.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
b="$cache/freecad-port"
sdk="$b/sdk"
cross=("$cache"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$cache/gtk3-port/tools/bin:$PATH"
mkdir -p "$b/deps" "$sdk/include/python3.14"
source "$port/fetch.sh"
apk() {
    local name=$1
    fetch_archive "$b/$name.apk" "$2" "https://dl-cdn.alpinelinux.org/alpine/v3.23/main/aarch64/$name.apk"
    if [[ ! -d "$b/deps/$name/usr" ]]; then
        mkdir -p "$b/deps/$name"
        tar xf "$b/$name.apk" -C "$b/deps/$name" usr
    fi
}
apk boost1.84-static-1.84.0-r3 82ad15bb6496107cf1977dac72c221ce10695680849170a1f0d1d22f9e96c1bc
apk icu-dev-76.1-r1 786a08e25fd4940abaad31dbcb94730e47cb18e35347664c408e221738a83163
apk icu-static-76.1-r1 93fad1683e4851e969f1cc8d0f7393a379b8843cc0b31d4cb96ef755730da0d2
# Boost's CMake variant files resolve library paths relative to their own prefix.
mkdir -p "$b/boost/usr/lib"
tar xf "$b/boost-headers.apk" -C "$b/boost" usr/lib/cmake
cp "$b/deps/boost1.84-static-1.84.0-r3/usr/lib/"*.a "$b/boost/usr/lib/"
fetch_archive "$b/yaml.tar.gz" fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16 https://codeload.github.com/jbeder/yaml-cpp/tar.gz/refs/tags/0.8.0
fetch_archive "$b/xerces.tar.gz" 4042f6f11c9eba745dc0e5f8035d98b442097ee4efc69e4853410e4737a987f8 https://codeload.github.com/apache/xerces-c/tar.gz/refs/tags/v3.2.5
[[ -d "$b/yaml-cpp-0.8.0" ]] || tar xf "$b/yaml.tar.gz" -C "$b"
[[ -d "$b/xerces-c-3.2.5" ]] || tar xf "$b/xerces.tar.gz" -C "$b"
common=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$port/lixinos.cmake" -DCMAKE_BUILD_TYPE=MinSizeRel
        -DCMAKE_INSTALL_PREFIX="$sdk" -DBUILD_SHARED_LIBS=OFF)
cmake -S "$b/yaml-cpp-0.8.0" -B "$b/yaml-build" "${common[@]}" -DYAML_CPP_BUILD_TESTS=OFF \
    -DYAML_CPP_BUILD_TOOLS=OFF -DYAML_CPP_BUILD_CONTRIB=OFF
cmake --build "$b/yaml-build" --target yaml-cpp --parallel "${JOBS:-8}"
cmake --install "$b/yaml-build"
cmake -S "$b/xerces-c-3.2.5" -B "$b/xerces-build" "${common[@]}" -Dnetwork=OFF -Dtranscoder=iconv
cmake --build "$b/xerces-build" --target xerces-c --parallel "${JOBS:-8}"
cmake -DCMAKE_INSTALL_PREFIX="$sdk" -P "$b/xerces-build/src/cmake_install.cmake"
if ! "$cache/cpython-port/host/python.exe" -c 'import math, subprocess' >/dev/null 2>&1; then
    make -C "$cache/cpython-port/host" -j "${JOBS:-8}" sharedmods
fi
cp -R "$cache/cpython-port/Python-3.14.5/Include/." "$sdk/include/python3.14/"
install -m 644 "$cache/cpython-port/target/pyconfig.h" "$sdk/include/python3.14/"
bash "$port/build-python-embed.sh"
