# Invoked from build.sh with its GTK sysroot, pinned sources and runtime.
thunar=thunar-4.18.11
fetch "$thunar" thunar 7d0bdae2076a568c137d403ab5600e06a7a4f7a02514d486da7b8414aa75d612 4.18
work="$build/thunar-objects"
mkdir -p "$work"
if ! patch -f -d "$thunar" -p1 -R --dry-run < "$port/thunar.patch" >/dev/null 2>&1; then
    patch -f -d "$thunar" -p1 < "$port/thunar.patch"
fi
deps=(gtk+-3.0 gio-unix-2.0 exo-2 libxfce4kbd-private-3 libxfce4ui-2 libxfconf-0)
read -r -a thunar_flags <<< "$(pkg-config --static --cflags "${deps[@]}" | sed 's/-pthread//g')"
read -r -a thunar_libs <<< "$(pkg-config --static --libs "${deps[@]}" | sed 's/-pthread//g')"
# Use the release tarball's generated resource, enum, marshal and GDBus sources.
make -f "$port/thunar.mk" -j"${JOBS:-8}" SRC="$build/$thunar" OUT="$work" CC="$cc" \
    CPPFLAGS="${thunar_flags[*]} -I$port/thunar-config -I$build/$thunar -I$build/$thunar/thunar -DHAVE_CONFIG_H -DTHUNARX_COMPILATION" \
    CFLAGS="${flags[*]}"
"$cc" "${link[@]}" -Wl,-T,"$root/User/rust/large.ld" "${runtime[@]}" "$work/"*.o \
  -Wl,--start-group "${thunar_libs[@]}" "$gtk/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group \
  -o "$build/thunar.tmp"
mv "$build/thunar.tmp" "$pkg/bin/thunar.win3"
cp "$thunar/COPYING" "$pkg/lib/xfce/COPYING-thunar"
