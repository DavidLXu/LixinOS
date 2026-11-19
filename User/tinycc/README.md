# Rebuild TinyCC inside LixinOS

Run these commands from the guest shell. The first command uses the distribution
compiler. Each subsequent build runs the executable produced by the preceding
command.

    tcc /src/tinycc/bootstrap.c -o tcc-new
    ./tcc-new /src/tinycc/bootstrap.c -o tcc-second
    ./tcc-second /src/bash/bash.c -o bash-new
    ./bash-new --version
    ./bash-new

Type `exit` to return to the original shell. To install a tested version, use
`./tcc-second /src/bash/bash.c -o /bin/bash`. The next login loads it.

The upstream/ directory contains the pinned ARM64 TinyCC compiler sources.
bootstrap.c selects the LixinOS command-line driver in port.c. sdk/ contains
the C library headers used to compile the compiler, and builtin/tccdefs.h
provides its target predefinitions. These inputs ship on the guest volume.
The seed compiler and LXE1 runtime (including newlib) ship with the OS image.
The two compiler generations share that runtime. The host packages the initial
sources and seed. All three builds above run inside the guest.

The generated compiler accepts one or more C translation units and links them
into a saved ARM64 LXE1 executable, for example
`./tcc-second main.c lib.c -o app`. Quoted includes are resolved relative to
their source file. Existing LXE1 code, data and file size limits apply.
