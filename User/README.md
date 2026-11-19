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
