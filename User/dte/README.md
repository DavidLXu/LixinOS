# dte on LixinOS

dte 1.11.1 is an optional AArch64 terminal code editor for the Rust and
TCC kernels. It includes line numbers, syntax highlighting, multiple
tabs, regular-expression search and replace, and UTF-8 editing.

```sh
dte main.c helper.c
```

| Keys | Action |
| --- | --- |
| Ctrl+S / Ctrl+Q | Save / quit |
| Ctrl+O | Open a file |
| Ctrl+F / Ctrl+G | Search / find next |
| Ctrl+R | Replace |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| Ctrl+B / Ctrl+T | Previous / next tab |
| Ctrl+E | Enter a dte command |

Press **Ctrl+E** and run `tcc main.c -o main` to build with the guest's
TinyCC. With GCC installed, use `compile gcc gcc -fsyntax-only main.c`.
dte reads the diagnostics and jumps to the first source location. The
commands `msg -n` and `msg -p` move between messages.

dte uses the LixinOS raw console for input and ANSI sequences for output.
It needs an interactive terminal for editing. The port saves by writing a
temporary sibling file and renaming it. It uses dte's external-change check
on save, with PID-based file locking disabled by default.

## Build and install

On the Mac, run from the repository root with GNU Make 4.x available as
`gmake` (or set `DTE_MAKE` to the GNU Make executable). The build verifies
the pinned [dte source release](https://github.com/craigbarnes/dte/tree/v1.11.1)
and uses the existing LixinOS musl, GNU C startup and spawn adapter. The
package includes the upstream GPL-2.0 license.

```sh
bash User/dte/build.sh
python3 User/gcc/install.py --toolchain dte \
  Kernel/target/user-build/dte-port/package \
  "$HOME/Library/Application Support/LixinOS/daily.img"
```

The installer takes an initialized, powered-off LixinOS disk and adds
`/bin/dte`. Use that disk the next time you start either kernel.

## Verify

```sh
python3 scripts/test-dte.py --kernel both \
  --disk "$HOME/Library/Application Support/LixinOS/daily.img"
```

The test installs the package on disposable copies and exercises tabs,
search, undo and redo, UTF-8 paste, editing and saving existing and new
files, and jumping to a source location from a child process diagnostic.
