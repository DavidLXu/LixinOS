# LixinOS user programs

From the guest shell, edit the source with nano and compile the command you
want to replace. Each command below writes a persistent LXE1 executable.

    tcc /src/ls/ls.c -o /bin/ls
    tcc /src/cecho/cecho.c -o /bin/cecho
    tcc /src/agent/run.c -o /bin/agent-run

Run ls or cecho by name. The shell's agent run command invokes /bin/agent-run
with access to the project agent service. A successful rebuild takes effect
on the next invocation.

The python, git and tcc directories contain LixinOS port sources and build
recipes. Their full builds use upstream MicroPython, libgit2 and TinyCC,
respectively, and currently run on the host. Files named start.c provide
startup code for the host-built images. Guest TinyCC supplies its own startup
code when compiling a command's main function.

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
