# Native system rebuild

In the guest shell, create a separate directory for the outputs and run:

    mkdir /native
    cd /native
    tcc /src/tinycc/bootstrap.c -o tcc-new
    tcc /src/build/rebuild.c -o rebuild
    ./rebuild

The seed creates the first compiler and build driver. The driver uses tcc-new
to build ./tcc, then uses ./tcc to build the C applications. It prints an exit
status per target. Try ./python smoke.py with a script of your own, ./desktop
in a QEMU window, or ./bash --version. The build directory must already exist.
Rebuilding there replaces its previous artifacts.

MicroPython compiles from its port, upstream C sources and configured headers
under /src/micropython. The distribution build stages the generated qstr, root
pointer and module definition tables. Changes to the MicroPython configuration
that affect those tables still need their generators.

Kilo uses a Rust function for mapping terminal keys. The native Lust frontend
exports this function as C and the native TinyCC links it into the editor.
Lust can also export a complete supported Rust program:

    lust emit-c /src/lust/hello.rs -o hello.c
    ./tcc hello.c -o hello
    ./hello

Kilo, Paint and htop require a foreground service grant. To use the rebuilt
versions, create /libexec and compile the corresponding targets there. From
/native, for example:

    mkdir /libexec
    ./tcc -DAPP_KILO -I/src/tinycc/sdk /src/foreground/runtime.c /src/kilo/kilo.c keycode.c -o /libexec/kilo
    ./tcc -DAPP_HTOP -I/src/tinycc/sdk /src/foreground/runtime.c /src/foreground/htop.c -o /libexec/htop
    ./tcc -I/src/tinycc/sdk /src/foreground/runtime.c /src/paint/main.c /src/paint/raster.c /src/paint/document.c -o /libexec/paint

Use kilo, paint and htop from the shell as usual. The launch service chooses
the disk binary when present and retains the existing file grant and conflict
checks. Desktop reads its font atlas from /src/gui/menlo.gray.

The host still builds the kernel, distribution compiler and C runtime, Lust
frontend, Rust SSH client, legacy Rust shell, nano and the Git and Links ports.
Lust accepts a Rust subset. Cargo dependency resolution, procedural macros
and compilation of the existing Rust kernel require additional tooling.
