# LixinOS user programs

From the guest shell, edit the source with nano and compile the command you
want to replace. Each command below writes a persistent LXE1 executable.

    tcc /src/ls/ls.c -o /bin/ls
    tcc /src/cecho/cecho.c -o /bin/cecho
    tcc /src/agent/run.c -o /bin/agent-run

Run ls or cecho by name. The shell's agent run command invokes /bin/agent-run
with access to the project agent service. A successful rebuild takes effect
on the next invocation.

The python and git directories contain the host build recipes for MicroPython
and libgit2. For the guest-native MicroPython, core utilities, desktop and
editor build, run `cat /src/build/README.md`. The tcc directory contains the distribution compiler port.
To build TinyCC twice inside LixinOS and use it to compile Bash, run
`cat /src/tinycc/README.md` for the commands and source layout.

The native GNU C toolchain builds static ARM64 ELF programs on either kernel.
See [GNU C on LixinOS](gcc/README.md) for shell examples and the build recipe.

MicroPython supports slices such as `items[:-1]`, sets, f-strings,
assignment expressions, and `select.poll()` on `sys.stdin`. Try
`python /src/games/snake.py` for a keyboard-driven example.

The `kilo` directory contains the LixinOS port of Salvatore Sanfilippo's Kilo
(upstream revision 323d93b29bd89a2cb446de90c4ed4fea1764176e).
Its BSD license is in kilo.c. The foreground runtime also hosts nano, htop
in user space. The core directory builds the individual
EL0 text utilities installed in /bin. Run `python3 scripts/test-foreground.py`
and `python3 scripts/test-panes.py` on the host to exercise these interfaces.

The interactive `htop` monitor samples CPU time per core and per process. Its
memory column reports process-private KiB. Use the arrow keys to select a PID,
`/` or F4 to filter command names, F5 for the process tree, and F6 to sort.
P, M and T sort by CPU usage, memory or accumulated CPU time. Press q to exit.

The optional [Node.js package](node/README.md) runs JavaScript with V8 and libuv.

The optional [dte package](dte/README.md) adds a terminal code editor with
multiple tabs, UTF-8 editing, and compiler diagnostic navigation.
The [LixinOS Code package](code/README.md) opens a graphical UTF-8 editor with the VS Code document model from the shell using `code filename`.
The optional [Lite XL package](lite-xl/README.md) adds a desktop code
editor with a project tree, mouse editing, and Chinese font fallback.
The optional [GTK 3 package](gtk3/README.md) adds a native window backend,
a C demo, and a host-side static library sysroot for porting GTK applications.
The [Wayland port](wayland/README.md) provides native client and server
libraries and a shared-memory display demo for both kernels.

The [ClassiCube port](classicube/README.md) runs classic creative worlds in the
graphical shell. Launch it with `classicube` after installing the package.
The [Doom engine port](doom/README.md) runs Freedoom Phase 2 in the graphical
shell. Launch it with `doom` after installing the package.

### LixinOS Bash

The default login shell is a C program at `/bin/bash`. The stock `/bin/sh`
entry follows that file. Both names start the same shell. Run `bash --version`
to identify it. Bash implements the LixinOS interactive command language.

To edit and try a build alongside your current shell:

    nano /src/bash/bash.c
    tcc /src/bash/bash.c -o bash-test
    ./bash-test

Return with `exit`. Install your changes with
`tcc /src/bash/bash.c -o /bin/bash`. The next `bash`, `sh`, or login loads the
updated executable. The current process continues running its original image.
The installer migrates known stock Rust shells and preserves guest-built
custom replacements of /bin/sh.

Saved LXE1 programs can include `<lixin.h>` and call
`lx_spawn(path, argc, argv)`. It returns the foreground child's exit status
(0–255), or -1 for a launch error. Each caller retains its memory, arguments
and open descriptors. The small foreground-shell example remains in
`/src/sh/csh.c`. Run `python3 scripts/test-sh.py` to exercise default Bash,
nested processes, and guest rebuilds across reboot on a disposable disk.

For concurrent work, a saved LXE1 program can start a child with
`lx_spawn_async(path, argc, argv)`, keep working, then collect its exit status
with `lx_wait(pid)`. Standard streams are inherited. `lx_pid()` identifies
the caller. Bash runs `producer | consumer` as concurrent stages and accepts
a trailing `&` for detached jobs.

The descriptor interface includes `lx_open`, `lx_open_write`, `lx_tcp_connect`,
`lx_read`, `lx_write`, `lx_poll` and `lx_close`. Read and write transfer up to
4 KiB per call and can return a short count. A zero-length result from a
nonempty read means EOF. Use descriptors 0, 1 and 2 for stdin, stdout and
stderr. `lx_poll(fd, LX_READABLE | LX_WRITABLE, timeout_ms)` returns the ready
bits, zero on timeout, or -1 for an invalid descriptor. A negative-one timeout
waits indefinitely. The older TCP-specific calls retain their nonblocking ABI.
