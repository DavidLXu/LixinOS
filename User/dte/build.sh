#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build/dte-port"
version=1.11.1
sha256=7c4aa5d3ba91f921e1ab2bc8aac3ffb34b096ca8533ab97c7ed2919babd0a4e9
archive="$cache/dte-$version.tar.gz"
runtime="$cache/runtime"
make_cmd=${DTE_MAKE:-gmake}

mkdir -p "$cache"
work=$(mktemp -d "$cache/build.XXXXXX")
trap 'rm -rf "$work"' EXIT
if [[ ! -f $archive ]]; then
    curl -fsSL "https://codeload.github.com/craigbarnes/dte/tar.gz/refs/tags/v$version" \
        -o "$work/source.tar.gz"
    printf '%s  %s\n' "$sha256" "$work/source.tar.gz" | shasum -a 256 -c -
    mv "$work/source.tar.gz" "$archive"
else
    printf '%s  %s\n' "$sha256" "$archive" | shasum -a 256 -c -
fi

tar -xzf "$archive" -C "$work"
src="$work/dte-$version"
patch -d "$src" -p1 < "$port/lixinos.patch"
LIXINOS_GCC_BUILD="$runtime" bash "$root/User/gcc/build.sh" rt
if ! command -v aarch64-none-elf-gcc >/dev/null; then
    cross=("$root"/Kernel/target/user-build/arm-gnu-toolchain-*-aarch64-none-elf)
    export PATH="${cross[0]}/bin:$PATH"
fi
abi="$root/User/tcc"
musl="$root/Kernel/target/user-build/cpython-port"
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$work/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION \
    -I"$root/User/gcc" -c "$root/User/cpython/loader.c" -o "$work/crt0.o"
"$musl/cc" -Os -ffunction-sections -fdata-sections \
    -I"$musl/musl-1.2.5/src/process" -c "$root/User/rust/spawn.c" -o "$work/spawn.o"
"$musl/cc" -Os -ffunction-sections -fdata-sections \
    -iquote "$src/src" -c "$port/process.c" -o "$work/process.o"
cat > "$work/cc" <<SH
#!/usr/bin/env bash
for arg do
    case "\$arg" in
        -c|-E|-S|-M|-MM|-print*|--version|-dump*) exec "$musl/cc" "\$@" ;;
    esac
done
exec "$musl/cc" "\$@" -nostartfiles -static \
    -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/gcc/application.ld" "$work/crt0.o" "$work/crt1.o" \
    -Wl,--start-group "$runtime/rt/liblx.a" -lc -lgcc -Wl,--end-group
SH
chmod +x "$work/cc"
"$make_cmd" -C "$src" -j "${JOBS:-8}" CC="\"$work/cc\"" LDLIBS_ICONV= \
    CPPFLAGS="-idirafter \"$abi\"" CFLAGS="-Os -ffunction-sections -fdata-sections" \
    LDFLAGS="-s -Wl,--wrap=fork_exec" LDLIBS="\"$work/process.o\" \"$work/spawn.o\""
package="$cache/package"
mkdir -p "$package/bin" "$package/lib/dte"
cp "$src/LICENSE" "$package/lib/dte/LICENSE"
cp "$src/dte" "$package/bin/dte.tmp"
mv "$package/bin/dte.tmp" "$package/bin/dte"
echo "dte package: $package"
