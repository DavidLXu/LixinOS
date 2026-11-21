hb_revision=33a3f8de60dcad7535f14f07d6710144548853ac
hb="$build/harfbuzz-$hb_revision"
if [[ ! -f "$build/harfbuzz.tar.gz" ]]; then
    curl -fL --retry 3 "https://codeload.github.com/harfbuzz/harfbuzz/tar.gz/$hb_revision" -o "$build/hb.tmp"
    mv "$build/hb.tmp" "$build/harfbuzz.tar.gz"
fi
echo "326e6c3e0fdab4e9a3df8c6daeb6251a6a8997711a58bf4f8da421195d279bf9  $build/harfbuzz.tar.gz" | shasum -a 256 -c -
if [[ ! -d "$hb" ]]; then tar -xzf "$build/harfbuzz.tar.gz" -C "$build"; fi
if [[ ! -f "$hb/guest.o" ]]; then
    cxxheaders=("${cross[0]}"/aarch64-none-elf/include/c++/*)
    "$musl/cc" -isystem "${cxxheaders[0]}" -isystem "${cxxheaders[0]}/aarch64-none-elf" \
        -std=gnu++17 -fno-rtti -fno-exceptions -fno-threadsafe-statics -ffunction-sections -fdata-sections \
        -Os -DHB_TINY -c "$hb/src/harfbuzz.cc" -o "$hb/guest.tmp"
    mv "$hb/guest.tmp" "$hb/guest.o"
fi
noto=https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac
for spec in \
  NotoSans:b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5 \
  NotoSansArabic:ceea25b464a656dc3b26849bab9356740401af62aedf1bfa8b7f0d9b75925b1b \
  NotoSansHebrew:a7fa16fffb27bedb060a0866267c29e9859aeb9c21cc33f5b3aaf6eb062eca85 \
  NotoSansDevanagari:385e78e6359a9d88a0f243d53b1209d7548361ba2194e2b9ec779bcaa7e8949d \
  NotoSansTamil:6532db33b8b264abe3a098a40619feb489b5ddf5ab1d2b46e72b51eeb548001b; do
    name=${spec%%:*}
    if [[ ! -f "$build/$name.ttf" ]]; then
        curl -fL --retry 3 "$noto/hinted/ttf/$name/$name-Regular.ttf" -o "$build/$name.tmp"
        mv "$build/$name.tmp" "$build/$name.ttf"
    fi
    echo "${spec#*:}  $build/$name.ttf" | shasum -a 256 -c -
done
if [[ ! -f "$build/noto-OFL.txt" ]]; then
    curl -fL --retry 3 "$noto/LICENSE" -o "$build/noto-license.tmp"
    mv "$build/noto-license.tmp" "$build/noto-OFL.txt"
fi
echo "0dab92d0544f7b233403f14b84a663bdbfa746982eda629e7f4f9ffe1b036feb  $build/noto-OFL.txt" | shasum -a 256 -c -
