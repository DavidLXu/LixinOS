#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
build=$(cd "$port/../.." && pwd)/Kernel/target/user-build/gnome-port
mkdir -p "$build"
while read -r file digest url; do
    [[ -n $file && $file != \#* ]] || continue
    destination="$build/$file"
    if [[ ! -s $destination ]]; then
        curl -fLsS --retry 3 "$url" -o "$destination.tmp"
        printf '%s  %s\n' "$digest" "$destination.tmp" | shasum -a 256 -c -
        mv "$destination.tmp" "$destination"
    fi
    printf '%s  %s\n' "$digest" "$destination" | shasum -a 256 -c -
done < "$port/shell-sdk.tsv"
