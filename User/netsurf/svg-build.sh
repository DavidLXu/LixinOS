# Sourced by build.sh with the guest compiler, build cache and sysroot selected.
build_svg_library() {
    local name=$1 revision=$2 digest=$3 dir="$build/$1-$2"
    if [[ ! -f "$build/$name.tar.gz" ]]; then
        curl -fL --retry 3 "https://codeload.github.com/sammycage/$name/tar.gz/$revision" -o "$build/$name.tmp"
        mv "$build/$name.tmp" "$build/$name.tar.gz"
    fi
    echo "$digest  $build/$name.tar.gz" | shasum -a 256 -c -
    if [[ ! -d "$dir" ]]; then tar -xzf "$build/$name.tar.gz" -C "$build"; fi
    if [[ ! -f "$dir/guest.a" ]]; then
        local file obj
        local -a objects=()
        for file in "$dir"/source/*.c; do
            obj="$dir/$(basename "${file%.c}").o"
            "$musl/cc" -std=gnu11 -Os -ffunction-sections -fdata-sections -I"$dir/include" \
                -I"$prefix/include" -DPLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD -DPLUTOVG_BUILD_STATIC -DPLUTOSVG_BUILD_STATIC -c "$file" -o "$obj"
            objects+=("$obj")
        done
        aarch64-none-elf-ar rcs "$dir/guest.tmp" "${objects[@]}"
        mv "$dir/guest.tmp" "$dir/guest.a"
    fi
    cp "$dir/guest.a" "$prefix/lib/lib$name.a"
    if [[ $name == plutovg ]]; then cp "$dir/include/plutovg.h" "$prefix/include/"
    else cp "$dir/source/plutosvg.h" "$prefix/include/"; fi
}
vg_revision=bbd91f0d06a71491691b36330f29dffa4af87ccf
svg_revision=fd8a080b3d0bccc41f1ae708d3785e7f08398a58
build_svg_library plutovg "$vg_revision" 19edc7777470bd5737526a6a339ea518892861960cf3563572d5847dd007ccdc
build_svg_library plutosvg "$svg_revision" 2194ea7a2c2313410e0e69960c29d21cb082fea49e7db6dacd8ae708add72ccb
