# Nano-X desktop

Run `nano-x` in a QEMU graphics console. Select **Files** to browse the
directory you launched from. Click a folder to enter it or **Up** to go back.
Drag the window by its title bar and use its close button to dismiss it.
Select **Exit** or press **Esc** to return to the shell. Run `desktop` for the
original LixinOS desktop.

Nano-X and its window manager come from [Microwindows](https://github.com/ghaerr/microwindows).
The port links the server, window manager and file browser into one EL0
program and uses the LixinOS framebuffer and tablet. Each session has its own
windows. The first version runs the bundled client in the same process and
uses a canvas sized when the session opens.

Build on the Mac and install on an initialized disk while QEMU is off:

```sh
bash User/nano-x/build.sh
python3 User/gcc/install.py --toolchain nano-x \
    Kernel/target/user-build/nano-x-package data/shared.img
```

The build pins and verifies the Microwindows source archive. The installed
license is available at `/lib/nano-x/LICENSE`.
