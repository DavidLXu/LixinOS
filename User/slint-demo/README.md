# Slint settings demo

`slint-demo` runs Slint’s software renderer at EL0 in LixinOS. Open it from
a shell in a QEMU graphics window. Click **Vivid accent** to animate the
switch, drag the QEMU window edge to resize the panel, and use **×** or
**Esc** to return to the shell. The switch state belongs to the demo.
Run `slint-demo --stats` to print frame counts when the app closes.

The package also installs `slint-editor`. Run `slint-editor hello.py` to
open or create a UTF-8 text file up to 1 MiB. Click to position the caret, edit
with the keyboard, and use **Ctrl+S** or **Save** to write the file. Press
**Esc** or use **×** to close. The editor asks you to save or discard changes
if the file has been modified. Run the saved script from the shell.

Build from the repository root on the Mac. The build uses the Rust
`aarch64-unknown-linux-musl` target and links against LixinOS’s C runtime.

```sh
bash User/slint-demo/build.sh
python3 User/gcc/install.py --toolchain slint-demo \
    Kernel/target/user-build/slint-package data/shared.img
```

Use an initialized, powered-off disk with the installer. It verifies the new
snapshot before making it active. Slint and the demo are built under GPL-3.0-only.
The Slint package includes the license text.

```sh
python3 scripts/test-slint-demo.py --kernel both
```

The test runs on disposable images. It captures the switch between its end
positions, resizes the window twice, clicks the moved control, checks partial
frame counts, and verifies that commands run after returning to the shell.

## Resizable canvas ABI

On AArch64, call service 19 with argument 1 to open or query a resizable canvas.
The result packs the logical width into bits 0–15, the logical height into bits
16–31, and a nonzero geometry generation into bits 32–63. The kernel sizes the
user pixel arena for the current dimensions before returning. A zero generation
identifies the fixed-size interface on older kernels.

To submit a region, put six 64-bit words at the graphics mailbox
`0x443d8000`: XRGB pixel address, destination x, destination y, width,
height, and source row stride in pixels. Pass the geometry generation as the
argument to service 20. The pixel address identifies the first pixel of the
rectangle. A result of −2 means the geometry changed. Query service 19 again,
update the layout, and redraw before continuing with partial updates.

For the fixed-size interface, use argument 0 for services 19 and 20 and submit
tightly packed rectangles with the first five mailbox words.
