# LixinOS

**0.1.0** · [中文系统手册](docs/README.md) · [版本说明](docs/0.1.0.md)

## Boot the Rust kernel in QEMU

The `Kernel/` crate is a bare-metal ARM64 guest for QEMU's `virt` board. On an Apple Silicon Mac with Homebrew and `rustup` installed, run the following. The launcher uses rustup's stable toolchain and also checks rustup's default install location when it is absent from `PATH`.

```sh
brew install qemu
rustup target add --toolchain stable aarch64-unknown-none-softfloat
./scripts/run-kernel.sh
```

If your shell cannot find `rustup`, use `"$HOME/.cargo/bin/rustup"` for the target setup command.

The default launcher opens the native QEMU Cocoa display. LixinOS draws its 82 × 24 console and fixed top status bar directly into a 1804 × 1100 ramfb framebuffer and reads a virtio keyboard using a US ASCII layout. The build uses Swift and Core Text to rasterize the locally installed Menlo font at 36 pixels. At 2× Retina scale the text appears at 18 pt. The default view uses the native pixel size. Press Control–Option–G to release captured input. Type `shutdown` at the shell prompt to power off. In MicroPython press Ctrl+D to return to the shell first.

The status bar samples once per second at console and Python VM polling points. CPU is the fraction of guest time spent outside input waits and the monitor's refresh delay. RAM counts the kernel image, stack, framebuffer, allocated heap and the 4 MiB user region reserved per pane against the configured 512 MiB. HEAP shows allocated kernel heap memory in KiB below 1 MiB, then in MiB to one decimal place. STACK shows sampled kernel stack use and capacity for the executing pane in KiB. FILES counts files on the mounted disk, including empty files. DISK shows how much of the usable snapshot capacity the current data occupies, including filesystem metadata and saved Agent versions. The bar remains visible while scrolling, clearing the screen, or editing a file.

### Desktop

Type `desktop` in a QEMU window. Click **文件** to browse the guest disk, a
folder to enter it, or **上一级** to return to its parent. The arrows and mouse
wheel page through longer listings.

Click a text file to edit it in Nano. To create a file, click **文本编辑器**,
type its path and press Enter. **Ctrl+O** saves and **Ctrl+X** returns to the desktop.

Click **终端** for a new window running the guest's `/bin/bash`. At a shell
prompt, click **[-] Desktop** or press **Ctrl+]** to put the window aside.
Use the desktop's **<** and **>** buttons to select a saved shell, then click
**Shell** to restore its screen, working directory and unfinished command.
Each window runs in the foreground. Type `exit` to close its shell.
**Esc** leaves the desktop and asks for a second press if sessions are still open.

Run `python3 scripts/test-desktop.py` (or add `--split`) for the QEMU test.

### Side-by-side terminals

Run `./scripts/run-kernel.sh --split` for two 52-column terminals in one native QEMU window. Press **Control–Tab** to pause the focused pane and resume the other, including inside Python, nano, htop and compiled C programs. Each pane remembers its working directory, variables, command draft and history navigation. Both use the same mounted filesystem. The status bar identifies the active pane. Only the focused pane executes. Each has its own kernel stack and 4 MiB user-program region. A 50 Hz ARM virtual timer checks for switching even in user code that never calls the kernel. Kernel operations switch at safe input or wait points. Both panes share the disk and guest uptime. Run `python3 scripts/test-panes.py` to verify pause/resume on a temporary disk.

### Other console modes

Run `./scripts/run-kernel.sh --terminal` for a macOS Terminal window with adjustable font size, or `./scripts/run-kernel.sh --serial` to use the calling terminal. These modes use the guest's PL011 serial port. All console modes use the same persistent disk.

### Connect from a Mac terminal over SSH

From the project root, start a guest with its serial console attached to a
loopback-only SSH listener:

```sh
./scripts/run-kernel.sh --ssh
```

Keep the launcher running. In another Mac terminal, use the command printed by
the launcher. With the default settings, from the project root this is:

```sh
ssh -i data/ssh/id_ed25519 -p 2222 lixin@127.0.0.1
```

On first login, compare the host fingerprint with the one the launcher printed.
SSH mode creates a private Python environment under `Kernel/target` (Python
3.10+ and internet access are needed on first setup). It creates a dedicated
client key and a persistent host key in `data/ssh`. Set `LIXINOS_SSH_PORT` to
choose another port or `LIXINOS_SSH_DIR` to choose another key directory.

Open the printed SSH command in two Mac terminals to get two guest shells on the
same running VM. The login banner shows each SSH terminal ID. With no arguments,
SSH resumes the oldest detached terminal, or creates one if all are attached.
To choose explicitly, use either command below from the project directory.

```sh
ssh -t -i data/ssh/id_ed25519 -p 2222 lixin@127.0.0.1 new
ssh -t -i data/ssh/id_ed25519 -p 2222 lixin@127.0.0.1 attach 1
```

To detach, press Return and type `~.`. Reconnecting preserves a live session,
including a Python REPL. Press Return for a fresh prompt if needed. Type
`exit` in the last shell of an SSH terminal to close that terminal. Any shell
can run `shutdown` to stop the VM. Stop a guest using the same disk before
launching another instance.

Inside a local console or SSH window, `shell new` creates a shell at `/`,
`shell list` lists that terminal's shells (the star marks the current one),
`shell use 1` switches to shell 1, and `shell close` closes the current shell.
Shell numbers are local to each terminal. Shells are allocated on demand within
available memory. Each keeps its working directory, variables, exit status and
history navigation. They share files and the disk's recent-command log.
Rebooting discards live sessions.

SSH uses one guest foreground execution slot. At shell prompts, multiple windows
can edit command lines independently. While one window runs Python or an editor,
other windows' input waits until that program returns. In Python, Ctrl+D
releases the foreground slot. Run `python3 scripts/test-sh.py` and
`python3 scripts/test-ssh.py` to exercise both interfaces on temporary disks.

### Guest outbound networking

The launcher attaches an Ethernet NIC on virtio-mmio bus 4. LixinOS runs ARP,
IPv4 and TCP in the guest. QEMU translates outgoing connections using its user
network. Run `netinfo` to see the NIC's MAC, static IPv4 address (10.0.2.15/24)
and default gateway (10.0.2.2). On this virtual network, 10.0.2.2 also provides
access to the Mac's loopback services.

To test a listening TCP service, build the included client inside the guest.
For example, if the Mac is listening on port 2222, run:

```sh
tcc /src/net/tcpcheck.c -o /bin/tcpcheck
tcpcheck 10.0.2.2 2222
```

Guest C programs can include `<lixin.h>` and use `lx_tcp_connect`,
`lx_tcp_read`, `lx_tcp_write` and `lx_tcp_close`. Each process has eight TCP
handles. Connect accepts numeric IPv4 addresses and waits up to five seconds.
Reads and writes can complete partially and return `LX_TCP_AGAIN` while
waiting for network progress. A read of zero bytes with a nonzero requested
length signals peer EOF. The kernel polls the stack during socket calls and
idle console input. Run `python3 scripts/test-net.py` to verify it with QEMU and a temporary
Mac loopback service.

### Connect to a remote SSH server from inside LixinOS

At the guest shell, run `ssh user@IPv4` or specify a port:

```sh
ssh -p 2222 guest@10.0.2.2
```

This example reaches an SSH server listening on the Mac's port 2222. Use your
server's numeric IPv4 address, username and port for a remote machine. The
server must offer an Ed25519 host key and SSH password authentication. The
existing Mac-to-guest console uses public-key login and serves a different role.

On the first connection, compare the displayed SHA256 fingerprint with the
server administrator's fingerprint before typing `yes`. Each trusted address
and port has its own disk record, for example `/.ssh-host-10.0.2.2-2222`.
A changed key stops login before the client asks for a password. For a verified
server key rotation, remove just that record and confirm the new fingerprint.
The password prompt disables local echo. Type `exit` in the remote shell to
return, or enter `~.` at the beginning of a line to disconnect locally.

The disk-resident `/bin/ssh` runs Sunset 0.6.0 at EL0 using the guest TCP stack.
QEMU provides host OS randomness through a separate virtio-rng device. The port
and its locked dependency manifest are under `/src/ssh`. Run
`python3 scripts/test-ssh-out.py` on the Mac to exercise authentication,
interactive terminal I/O, reboot persistence and host-key rejection in QEMU.

### Shell and disk

The launcher creates a sparse raw disk (128 MiB + 65 KiB + one 512-byte layout header) at `data/lixinos.img` on first use. Every successful file or directory change is committed to this disk before the next prompt. Launch again with the same image to resume. Stop the running VM before switching modes with the same disk. Set `LIXINOS_DISK` to an absolute path to select another image. Shell variables and the working directory reset on boot. For example:

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

The shell accepts ASCII input, single and double quotes, backslash escapes, simple variable assignments and `$name` expansion, buffered pipelines, and file redirection. LixinOS Bash is the default interactive shell, written in C and running at EL0 from `/bin/bash`. The stock `/bin/sh` entry follows `/bin/bash`. It owns aliases, variables, PATH lookup, redirection and pipeline sequencing. Edit `/src/bash/bash.c` and rebuild in the guest with `tcc /src/bash/bash.c -o /bin/bash`. Start another shell with `sh` and use `exit` to resume its parent. Enter `q` followed by Return to invoke the shutdown alias. External Linux programs, Bash scripts with control flow, wildcard expansion, and job control are outside this version's scope. Each command line fits 512 bytes and each file fits 1 MiB.

`ls` defaults to the `ls -lh` layout with type, a size in B or KiB, and name. Use `ls -1` for names only, `ls -l` for sizes in bytes, and `ls -a` to include dot-prefixed names and the directory entries `.` and `..`. Directories show type `d` and a dash in the size column. Interactive listings show directories in cyan and regular files in the default text color. Pipes and redirected listings use plain text.

### Edit a file

At the guest shell, type `nano hello.py` to open an existing file or start a new one. Type your script directly, using Return for new lines and Tab for indentation. Press **Ctrl+O** to write the named file, then **Ctrl+X** to return to the shell and run `python hello.py`. Exiting with unsaved changes offers Save (Y), Discard (N), or Cancel (Esc).

Use the arrow keys to move, Home/End for line boundaries, and Backspace/Delete to remove text. **Ctrl+K** cuts the current line and **Ctrl+U** pastes it at the cursor. The editor scrolls vertically and horizontally and accepts ASCII input. Each saved file uses the persistent disk. Both editors accept files up to the 1 MiB filesystem limit.

Open `kilo main.c` for C/C++ syntax highlighting. **Ctrl+F** starts incremental
search, the arrow keys step through matching lines, Enter accepts a match and
Esc cancels. **Ctrl+S** saves and **Ctrl+Q** quits. Both Kilo and nano run in EL0.
Each editor can replace its opened file atomically. Saving detects edits made
in the other pane. Its non-executable heap grows in private blocks on demand
and is released when the editor exits.

### Memory and monitoring

Type `htop` at the guest prompt for a live dashboard. It refreshes once per second. Press `q` to return to the shell. In the QEMU window the last dashboard frame stays in the terminal history. Uptime comes from the ARM virtual counter. Heap usage comes from the allocator. File counts include empty files, directory counts include the root, and content bytes exclude filesystem metadata.

The launcher assigns one vCPU and 512 MiB of RAM. The kernel uses a 128 KiB boot stack and a first-fit heap above the kernel and user banks. Split mode reserves a separate 128 KiB kernel stack for the right pane. Its page tables reserve 4 MiB of guest RAM per terminal for its foreground user program, with separate read/execute and read/write mappings. The kernel loads validated images and suspends a calling process while its child runs. The filesystem tree and file contents occupy heap memory while mounted. The kernel alternates between two checksummed disk snapshots, flushing the payload before publishing its header. On boot it loads the newest valid snapshot. The current limits are 1,048,576 entries including the root and 1 MiB per file. A snapshot has 64 MiB minus its 512-byte header for all names, contents and version history together. The two 64 MiB slots follow the layout header. A separate 65 KiB ring at the end keeps the latest 64 shell commands. On first launch with an older disk, the launcher copies valid snapshots and history to a temporary image and atomically replaces the old image after verification. Use `touch --files-from names.txt` to create a batch of files with one path per line and one snapshot commit.

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

### Compile and run Rust inside LixinOS

The user-space `/bin/lust` bundles the Lust Rust-subset frontend and the ARM64 TinyCC backend. Run these commands at the guest shell:

```sh
lust build /src/lust/hello.rs -o hello
./hello
```

The example prints `Hello from LixinOS Rust: 42`. For your own program, use `nano demo.rs`, save it, then run `lust build demo.rs -o demo` and `./demo`. Supported language features, executable limits and host package details are in [the port notes](User/lust/README.md).

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

TinyCC supports both `main(void)` and `main(int argc, char **argv)`. Pass
arguments with `tcc -run source.c arg` or save the program and run it later:

```sh
tcc hello.c -o hello
./hello
```

Saved C and Rust programs use LXE1 and can fill a 1 MiB file including the
16-byte header. The fixed ARM64 memory layout allows up to 512 KiB of code
and 860 KiB of data (including zero-initialized storage) within that total.
TinyCC's in-memory `-run` mode has a separate 256 KiB code buffer. Failed builds preserve the previous executable.
The guest supplies a small `<stdio.h>` with `getchar`, `putchar`, `puts`,
`printf`, `fprintf`, and read-only `fopen`, `fread`, and `fclose`.
Use `fprintf(stderr, ...)` for diagnostics and `echo $?` to inspect the last
stage's exit status. Saved programs work with the shell's text pipes and
redirections. File reads support binary data. The guest's `<lixin.h>` provides
`lx_stat` and `lx_dir` for metadata and directory enumeration, plus
`lx_open`, `lx_read`, and `lx_close` for descriptor-based reads.

To run the program by name from any directory, compile it into the guest's
persistent `/bin` directory:

```sh
tcc hello.c -o /bin/hello
mkdir work
cd work
hello
```

Each shell starts with `PATH=/bin`. Use `echo $PATH` to inspect it or
`PATH=/tools:/bin` to search additional directories in order. System commands also have files in `/bin`. For example, `/bin/ls /bin` lists
them and `/bin/cd /work` changes the current shell's directory. The guest stores `/bin/ls` as a statically linked AArch64 ELF executable.
The shell loads its code from disk into EL0. The program reads filesystem
metadata through a syscall and handles its options and formatting in C. The
loader supports a single fixed-address read/execute segment and the LixinOS
calling convention. Other system launcher files still select built-in handlers.
Bare names select the first runnable file on `PATH`. Boot replaces the original
`ls` launcher with the ELF and preserves user replacements. The guest also installs
the editable source as `/src/ls/ls.c` when that path is free. To rebuild and install
your own LXE1 version from the guest shell:

```sh
nano /src/ls/ls.c
tcc /src/ls/ls.c -o /bin/ls
ls -lah /bin > listing.txt
cat listing.txt
```

Restart with the same disk to use the saved executable and source again.
The guest mirrors project-owned user program sources and build inputs under
`/src`. Run `cat /src/README.md` for the guest rebuild commands for `ls`,
`cecho`, and `agent-run`. Existing source files keep their edits. When migrating
an older disk, the initial copies of `ls/ls.c` and `agent/run.c` inherit the
contents of the old `/ls.c` and `/agent.c` if present.

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

### Improve and roll back a saved script

At the LixinOS prompt, give the Agent an existing `.py` path and a requirement. Then run the script by its usual name. The activated version survives a reboot.

```sh
agent improve double.py 'Print invalid for non-integer input and keep the output for integers.'
python double.py
```

For an exact acceptance example, save a JSON array of `stdin` and `stdout` strings and pass it with `--examples`. Include output newlines where expected.

```sh
echo '[{"stdin":"oops\n","stdout":"invalid\n"}]' > examples.json
agent improve double.py 'Print invalid for non-integer input.' --examples examples.json
```

The guest runs the active script's saved tests before proposing a change. Each candidate must pass those tests and the new examples before the guest commits its code, updated tests and previous version together. A failed improvement keeps the active version. The saved suite supports up to 12 distinct inputs.

Scripts created before version tracking may have no saved tests. Pass `--baseline old.json` with the original cases if you have them. With no saved or supplied tests, the Agent proposes baseline cases from the current source and checks them against the current script before making changes. The Mac forwards the script, tests and repair feedback to DeepSeek.

```sh
agent rollback double.py
```

Rollback restores the preceding script and its tests in one commit and can run offline. Each script has one rollback slot, consumed on restore. The current kernel mounts the earlier disk format as well as the extended format with version records. Back up the disk before booting it with an older kernel.

### Run the user-space project agent

Choose a guest project directory and give the Agent a task.

```sh
mkdir /work
agent run /work 'Create hello.py to print Hello, then test the exact output.'
```

The shell starts the disk-resident `/bin/agent-run` executable at EL0. It asks DeepSeek to choose a tool, yields the foreground slot while the guest executes it, then resumes the same user process with the result. The Mac carries model requests and responses. The guest broker restricts operations to the chosen directory, permits text reads and staged `.py` edits, and uses the existing MicroPython test runner. An edit stays in memory until it passes testing and you press `y` after inspecting the current and proposed content. Approved files commit together. Use `agent improve` for scripts already managed by its regression and rollback workflow.

Press **Ctrl+P** while the agent is waiting for the model, running a test, or awaiting approval to pause and add a follow-up at the `agent>` prompt. Press Enter on an empty line to resume. An interrupted model response is ignored and an interrupted test is eligible to run again. Press **Ctrl+G** at any of these points to cancel the task and discard staged edits. Tests observe these keys at MicroPython polling points. A disk commit that has already started completes atomically.

The user-space program owns the tool budget and console log format. Project sessions default to 50 tool calls and 512 bytes of preview per result. Use `agent tune` to ask the agent to change those settings. It stages and tests `/agent-policy/settings.py`, shows the proposed change, and waits for approval. An approved policy takes effect on the next run and survives reboot. `agent rollback-tune` restores the previous settings.

```sh
agent tune 'Keep the tool budget and show up to 768 bytes per result.'
agent rollback-tune
```

The initial source is installed as `/src/agent/run.c`. Edit the source and compile a replacement inside LixinOS. The next `agent run` loads the file from `/bin`; reboot also retains the replacement.

```sh
nano /src/agent/run.c
tcc /src/agent/run.c -o /bin/agent-run
```

### Apply system-wide Agent changes

Run `agent system "task"` to grant the Agent access to the LixinOS guest volume. It can stage up to 16 filesystem entries across directories in one task. It supports text edits, binary data, copying files, creating directories, removing files and empty directories, and compiling C source with guest TCC into a staged executable. Compiling checks that TCC accepts the source. It does not run the executable.

The Agent shows a preview and waits for `y` before committing the whole batch and its rollback data in one snapshot. Type `n` to discard the batch. `agent system-rollback` restores the last approved system batch, including after reboot. Rollback refuses to overwrite later changes to the same entries. The root directory and the reserved rollback records are managed by LixinOS. System mode targets the guest volume, while the Mac still relays model requests.

```sh
agent system 'Create /src/system-demo.c and use TCC to stage /bin/system-demo. It should print Hello from LixinOS.'
agent system-rollback
```

### Local Git repositories

LixinOS runs an ARM64 C client linked with libgit2 1.9.1 in the foreground user slot.
Create a project directory and use these commands from the guest shell.

```sh
mkdir project
cd project
git init
echo hello > note.txt
git status
git add note.txt
git diff --staged
git commit -m "First version"
echo world >> note.txt
git diff
git log
```

`git add .` stages the current directory, including tracked deletions.
The initial branch is `main`. Commits use the guest clock and the identity
`LixinOS <user@lixinos.local>`. The guest Git port currently transfers files, including its index and objects, up to 16 KiB. Each command saves
its metadata in one snapshot.

From the project root on the Mac, export into a **new directory**:

```sh
python3 scripts/export-git.py /project ~/Desktop/lixinos-project
git -C ~/Desktop/lixinos-project status
git -C ~/Desktop/lixinos-project log --oneline
```

The exporter copies the worktree and its standard `.git` directory, including
the current index and untracked files. It reads the newest valid snapshot from
`data/lixinos.img`; use `--disk /path/to/image` for a different disk.
Run `python3 scripts/test-git.py` for the disposable-disk interoperability test.
The build fetches pinned libgit2 sources, including its COPYING file, into the
user build cache.
