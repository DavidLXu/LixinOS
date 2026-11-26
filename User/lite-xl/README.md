# Lite XL for LixinOS

Lite XL 2.1.8 runs as a resizable graphical window on the LixinOS desktop.
For a project, start the desktop from its directory.

```sh
cd /path/to/project
desktop
```

Open **Apps → lite-xl.win3**. Use the file tree, **Ctrl+P** to find a
project file, or **Ctrl+O** to open another file. Click or drag to select
text, and scroll inside the editor or file tree. **Ctrl+E** opens the
command palette, **Ctrl+F** searches the current document, and **Ctrl+S**
saves it. The title bar **×** asks Lite XL to confirm before closing an
unsaved buffer.

Chinese text and filenames use the bundled Noto CJK fallback. **Ctrl+C**
publishes a selection to the system clipboard. **Ctrl+V** pastes within
the editor, and the QEMU clipboard bridge's **Ctrl+Shift+V** pastes from the
Mac. Paired kernel releases from `kernel-2026-11-21.10` support desktop paste
that keeps UTF-8, line breaks, and tabs intact. Desktop clipboard transfers
are limited to 4096 UTF-8 bytes at a time.
File saves write a temporary sibling and rename it over the destination.

The port uses Lite XL's Lua editor, SDL3 software surfaces, FreeType and
PCRE2. The LixinOS desktop supplies the window, mouse and key events.
FreeType faces for different sizes and weights share the same open font file.
Native plugins and extensions that start subprocesses require further
runtime support.

The local Ctrl+C and Ctrl+V shortcuts also share up to 1 MiB of UTF-8 text with LixinOS Code and the guest `clip` command through `/tmp/.lixinos-clipboard`. Large selections use that channel in full. Short selections also use the existing Win3 desktop clipboard.

## Build and install

Run from the repository root on macOS with CMake and the usual LixinOS
cross-build tools. The build verifies the pinned source and font downloads.

```sh
bash User/lite-xl/build.sh
bash User/netsurf/build.sh

disk="$HOME/Library/Application Support/LixinOS/daily.img"
python3 User/gcc/install.py --toolchain netsurf \
  Kernel/target/user-build/netsurf-port/package "$disk"
python3 User/gcc/install.py --toolchain lite-xl \
  Kernel/target/user-build/lite-xl-port/package "$disk"
```

The NetSurf package supplies the desktop with the `.win3` protocol. Use
an initialized, powered-off disk. The package installer validates its
existing files and commits through the inactive snapshot.

## Verify

```sh
python3 scripts/test-lite-xl.py --kernel both --release kernel-2026-11-21.13 \
  --disk "$HOME/Library/Application Support/LixinOS/daily.img"
```

The test uses disposable images and opens the editor through the desktop
Apps menu. It exercises opening and editing a project file, mouse
scrolling, UTF-8 clipboard transfers, the unsaved-close prompt and window
resize. It reads the saved file from the disk image after shutdown.
Pass `--installed` to test the copies already installed on the source disk.
