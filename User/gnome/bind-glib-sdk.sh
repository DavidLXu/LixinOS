#!/usr/bin/env bash
set -euo pipefail
pcdir=$1/lib/pkgconfig
# Meson resolves -l flags by walking all of a dependency's -L paths in order.
# Keep GTK3's older GLib from winning that lookup through its Cairo dependency.
for name in glib-2.0 gobject-2.0 gthread-2.0 gio-2.0 gmodule-2.0 gmodule-no-export-2.0; do
    library=${name/gmodule-no-export/gmodule}
    file="$pcdir/$name.pc"
    sed "s|-l$library|\${libdir}/lib$library.a|g" "$file" > "$file.tmp"
    mv "$file.tmp" "$file"
done
