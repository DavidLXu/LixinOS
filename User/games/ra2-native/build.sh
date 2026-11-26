#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../../.." && pwd)
export RA_SDK="$root/Kernel/target/user-build"
cross=("$RA_SDK"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
base="$RA_SDK/ra2-engine-port"
build="$base/build"
package_name=
mkdir -p "$base"
fetch() {
    if [[ ! -d "$1/.git" ]]; then git init -q "$1"; fi
    if ! git -C "$1" cat-file -e "$3^{commit}" 2>/dev/null; then git -C "$1" fetch --depth=1 "$2" "$3"; fi
    git -C "$1" checkout -q --detach "$3"
}
fetch "$base/upstream" https://github.com/omarelarab/ra2-engine a8cfdccfef8592c13a6517c3fb76d834b1534027
fetch "$base/sdl-src" https://github.com/libsdl-org/SDL c98c4fbff6d8f3016a3ce6685bf8f43433c3efcc
[[ -x "$RA_SDK/gnome-port/cxx" ]] || bash "$root/User/gnome/build-deps.sh"
recomp="$RA_SDK/ra2-recomp"
mkdir -p "$recomp"
extra=(-DRECOMP_DIR= -DPREFLIGHT=OFF -DTEST_IMAGE=)
case "${1:-ra2-engine}" in
ra2-engine) target=ra2-engine ;;
ra2-recomp|yr-recomp|ra2-recomp-check)
    target=$1
    if [[ $target == yr-recomp ]]; then
        export RA2_TARGET=gamemd
        target=ra2-recomp
    fi
    fetch "$recomp/upstream" https://github.com/sp00nznet/redalert2-recomp c973ad7f6e04079f89a46ad38a905bcda7070e32
    fetch "$recomp/pcrecomp" https://github.com/sp00nznet/pcrecomp 7c07a0b42059b6dc4313d9b4ebded6f7a72db8f3
    fetch "$recomp/ttf" https://github.com/libsdl-org/SDL_ttf 89d1692fd8fe91a679bb943d377bfbd709b52c23
    prepared="$base/pc-port"
    if ! cmp -s "$port/recomp/memory.patch" "$prepared/applied.patch" ||
       ! cmp -s "$port/recomp/pe-read.patch" "$prepared/pe-read.patch"; then
        mkdir -p "$prepared"
        git -C "$recomp/pcrecomp" archive HEAD runtime | tar -x -C "$prepared"
        (cd "$prepared" && patch -p1 < "$port/recomp/memory.patch")
        (cd "$prepared" && patch -p1 < "$port/recomp/pe-read.patch")
        cp "$port/recomp/memory.patch" "$prepared/applied.patch"
        cp "$port/recomp/pe-read.patch" "$prepared/pe-read.patch"
    fi
    [[ -f "$RA_SDK/lite-xl-port/freetype-build/libfreetype.a" ]] || bash "$root/User/lite-xl/build.sh"
    source "$port/recomp/python-env.sh"
    if [[ $target == ra2-recomp-check ]]; then
        "$lift_python" "$port/recomp/fixture.py" "$recomp/pcrecomp" "$recomp/upstream" "$base/fixture"
        extra+=("-DTEST_IMAGE=$base/fixture")
    fi
    game=${RA2_TARGET:-game}
    case $game in game) gen=gen_game;; gamemd) gen=gen;; *) echo "Use RA2_TARGET=game or gamemd" >&2; exit 2;; esac
    if [[ $game == gamemd && $target == ra2-recomp ]]; then
        package_name=yr-recomp
        build="$base/build-yr"
    fi
    if [[ $target == ra2-recomp && -n ${RA2_DIR:-} && -z ${RA2_LIFT_DIR:-} ]]; then
        exec "$port/retail-lift.sh" "$RA2_DIR"
    fi
    lift_dir=${RA2_LIFT_DIR:-"$recomp/upstream/src/recomp/$gen"}
    if [[ -z ${RA2_LIFT_DIR:-} && -f "$recomp/current-$game" ]]; then
        IFS= read -r lift_dir < "$recomp/current-$game"
    fi
    if [[ $target == ra2-recomp && ! -f "$lift_dir/recomp_dispatch.c" ]]; then
        if [[ $game == gamemd ]]; then
            echo "Set RA2_DIR to the retail folder containing gamemd.exe." >&2
            exit 2
        fi
        if [[ -n ${RA2_LIFT_DIR:-} || -f "$recomp/current-$game" ]]; then
            echo "Incomplete translation at $lift_dir. Rebuild with RA2_DIR." >&2
            exit 2
        fi
        lift_dir="$base/preflight-$game"
        "$lift_python" "$port/recomp/preflight.py" "$recomp/pcrecomp" "$recomp/upstream" "$lift_dir" "$game"
        extra+=("-DPREFLIGHT=ON")
        echo "Building the retail PE loader. Set RA2_DIR to build the playable translation."
    fi
    extra+=("-DRECOMP_DIR=$recomp/upstream" "-DPC_PORT=$prepared" "-DTTF_DIR=$recomp/ttf" "-DLIFT_DIR=$lift_dir")
    ;;
*) echo "Choose ra2-engine, ra2-recomp-check, ra2-recomp or yr-recomp" >&2; exit 2 ;;
esac
package_name=${package_name:-$target}
cmake -S "$port" -B "$build" -DCMAKE_TOOLCHAIN_FILE="$port/toolchain.cmake" -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DENGINE_DIR="$base/upstream" -DSDL_DIR="$base/sdl-src" "${extra[@]}"
cmake --build "$build" --target "$target" -j "${JOBS:-10}"
python3 "$port/package.py" "$build/$target" "$package_name" "${RA2_PACKAGE_DIR:-$base/$package_name-package}"
