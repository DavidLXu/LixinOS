#!/usr/bin/env bash
set -euo pipefail
# Run in the build directory. Verify the archive before extracting sources.
unpack() {
    local name=$1 archive=$2 sha=$3 url=$4
    if [[ -f $name/.source-sha256 && $(cat "$name/.source-sha256") == "$sha" ]]; then return; fi
    if ! echo "$sha  $archive" | shasum -a 256 -c - >/dev/null 2>&1; then
        curl --fail --location --retry 3 --connect-timeout 15 "$url" -o "$archive.tmp"
        echo "$sha  $archive.tmp" | shasum -a 256 -c -
        mv "$archive.tmp" "$archive"
    fi
    tar xf "$archive"
    echo "$sha" > "$name/.source-sha256"
}
mirror=https://mirrors.kernel.org/gnu
unpack gcc-4.8.5 gcc.tar.bz2 22fb1e7e0f68a63cee631d85b20461d1ea6bda162f03096350e38c8d427ecf23 https://gcc.gnu.org/pub/gcc/releases/gcc-4.8.5/gcc-4.8.5.tar.bz2
unpack binutils-2.43.1 binutils.tar.xz 13f74202a3c4c51118b797a39ea4200d3f6cfbe224da6d1d95bb938480132dfd "$mirror/binutils/binutils-2.43.1.tar.xz"
unpack gmp-6.2.1 gmp-6.2.1.tar.xz fd4829912cddd12f84181c3451cc752be224643e87fac497b69edddadc49b4f2 "$mirror/gmp/gmp-6.2.1.tar.xz"
unpack mpfr-4.1.0 mpfr-4.1.0.tar.xz 0c98a3f1732ff6ca4ea690552079da9c597872d30e96ec28414ee23c95558a7f "$mirror/mpfr/mpfr-4.1.0.tar.xz"
unpack mpc-1.2.1 mpc-1.2.1.tar.gz 17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459 "$mirror/mpc/mpc-1.2.1.tar.gz"
