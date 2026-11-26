# System fonts

LixinOS stores scalable fonts in `/usr/share/fonts`. The bundled faces are Fira Sans for UI text, JetBrains Mono for code and Noto Sans Mono CJK SC for Chinese. New font files survive a reboot.

```sh
fonts list
fonts match monospace
fonts match 'sans-serif:charset=4e2d'
fonts install /work/MyFont.ttf
```

`fonts list` prints the family and path. `match` uses Fontconfig patterns. `install` validates a TTF, OTF or TTC with FreeType and installs it as `/usr/share/fonts/user-MyFont.ttf`. Installing the same filename again atomically replaces that user copy. Restart an open app to refresh its font list. Use a shell account with write access to the shared directory.

GTK, GNOME and Writer discover fonts through Fontconfig. Code and FreeCAD scan the shared directory through Qt. Lite XL uses the shared UI, code and CJK faces when present. Lite XL configuration can load another face by its file path. Applications without the font package retain their bundled fonts.

## QEMU graphical console and native GUI

```sh
fonts console /usr/share/fonts/JetBrainsMono-Regular.ttf
fonts console --reset
```

Choose one of these commands and restart QEMU to apply the change to the graphical console. The command rasterizes printable ASCII into `/lib/fonts/console.gray` before activating it. Both kernels validate its length and fall back to their normal console atlas. This retains the terminal grid and leaves wide-character rendering on its separate path. The host terminal controls its own font in serial mode.

The native desktop consumes the selected ASCII face when it starts. Programs compiled with the updated `lixin-gui.h` or `window-app.h` draw UTF-8 Han characters using the guest's shared Unifont bitmap. Recompile an older LXE to update its inline drawing routines. These cell APIs target UI labels and do not perform complex-script shaping.

## Build and install

After building the guest GTK3, Lite XL and GNOME runtime dependencies:

```sh
bash User/fonts/build.sh
python3 User/gcc/install.py --toolchain fonts Kernel/target/user-build/fonts-port/package data/shared.img
python3 scripts/test-fonts.py --disk data/shared.img --kernel rust
python3 scripts/test-fonts.py --disk data/shared.img --kernel tcc
```

On an existing disk, update the graphical app packages alongside the fonts to pick up their new lookup paths. Installing the font package alone supplies the font files, system Fontconfig configuration and command.
