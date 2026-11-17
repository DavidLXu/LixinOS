# LixinOS

**0.0.1** · [中文系统手册](docs/README.md) · [版本说明](docs/0.0.1.md)

## Boot the Rust kernel in QEMU

The `Kernel/` crate is a bare-metal ARM64 guest for QEMU's `virt` board. On an Apple Silicon Mac with Homebrew and `rustup` installed, run the following. The launcher uses rustup's stable toolchain and also checks rustup's default install location when it is absent from `PATH`.

```sh
brew install qemu
rustup target add --toolchain stable aarch64-unknown-none-softfloat
./scripts/run-kernel.sh
```

If your shell cannot find `rustup`, use `"$HOME/.cargo/bin/rustup"` for the target setup command.

The default launcher opens the native QEMU Cocoa display. LixinOS draws its 82 × 24 console and fixed top status bar directly into a 1804 × 1100 ramfb framebuffer and reads a virtio keyboard using a US ASCII layout. The build uses Swift and Core Text to rasterize the locally installed Menlo font at 36 pixels. At 2× Retina scale the text appears at 18 pt. The default view uses the native pixel size. Press Control–Option–G to release captured input. Type `shutdown` at the shell prompt to power off. In MicroPython press Ctrl+D to return to the shell first.

The status bar samples once per second at console and Python VM polling points. CPU is the fraction of guest time spent outside input waits and the monitor's refresh delay. RAM counts the kernel image, stack, framebuffer, allocated heap and the 4 MiB user region reserved per pane against the configured 128 MiB. HEAP details the allocated kernel heap in KiB. The bar remains visible while scrolling, clearing the screen, or editing a file.

### Side-by-side terminals

Run `./scripts/run-kernel.sh --split` for two 52-column terminals in one native QEMU window. Press **Control–Tab** to pause the focused pane and resume the other, including inside Python, nano, htop and compiled C programs. Each pane remembers its working directory, variables, command draft and history navigation. Both use the same mounted filesystem. The status bar identifies the active pane. Only the focused pane executes. Each has its own kernel stack and 4 MiB user-program region. A 50 Hz ARM virtual timer checks for switching even in user code that never calls the kernel. Kernel operations switch at safe input or wait points. Both panes share the disk and guest uptime. Run `python3 scripts/test-panes.py` to verify pause/resume on a temporary disk.

### Other console modes

Run `./scripts/run-kernel.sh --terminal` for a macOS Terminal window with adjustable font size, or `./scripts/run-kernel.sh --serial` to use the calling terminal. These modes use the guest's PL011 serial port. All console modes use the same persistent disk.

### Shell and disk

The launcher creates an 8 MiB raw disk at `data/lixinos.img` on first use. Every successful file or directory change is committed to this disk before the next prompt. Launch again with the same image to resume. Stop the running VM before switching modes with the same disk. Set `LIXINOS_DISK` to an absolute path to select another image. Shell variables and the working directory reset on boot. For example:

```sh
mkdir demo
cd demo
name='hello world'
echo "$name" > notes
echo again >> notes
cat < notes
grep hello < notes | wc -l
```

The final command prints `1`. At the shell prompt, press Up or Down to browse the last 64 commands across boots on the same disk image. History is saved on submission; blank lines, consecutive duplicates, and `shutdown` are skipped. Down past the newest entry restores your unfinished line. Press Return to run the selected command.

Press Tab to complete a command at the start of a line or a path in an argument. Directory matches end in `/`. For ambiguous names, Tab fills the shared prefix and a second Tab lists the choices.

The shell accepts ASCII input, single and double quotes, backslash escapes, simple variable assignments and `$name` expansion, buffered pipelines, and file redirection. Shell built-ins run in kernel mode; standalone foreground commands `cecho` and `python` run at EL0. External Linux programs, Bash scripts with control flow, wildcard expansion, and job control are outside this version's scope. Each command line fits 512 bytes and each file fits 16 KiB.

Use `ls -a` to include dot-prefixed names and the directory entries `.` and `..`. Use `ls -l` for type, byte size and name, or `ls -lh` for B/KiB sizes. Directories show type `d` and a dash in the size column. Options can be combined, for example `ls -lah demo`.

### Edit a file

At the guest shell, type `nano hello.py` to open an existing file or start a new one. Type your script directly, using Return for new lines and Tab for indentation. Press **Ctrl+O** to write the named file, then **Ctrl+X** to return to the shell and run `python hello.py`. Exiting with unsaved changes offers Save (Y), Discard (N), or Cancel (Esc).

Use the arrow keys to move, Home/End for line boundaries, and Backspace/Delete to remove text. **Ctrl+K** cuts the current line and **Ctrl+U** pastes it at the cursor. The editor scrolls vertically and horizontally and accepts ASCII input. Each saved file uses the shell’s persistent disk and 16 KiB file limit.

### Memory and monitoring

Type `htop` at the guest prompt for a live dashboard. It refreshes once per second. Press `q` to return to the shell. In the QEMU window the last dashboard frame stays in the terminal history. Uptime comes from the ARM virtual counter. Heap usage comes from the allocator. File counts include empty files, directory counts include the root, and content bytes exclude filesystem metadata.

The launcher assigns one vCPU and 128 MiB of RAM. The kernel uses a 16 KiB boot stack and an 8 MiB first-fit heap. Split mode reserves a separate 32 KiB kernel stack for the right pane. Its page tables reserve 4 MiB of guest RAM per terminal for its foreground user program, with separate read/execute and read/write mappings. The shell and its built-ins run synchronously in kernel mode. The filesystem tree and file contents occupy heap memory while mounted. The kernel alternates between two checksummed disk snapshots, flushing the payload before publishing its header. On boot it loads the newest valid snapshot. The current limits are 128 entries including the root and 16 KiB per file.

### Run a C program or MicroPython

At the guest prompt, type `cecho`, enter a line, and press Return. The C program echoes the line and exits to the shell. Type `python` for an interactive MicroPython v1.25.0 session:

```text
lixin:/$ python
>>> x = 6
>>> x * 7
42
>>> for i in range(3):
...     print(i)
...
0
1
2
```

Finish a multiline block with a blank line. Press Ctrl+D at an empty `>>>` prompt to return to the shell. Each launch starts with a fresh 512 KiB Python heap. To save and run a script, enter these commands at the LixinOS shell prompt:

```sh
echo 'for i in range(3):' > hello.py
echo '    print("Hello", i)' >> hello.py
python hello.py
```

This prints `Hello 0`, `Hello 1`, and `Hello 2`. Restart QEMU with the same disk and run `python hello.py` again. The shell resolves the path in the guest filesystem and copies the saved source into the foreground program’s memory. Script execution returns to the shell on completion or an uncaught exception. Bare `python` still opens the interactive prompt.

Cargo builds both C images before embedding them. The first build needs Git, Make, Python 3 and network access to fetch the pinned MicroPython source and, on Apple Silicon, Arm's checksummed bare-metal GNU toolchain. Sources, toolchain and build products are cached under `Kernel/target/user-build/`. An existing `aarch64-none-elf-gcc` with newlib on PATH can also be used. Run `python3 scripts/test-user.py` to exercise the serial interface on a temporary disk.

### Compile and run C inside LixinOS

At the guest shell, use `nano hello.c` to write a C source file, or enter:

```sh
echo '#include <stdio.h>' > hello.c
echo 'int main(void) { puts("Hello from LixinOS"); return 42; }' >> hello.c
tcc -run hello.c
```

TinyCC compiles the saved source to ARM64 instructions in the guest and calls
`main(void)` at EL0. The example prints `Hello from LixinOS` followed by
`[exit 42]`. Editing the source and rerunning the command recompiles it immediately.

This single-file port provides an embedded `stdio.h` with `puts`, `putchar` and
`printf`. To keep a compiled program on the guest disk, run:

```sh
tcc hello.c -o hello
./hello
```

After restarting with the same disk image, run `./hello` again. The saved file
contains a versioned LixinOS executable header (LXE1), relocated ARM64 code,
and initial data. The loader supplies the guest stdio runtime and starts
`main(void)` with fresh data each time. Each executable fits the filesystem's
16 KiB file limit. A failed compile preserves any previous executable at the
output path. The `-run` form still compiles directly in memory.

The first host build fetches the pinned upstream TinyCC source (LGPL) into
`Kernel/target/user-build/tinycc`. The guest port and build recipe are in `User/tcc/`.

### Generate a script with the Agent

Save your DeepSeek API key once in the macOS Keychain. The `security` command prompts for the key without echoing it. Subsequent launches of LixinOS load it automatically.

```sh
/usr/bin/security add-generic-password -U -a "$(id -un)" -s LixinOS.DeepSeek -w
./scripts/run-kernel.sh
```

Set `DEEPSEEK_API_KEY` in the launch environment to override the stored key. Set it to an empty string to launch with the bridge disabled.

At the LixinOS prompt, give the Agent a new `.py` filename and a task. The shell accepts ASCII input. Use the normal `python` command to run the saved script.

```sh
agent double.py 'Read one integer with input() and print twice its value.'
python double.py
```

The Agent gets up to three test cases before requesting code. It runs each candidate in a fresh MicroPython instance at EL0, feeds the same test inputs on every attempt, and compares captured output and exit status. It sends a failed test's diagnostic back with the next request and tries up to three candidates. Tests cap captured output at 2 KiB and check a two-second deadline at MicroPython VM polling points and console calls. A passing script is committed to the guest disk and can be run after reboot, including when the bridge is offline. The output path must be new.

The Mac bridge sends the task, test cases, and any repair diagnostics to DeepSeek over HTTPS. The API key is read on the Mac. The default model is `deepseek-flash`, which you can override with `DEEPSEEK_MODEL`. The first version generates standalone MicroPython scripts using built-ins such as `input()` and `print()`. The guest's Python runtime has no filesystem or network APIs.

Run `PYTHONDONTWRITEBYTECODE=1 python3 scripts/test-agent.py` for the QEMU integration test with deterministic DeepSeek responses and a temporary guest disk.

### Inspect LixinOS with the Agent

With the model bridge enabled, ask a question at the LixinOS prompt.

```sh
agent inspect 'Report the guest CPU, RAM, disk, and filesystem usage.'
agent inspect 'List the files in the current directory and report their sizes.'
```

The Agent chooses up to six read-only queries. CPU count and RAM capacity come from the QEMU boot device tree. Heap usage comes from the allocator, and disk capacity and snapshot generation come from the mounted VirtIO disk. The filesystem queries report counts, limits, filenames, types and sizes. Relative paths resolve from the current directory. The busy percentage is an estimate from the graphical status bar when a sample is available. Total free physical RAM is unavailable.

The Mac bridge forwards the question and requested observations to DeepSeek. Inspection sends file metadata when requested, with file contents excluded. It leaves the guest disk and command history unchanged.
