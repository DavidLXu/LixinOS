# GTK 3 on LixinOS

Open `gtk3-demo.win3` from the desktop **Apps** menu to try GTK 3.24.49
with Chinese text, a text view and an **About GTK** dialog. The package
also builds `mousepad.win3` from Xfce Mousepad 0.6.3 and GtkSourceView
4.8.4. Both use the `.win3` protocol for keyboard, pointer, scrolling,
close requests and software-rendered updates.

In Mousepad, use **Ctrl+O** to open a file and **Ctrl+S** to save it.
The desktop title bar controls the outer window size. Opening, editing and
saving a file have been verified on both kernels. Mousepad can sometimes
leave its window open after a title-bar close request following a save.

The port reuses GTK 3's Broadway GDK window and event types. The local
transport composites Cairo surfaces into the desktop window and sends
RGB565 updates over its pipes. The toolkit and its dependencies are
statically linked into each application. Use the Rust and TCC kernels from
`kernel-2026-11-21.19` or a later compatible release. That pair supports
loading the larger executable on the ordinary working disk.

The first version hosts an application's windows inside one desktop window.
Large dialogs are scaled to fit the available area. GLib reads private
file mappings into memory for GSettings and other file-backed resources.
Run `ibus-run desktop` to start the IBus session for GTK applications.
The built-in GTK IBus module supports Compose with F12 as the compose key.
Dynamic GTK modules, OpenGL acceleration, printing,
and desktop-wide clipboard ownership require further integration.

## Build and install

Use the macOS build environment for LixinOS, including CMake, pkg-config,
gettext tools and host GLib tools. The build prepares Meson and Ninja in
a local virtual environment and reuses the LixinOS musl runtime plus the
FreeType and image libraries from the Lite XL and NetSurf builds.

```sh
bash User/gtk3/build.sh
python3 User/gcc/install.py --toolchain gtk3 \
  Kernel/target/user-build/gtk3-port/package \
  "$HOME/Library/Application Support/LixinOS/daily.img"
```

Power off the working disk before installing. The installer commits the
package through the inactive snapshot. The static libraries and pkg-config
files for building GTK programs are under
`Kernel/target/user-build/gtk3-port/sysroot`.

## Verify

```sh
disk="$HOME/Library/Application Support/LixinOS/daily.img"
python3 scripts/test-gtk3.py --kernel rust --release kernel-2026-11-21.19 --disk "$disk"
python3 scripts/test-gtk3.py --kernel tcc --release kernel-2026-11-21.19 --disk "$disk"
```

The tests install GTK on disposable disk copies, start the demo through the
Apps menu, and use QEMU input and framebuffer checks to verify typing,
maximizing, and opening and dismissing a modal dialog. Pass `--app mousepad`
to exercise Mousepad's file editing flow.

## Additional GTK 3 applications

The ports build the original upstream sources. Tetravex 3.38.3 uses the
existing GTK and Cairo stack. Mahjongg 3.38.3 renders its original tile
themes through the upstream bitmap path. Mines 40.1 also builds libgee
and libgnome-games-support for its score handling. The build rasterizes
the supplied SVG game art on the host with `rsvg-convert`.

Ristretto 0.14.0 uses static Xfce libraries and libexif. Its local image
viewer uses the LixinOS GTK backend and a small GIO image MIME database.
It runs with the upstream in-memory preferences and without the Xfce
thumbnail and wallpaper services.

```sh
bash User/gtk3/build.sh
bash User/gtk3/build-games.sh tetravex
bash User/gtk3/build-games.sh mahjongg
bash User/gtk3/build-games.sh mines
bash User/gtk3/build-ristretto.sh
```

The games require host Vala. Mahjongg and Mines also require
`rsvg-convert`. Packages are under
`Kernel/target/user-build/gtk3-port/extra/<app>-package` and install with
`User/gcc/install.py --toolchain <app>`. Mines uses the desktop's optional
right-button event. Rebuild the desktop with `User/netsurf/build.sh` for
that event. The desktop continues to open its generic edit menu for
clients that do not subscribe to the event.

```sh
LIXINOS_DISK="$HOME/Library/Application Support/LixinOS/gtk-apps.img" \
  ./scripts/run-kernel.sh --kernel rust --release kernel-2026-11-21.19 --window
```

Open the games and viewer from **Apps**. The regression runner also
accepts those four names via `--app`. Use `--desktop PATH` to test a newly
built desktop executable on the disposable disk.

Drawing and GIMP remain in progress. The Pycairo, PyGObject and
GObject Introspection C archives compile for LixinOS. Drawing still needs
guest GTK typelibs and symbol resolution for the statically linked GTK
libraries. The babl and GEGL core archives also compile. GIMP still
needs GEGL operation modules and a C++ runtime that can build Exiv2
with exceptions and locale support.

## Nautilus

Nautilus 3.24.2 runs as `nautilus.win3` through the GTK 3 backend. Open it
from **Apps** to browse local files. The port uses GIO's local filesystem
backend and in-memory preferences. Tracker indexing, GNOME desktop services,
remote volumes, app associations, and thumbnail generation are outside this
port's scope.

```sh
bash User/gtk3/build-nautilus.sh
disk="$HOME/Library/Application Support/LixinOS/daily.img"
python3 User/gcc/install.py --toolchain nautilus \
  Kernel/target/user-build/gtk3-port/nautilus-port/package "$disk"
```

The build uses `rsvg-convert` to package Adwaita icons as PNGs. It checks
source archive hashes and includes GTK fonts, compiled GSettings schemas,
and license files. With the guest powered off, run the desktop smoke test
on a disposable copy of the disk.

```sh
python3 scripts/test-gtk3.py --app nautilus --disk "$disk" \
  --release kernel-2026-11-21.23 --kernel rust
python3 scripts/test-gtk3.py --app nautilus --disk "$disk" \
  --release kernel-2026-11-21.23 --kernel tcc
```
