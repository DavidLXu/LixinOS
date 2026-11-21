# LixinOS

**0.4.0** · [中文系统手册](docs/README.md) · [版本说明](docs/0.4.0.md)

## Choose a kernel at startup

Run `./scripts/run-kernel.sh` to open the macOS kernel picker before QEMU starts. Choose the original Rust kernel (default) or the guest-built TCC kernel. Both kernels prefer the local daily work disk once initialized. Otherwise the launcher uses `data/shared.img` if installed. Enter `q` and press Return before switching kernels. Interactive `--serial` and `--ssh` sessions show a numbered menu. For unattended startup, pass `--kernel rust` or `--kernel tcc` with the desired display mode, for example `./scripts/run-kernel.sh --kernel tcc --split`.

## 我的工作盘

在 Mac 上运行 `python3 scripts/workdisk.py status` 查看工作盘和备份。首次设置时关掉使用原盘的虚拟机，运行
`python3 scripts/workdisk.py init data/desktop-pingfang.img`。启动器使用本地副本
`~/Library/Application Support/LixinOS/daily.img`。两个发布内核共用项目和历史；原镜像保存在项目目录。
`LIXINOS_DISK` 可指定其他盘进行测试。

关机后在 Mac 上用以下命令备份。需要恢复时，把备份路径和要找回的 guest 文件路径交给 `restore`。

```sh
python3 scripts/workdisk.py backup
python3 scripts/workdisk.py status
python3 scripts/workdisk.py restore "/完整路径/备份.img" /home/lixin/hello.py
```

目标父目录须存在。要覆盖同名文件，在命令末尾加 `--replace`。本地备份也可以复制到外置盘保管。

在日常工作盘的桌面打开 **Apps → settings.win3**，可以调整默认编辑器、登录与开发环境、Agent 限额、时区、显示和连接选项。各项会标明生效时间。[设置使用说明](User/desktop/README.md#系统设置)

开机进入 `/home/lixin`。用 `edit .profile` 修改登录设置，打开新 shell 或重启后生效。
每行可以写变量赋值、`export NAME=value` 或 `cd 目录`，支持引号、变量展开和注释。
例如添加 `export EDITOR=kilo` 后，`edit hello.py` 会使用 Kilo。
`cd` 回到 `$HOME`；编译到 `$HOME/bin` 的程序可以直接按名字运行。

前台程序或管道卡住时按 **Ctrl+C**，随后用 `echo $?` 可看到退出码 130。
Desktop 的独立窗口应用可点右上角关闭，程序会先收到管道关闭通知，250 毫秒后仍在运行的子进程会被结束。

## CPython in the guest

Run `python3` for the CPython 3.14.5 REPL or `python3 script.py` to execute a guest file. Both kernels use the same native AArch64 interpreter. The existing `python` command runs MicroPython. See the [CPython port guide](User/cpython/README.md) for the supported modules and two-kernel regression test.

## Boot the guest-built C kernel

LixinOS can rebuild a separate AArch64 kernel and its disk bootloader from C and assembly using its own TCC. With the prepared image, run `./scripts/run-tcc-kernel.sh --window`. The TCC kernel uses 24 GiB of QEMU RAM with the shared build-capable disk and 1536 MiB with its smaller legacy disk. The Agent service is still part of the Rust-kernel configuration. See the [native kernel build and verification guide](Kernel/native/README.md).

## Boot the Rust kernel in QEMU

The `Kernel/` crate is a bare-metal ARM64 guest for QEMU's `virt` board. On an Apple Silicon Mac with Homebrew and `rustup` installed, run the following. The launcher uses rustup's stable toolchain and also checks rustup's default install location when it is absent from `PATH`.

```sh
brew install qemu
rustup target add --toolchain stable aarch64-unknown-none-softfloat
./scripts/run-kernel.sh
```

If your shell cannot find `rustup`, use `"$HOME/.cargo/bin/rustup"` for the target setup command.

The default launcher opens a native QEMU Cocoa window at the guest display size. If macOS Accessibility access is unavailable to the launcher, enable **View → Zoom To Fit** in QEMU after launch to unlock window dragging. Drag the window edge to change the number of terminal rows and columns. LixinOS reflows visible text at the same Menlo cell size and sends the new terminal dimensions to connected SSH sessions. Split mode gives each pane half the available width. Nano in the main QEMU console updates its layout as you resize the window. The desktop, Paint and Kilo apply pending resizes when they return to the main console. Enlarge the window before opening the desktop if it has too little space. Press Control–Option–G to release captured input before resizing. Type `shutdown` at the shell prompt to power off. In MicroPython press Ctrl+D to return to the shell first.

The status bar samples once per second at console and Python VM polling points. CPU is the fraction of elapsed guest time outside scheduler idle waits. RAM counts the kernel image, stack, framebuffer, allocated heap (including live process images and stacks) against the available guest RAM. HEAP shows allocated kernel heap memory in KiB below 1 MiB, then in MiB to one decimal place. STACK shows sampled kernel stack use and capacity for the executing pane in KiB. FILES counts files on the mounted disk, including empty files. DISK shows how much of the usable snapshot capacity the current data occupies, including filesystem metadata and saved Agent versions. The bar remains visible while scrolling, clearing the screen, or editing a file.

The clock at the right shows 24-hour local time. Narrow windows show hours
and minutes. The launcher uses Asia/Shanghai by default. Set
`LIXINOS_TIMEZONE` to use a different time zone at startup.

### NetSurf graphical browser

The [NetSurf package](User/netsurf/README.md) adds NetSurf 3.11 to both kernels.
Run `netsurf https://example.com/` in a QEMU graphics window or click **Browser**
in the packaged desktop. Press **Ctrl+L** to enter an address.

### Links 2 browser

Run `links` in the guest shell, or open a guest file with `links hello.html`.
Use **Tab** or the arrow keys to select a link, **Enter** to open it, **B**
to go back, **G** to enter an address and **Q** to return to the shell.
The command `links2` also starts the browser.

To browse files served by your Mac, run the following in the Mac directory
containing your HTML files.

```sh
python3 -m http.server 8000 --bind 127.0.0.1
```

Then run `links http://10.0.2.2:8000/` inside LixinOS. The port uses the
Links 2.30 HTML and table renderer. Open a public site directly in the guest:

```sh
links https://example.com
```

In a QEMU window, use `links -g https://example.com` for the graphical browser,
or click **Browser** in the desktop dock. Click a link to follow it, the address bar
to type a URL, **Back** to return, or **×** to close. Drag the title bar to
move the window. The mouse wheel scrolls by a page.

LixinOS resolves IPv4 hostnames through QEMU user networking's DNS server.
The browser uses BearSSL with TLS 1.2, SNI, certificate chain and hostname
verification, the QEMU RTC for validity dates, and virtio-rng for entropy.
The build embeds the public roots from Homebrew's CA bundle when available,
falling back to `/etc/ssl/cert.pem`. Rebuild after updating the bundle.
The HTML body limit is **10 MiB** (10,485,760 bytes). The browser applies this limit to both local and network documents.
Both interfaces lay out UTF-8 Chinese text with full-width characters and
line wrapping. Serial mode emits UTF-8 for the terminal on the Mac. The
graphical browser carries its own font for common simplified and traditional
Han characters and Chinese punctuation.

Graphical pages display inline PNG, JPEG and GIF images (first frame). Images
keep their aspect ratio and fit the viewport. Each page can retain up to 32
images with a combined limit of 4,194,304 decoded pixels. Text mode uses alt text.

Classic inline and external JavaScript runs inside LixinOS using Duktape 2.7.
Each scripted page gets a Domino 2.1.6 document and a Duktape context with a
24 MiB allocation limit. Scripts can query, create and modify DOM nodes,
listen for clicks, and update the page during DOMContentLoaded or load.
The renderer redraws the resulting document after callbacks. Dynamically
inserted classic scripts can load from the page's own HTTP origin. The foreground event loop runs `setTimeout`, `setInterval` and their
cancellation functions. A same-origin `fetch(url)` issues a GET and returns a
promise with `status`, `ok`, `text()` and `json()`. Responses for scripts are
limited to 1 MiB. The transport processes one request at a time. Leaving the
document discards pending callbacks. Search fields submit GET forms. HTTP and
JavaScript share a host-scoped, in-memory cookie jar for the browser session.
JavaScript modules, full CSS, POST forms and TLS 1.3 remain unsupported.

Run `python3 scripts/test-links.py --online` to test local HTML, HTTP,
public DNS and HTTPS, and rejection of an untrusted certificate on a disposable
disk. Run `python3 scripts/test-links-window.py` to exercise the desktop
browser with QEMU mouse events. Run `python3 scripts/test-links-pages.py` for
the exact 10 MiB boundary and script execution tests, and
`python3 scripts/test-links-window.py --pages` for image and Chinese glyph checks.
Run `python3 scripts/test-links-window.py --events` to click a button with the
QEMU mouse and check the server's JSON value in the repainted document.
For a live Google compatibility check, run `python3 scripts/test-links.py --google`.
The graphical guest currently displays the Google home page and accepts a query.
Google returns its JavaScript compatibility page after submission. The live
check requires a visible IANA result and currently fails at that step.

In the QEMU console or a desktop terminal, place the pointer over the text and
use the mouse wheel to review output. Each notch moves three rows. Each terminal
retains up to 1,024 rows and returns to live output when it receives new text.
In split mode the wheel acts within the focused pane.

### Desktop

Type `desktop` in a QEMU window. The left dock opens **Files**, **Browser** (NetSurf when installed, otherwise Links),
**Calc**, **Terminal**, **Nano**, **Editor**, **Eyes** and **Apps**.
Use **Apps** to launch independent window programs from /bin and its **Windows**
tab to switch instances or restore minimized windows. Build one in the guest
with `tcc /src/desktop/counter.c -o /bin/counter.win`. Open **stopwatch.win2** in
Apps for a resizable example with timer-driven partial redraw. Space starts or
pauses the stopwatch and R resets it. Files, Browser and Calc can stay
open together. Click a window or its dock icon to bring it to the front. Drag
its title bar to move it, or drag its lower-right corner to resize it. The
**−**, **□** and **×** buttons minimize, maximize/restore and close the window.
Click its dock icon to restore a minimized app with its contents intact.

Click **Files** to browse the guest disk, a
folder to enter it, or **上一级** to return to its parent. The arrows and mouse
wheel page through longer listings.

Click a text file to edit it, or click **Nano**, type a path and press Enter
to create one. Click in the document to place the caret and use the arrow keys
to move it. Drag across text to select, including across lines. **Ctrl+C** copies,
**Ctrl+X** cuts, and **Ctrl+V** pastes from the guest clipboard. Typing replaces
the selection. **Ctrl+A** selects the document, **Ctrl+S** saves and **Ctrl+Q**
returns to the desktop. You can also click the actions along the bottom of the editor. The shell command `nano` retains its console shortcuts.

Click **Terminal** to open Bash. Click **+** in the title bar to open another
tab and click a tab number to select it. Up to four tabs can run independently
in each pane. A command keeps running when you switch tabs and its output
stays in its own tab. Keyboard input and Ctrl+C go to the selected tab.
**×** or `exit` closes the selected session. **−** hides the tab group while
its sessions keep running. Use the session number at the bottom of the dock
to reopen it. At a shell prompt, Ctrl+] also hides the group.

#### Desktop window conventions

- Every application window has a 44-logical-pixel title bar and a **×** button in the upper right. The file browser shows the directory it is displaying. The editor and terminal show the application name.
- Hold the left mouse button on the title bar and drag to move the entire window. Releasing the button leaves it in place. Window edges stay within the desktop, including in split mode. Title-bar buttons handle clicks separately from dragging.
- **×** closes the foreground window. The file browser returns to the desktop. The editor offers **Y** to save, **N** to discard, or **Esc** to continue editing when there are unsaved changes. The filename dialog can also be dragged or closed.
- In a terminal, **×** ends that shell session along with its foreground command. The separate **−** button hides the tab group and keeps its sessions running.

Run `python3 scripts/test-desktop.py` (or add `--split`) for the QEMU test.

### Side-by-side terminals

Run `./scripts/run-kernel.sh --split` for two 68-column terminals in one 3014 × 1408 native QEMU window. Press **Control–Tab** to change keyboard focus. Both panes keep running and share the mounted filesystem. A 50 Hz virtual timer preempts EL0 code, including compute loops. Each scheduled process has its own PID, page tables and kernel stack. Bash accepts a trailing `&` to start an external command or pipeline in the background, for example `tcc hello.c -o hello &`. An external pipeline uses a 4 KiB pipe between each pair of stages. Writers wait for space and readers wait for data or EOF. `scripts/test-panes.py` exercises simultaneous editing, compilation and a TCP job that survives closing its launching shell. `scripts/test-jobs.py` exercises process isolation, memory reclamation and pipe backpressure on a disposable disk.

### Other console modes

In a QEMU window, the pointer becomes an I-beam over selectable text. Drag
to highlight text, then right-click and choose **Copy**. Choose **Paste** in
the same menu to insert clipboard text into the focused input. The menu is
available across the graphical console and application canvases. Text
selection is supported by terminals, the graphical Nano window, Text Editor
and desktop filename fields. The Mac clipboard bridge supports up to 4 KiB
of plain ASCII per paste. Nano, Kilo and Text Editor preserve tabs and line
breaks. At the shell, these become spaces and Return submits the command.
**Ctrl+Shift+C** and **Ctrl+Shift+V** also work in the graphical console.

In macOS Terminal, including SSH sessions, use the terminal app's usual
**Command+C** and **Command+V** shortcuts.

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
It requests an `xterm` PTY with the current guest console dimensions. The
graphical console handles cursor visibility and alternate screens for
terminal applications such as curses dashboards.
QEMU provides host OS randomness through a separate virtio-rng device. The port
and its locked dependency manifest are under `/src/ssh`. Run
`python3 scripts/test-ssh-out.py` on the Mac to exercise authentication,
interactive terminal I/O, reboot persistence and host-key rejection in QEMU.

#### Transfer files to and from the Mac

Enable Remote Login on the Mac for your account. From the guest, use the Mac
username and QEMU host address. SFTP shares the SSH fingerprint records and the
enrolled GPU20 profile. Quote paths containing spaces.

```text
sftp xulixin@10.0.2.2
sftp> get -r Documents/hello hello
sftp> quit
tcc hello/main.c -o hello/app
./hello/app
sftp xulixin@10.0.2.2
sftp> put -r hello Documents/hello-from-LixinOS
sftp> quit
```

Use `-P PORT` for a custom port. An existing destination directory receives the source under its basename. A download publishes each complete regular file atomically.
Uploads use a temporary remote file and rename it after closing it. Replacing
an existing remote file requires the server's OpenSSH POSIX rename extension.
Transfers currently support files up to 100 MiB and 32 directory levels.

#### GPU20 through a Mac jump host

On an enrolled disk, type `ssh gpu20` at the LixinOS prompt. The guest SSH
client verifies the final server’s pinned Ed25519 host key and signs in with
the guest’s dedicated Ed25519 identity. The QEMU-only endpoint
`10.0.2.100:22` starts a byte forwarder on demand. It uses the destination
and single ProxyJump from the Mac’s `ssh -G gpu20` configuration. The jump
host must already be trusted by OpenSSH and accessible in batch mode.

For a new disk, run `ssh --public-key gpu20` inside the guest to create the
identity and print its public key. Enroll that public key in the intended
remote account’s `~/.ssh/authorized_keys`. Save the remote username and the
independently verified Ed25519 host fingerprint as one space-separated line
in the guest file `/.ssh-gpu20`. The guest refuses a mismatched host key.
The disk contains the private seed at `/.ssh-gpu20-key`. Anyone with access
to the disk or its guest filesystem can read it. To revoke this disk’s
access, remove its public key from the remote account.

### Shell and disk

The shared daily disk is `data/shared.img`. Both kernels see the same programs, sources, build products and shell history. Use `df` in the guest to see free space. Build under `/scratch` and copy finished programs to `/artifacts`. A clean shutdown with `q` flushes pending scratch changes. Only one VM can write the disk at a time.

To migrate an installation, shut down its VMs and run the following from the repository root with Python 3.11 or newer. Supply the boot disk from a paired release that supports the 4 GiB layout. The optional toolchain argument copies the stage1 binaries and their matching runtime from a completed build volume. The installer keeps both original images. It takes Rust's command history, merges files unique to either disk, prefers Rust's daily system files on conflicts, and installs the released TCC kernel with its matching loader and sources.

```sh
python3.13 scripts/prepare-shared-disk.py \
  --boot data/kernel-releases/kernel-2026-11-20.1/data/tcc-os/system.img \
  --toolchain data/tcc-bootstrap.img
```

Without a shared disk, Rust defaults to `data/lixinos.img` and TCC to `data/tcc-os/system.img`. Select a different disk with `LIXINOS_DISK`, or set `LIXINOS_TCC_DISK` for TCC only. Use absolute paths. Shell variables and the working directory reset on boot. For example:

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

The shell accepts ASCII input, single and double quotes, backslash escapes, simple variable assignments and `$name` expansion, buffered pipelines, and file redirection. LixinOS Bash is the default interactive shell, written in C and running at EL0 from `/bin/bash`. The stock `/bin/sh` entry follows `/bin/bash`. It owns aliases, variables, PATH lookup, redirection and pipeline sequencing. Edit `/src/bash/bash.c` and rebuild in the guest with `tcc /src/bash/bash.c -o /bin/bash`. Start another shell with `sh` and use `exit` to resume its parent. Enter `q` followed by Return to invoke the shutdown alias. External Linux programs, Bash scripts with control flow, wildcard expansion, and job control are outside this version's scope. Each command line fits 512 bytes and each file fits 100 MiB.

`ls` displays names in columns sized to the terminal. Use `ls -1` for one name per line, `ls -l` for details with sizes in bytes, and `ls -lh` for details with sizes in B or KiB. Add `-a` to include dot-prefixed names and the directory entries `.` and `..`. Detailed listings include the UTC modification time and show directories with type `d` and a dash for size. Interactive listings show directories in cyan and regular files in the default text color. Pipes and redirected listings default to one name per line in plain text.

`stat PATH` shows creation and modification times. Writes and `touch` update
the modification time at one-second resolution. Renaming preserves both
timestamps. Directories update their modification time when children are
created, removed, or renamed. Python exposes the modification time as
`os.stat(path)[8]`. Entries carried over from older snapshots display a dash
for unknown times. The kernel reads older snapshots and writes timestamps in
`LIXINFS3` snapshots on the same disk image.

### Everyday commands

| Task | Examples |
| --- | --- |
| Copy, move and remove | `cp notes.txt copy.txt`, `cp -r src backup`, `mv old new`, `rm -r backup`, `rmdir empty` |
| Create files and directories | `mkdir -p work/src`, `touch a.txt b.txt` |
| Find files and inspect directories | `find work -maxdepth 3 -name '*.txt' -type f`, `tree -L 2 work`, `stat notes.txt`, `du -sb work` |
| Search and read text | `grep -rn word work`, `head -n 5 notes.txt`, `tail -n 5 notes.txt`, `sed -n '10,20p' notes.txt`, `nl notes.txt` |
| Compare and transform | `cmp -s old.txt new.txt`, `diff -u old.txt new.txt`, `sed 's/before/after/g' old.txt > new.txt`, `cut -d , -f 2 table.csv`, `tr a-z A-Z < notes.txt` |
| Combine text tools | `sort names.txt | uniq -c | tee counts.txt`, `wc -lwc counts.txt` |
| Run a command on matching files | `find work -name '*.txt' -type f -print0 | xargs -0 -n 8 wc -l` |
| Prepare and check arguments | `test -f notes.txt`, `printf '%s=%d\n' total 42`, `seq 1 3`, `basename work/a.txt`, `dirname work/a.txt` |
| Inspect the guest | `cpuinfo` samples guest CPU usage, `ps` lists guest tasks, `df` reports committed filesystem bytes, and `sysinfo` reports the guest identity and memory |
| Work in Bash | `history`, `command -v python`, `type cd`, `unset NAME`, `cd -`, `exit 0` |

`cp -n` and `mv -n` preserve an existing destination. Directory copies
require `cp -r` and a new target directory. `grep` searches for literal
text and skips files containing NUL bytes during recursive searches.
Use `grep -rl` for matching filenames and `grep -rc` for per-file counts.
`find -name` supports quoted `*` and `?` patterns. Pair `-print0` with
`xargs -0` for filenames containing spaces. Use `xargs` with commands that
take their inputs as arguments, since its child inherits stdin. `du`
totals file contents in bytes. `diff -u` emits one whole-file hunk per
changed text file. `sed` supports literal substitutions and line ranges.
Use `help` in the guest for the command list.

Agent chat can invoke these commands with `exec` and separate arguments.
For a pipeline or redirection, it can run `bash -c` with the command
line. Commands outside Agent chat's automatic allowlist require approval.
To rebuild a shared core command in the guest, bootstrap the command-line
compiler with `tcc /src/tinycc/bootstrap.c -o ./tcc-new`, then run
`./tcc-new -DAPP=grep /src/core/commands.c -o ./grep`. For `xargs`,
use `tcc /src/core/xargs.c -o ./xargs`.

The stock `cp`, `mv`, `rm`, `mkdir`, `rmdir`, and `touch` are user-space
LXE1 programs. LixinOS compiles them with its own TCC and installs them in
`/bin` when absent or when upgrading an old stock launcher. Rebuild any of
them from its `/src/core` entry point, for example
`tcc /src/core/cp.c -o /bin/cp`. The six entry points share
`/src/core/files.c`. File operations publish a batch in one filesystem
snapshot, including `touch --files-from` and recursive changes.

### Edit a file

At the guest shell, type `nano hello.py` to open an existing file or start a new one. Type your script directly, using Return for new lines and Tab for indentation. Press **Ctrl+O** to write the named file, then **Ctrl+X** to return to the shell and run `python hello.py`. Exiting with unsaved changes offers Save (Y), Discard (N), or Cancel (Esc).

Use the arrow keys to move, Home/End for line boundaries, and Backspace/Delete to remove text. **Ctrl+K** cuts the current line and **Ctrl+U** pastes it at the cursor. The editor scrolls vertically and horizontally and accepts ASCII input. Each saved file uses the persistent disk. Both editors accept documents up to 1 MiB.

Open `kilo main.c` for C/C++ syntax highlighting. **Ctrl+F** starts incremental
search, the arrow keys step through matching lines, Enter accepts a match and
Esc cancels. **Ctrl+S** saves and **Ctrl+Q** quits. Both Kilo and nano run in EL0.
Each editor can replace its opened file atomically. Saving detects edits made
in the other pane. Its non-executable heap grows in private blocks on demand
and is released when the editor exits.

### Memory and monitoring

Type `htop` at the guest prompt for a live dashboard. It refreshes once per second. Press `q` to return to the shell. In the QEMU window the last dashboard frame stays in the terminal history. Uptime comes from the ARM virtual counter. Heap usage comes from the allocator. File counts include empty files, directory counts include the root, and content bytes exclude filesystem metadata.

The launcher assigns one vCPU. With a shared build volume it assigns 12 GiB of RAM to Rust or 24 GiB to TCC. The smaller legacy disks use 640 MiB or 1536 MiB respectively. The boot stack is 128 KiB. Up to 16 tasks, including the login shells, share the CPU. The kernel allocates a 128 KiB stack and private translation tables for each new task, and backs each EL0 image with 4 MiB of heap memory on launch. Code is executable and read-only, data is writable and non-executable. Additional user arenas grow in 2 MiB blocks. A synchronous caller waits for its child while other runnable tasks take turns. Exiting releases image memory, and the next continuation reclaims the stack and page tables. A joinable child's PID and exit status remain until collected. The filesystem tree and file contents occupy heap memory while mounted. The kernel alternates between two checksummed disk snapshots, flushing the payload before publishing its header. On boot it loads the newest valid snapshot. The Rust writer keeps the offset of each unchanged large file in each slot. Warm saves can reuse its aligned sectors. The first saves after boot and changes that move a body may write it in full. The shared disk has two 4 GiB snapshot slots. One slot holds the active filesystem, including a 512-byte header, names, contents, timestamps and version history. Each file can occupy up to 512 MiB. Legacy disks have 192 MiB slots and a 100 MiB file limit. A separate 65 KiB ring at the end keeps the latest 64 shell commands. On first launch with an older disk, the launcher copies valid snapshots and history to a temporary image and atomically replaces the old image after verification. Use `touch --files-from names.txt` to create a batch of files with one path per line and one snapshot commit.

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

Normal guest Python provides `os` (also `uos`), `sys`, `random`,
`time`, and `io`. Use `open()` or `io.open()` for sequential text or
binary file I/O in read, write, or append mode. Paths resolve from the
process working directory. `os` includes `getcwd`, `chdir`, `listdir`,
`stat`, `mkdir`, `remove`, `rmdir`, `uname`, and `urandom`.
The guest RTC provides wall time at one-second resolution.

`python -c 'print(42)'` runs inline code. `os.system(command)` runs one
LixinOS Bash command in the guest and returns its exit code. For example,
`os.system("echo hello | cat > hello.txt")` writes to the guest disk.
The Agent's isolated test runner blocks filesystem and command calls.

Cargo builds both C images before embedding them. The first build needs Git, Make, Python 3 and network access to fetch the pinned MicroPython source and, on Apple Silicon, Arm's checksummed bare-metal GNU toolchain. Sources, toolchain and build products are cached under `Kernel/target/user-build/`. An existing `aarch64-none-elf-gcc` with newlib on PATH can also be used. Run `python3 scripts/test-user.py` to exercise the serial interface on a temporary disk.

### Compile and run Rust inside LixinOS

With the native Rust toolchain installed, either kernel can build a Cargo project from its guest shell:

```sh
cargo new --vcs none demo
cd demo
cargo run
```

The project prints `Hello, world!`. The upstream Rust 1.100.0 compiler uses Cranelift and the guest GNU linker. For single-file programs use `rustc hello.rs -o hello` and `./hello`. See the [Rust toolchain port](User/rust/README.md) for installation, supported dependencies and executable size limits. On the dedicated Rust-kernel build disk the seed toolchain also compiles `rustc` and Cargo from their locked sources. The resulting tools are available as `/stage1/rustc` and `/stage1/cargo`. The [guest bootstrap instructions](User/rust/README.md#native-compiler-and-cargo-build) describe the build and its seeded libraries.

The user-space `/bin/lust` bundles the Lust Rust-subset frontend and the ARM64 TinyCC backend. Run these commands at the guest shell:

```sh
lust build /src/lust/hello.rs -o hello
./hello
```

The example prints `Hello from LixinOS Rust: 42`. For your own program, use `nano demo.rs`, save it, then run `lust build demo.rs -o demo` and `./demo`. Supported language features, executable limits and host package details are in [the port notes](User/lust/README.md).

The guest-native system rebuild and its remaining host dependencies are
described in [the system build guide](User/build/README.md). Run
`python3 scripts/test-native-build.py --save-disk /absolute/path/native.img`
to build in a new guest disk and check its programs across reboot. To boot
a matching, already built kernel, set `LIXINOS_KERNEL` to its absolute path
when invoking the launcher.

### Bootstrap TinyCC inside LixinOS

At the guest prompt, build two generations of TinyCC and try the shell built
by the second generation:

```sh
tcc /src/tinycc/bootstrap.c -o tcc-new
./tcc-new /src/tinycc/bootstrap.c -o tcc-second
./tcc-second /src/bash/bash.c -o bash-new
./bash-new --version
```

The [guest build notes](User/tinycc/README.md) describe the native source
tree and multi-file builds. Run `python3 scripts/test-tcc-bootstrap.py` for
the clean-disk bootstrap and reboot test.

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

Programs saved by TinyCC and Lust use LXE1 and can fill a 1 MiB file including
the 16-byte header. The ARM64 layout allows up to 512 KiB of code and 860 KiB
of data, including zero-initialized storage, within that total. Rebuild
programs saved in the experimental LXE2 format from their source.
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

### Build a graphical app in LixinOS

From the main shell in a QEMU window, compile both examples in the guest.

```sh
tcc /src/gui/calculator.c -o /bin/calculator
tcc /src/gui/text-editor.c -o /bin/text-editor
desktop
```

Click **Calc** for the built-in calculator, or **Editor** for the compiled text editor. Run `calculator` to open the standalone build. You can also start a
saved LXE1 app with an `.app` filename by clicking it in the desktop file browser. Apps use
the foreground pane and return to the desktop on close. Saved programs and
documents persist when you restart with the same disk.

Calculator accepts mouse and keyboard input. Text Editor opens
`/notes.txt` from the desktop or a path you pass with `text-editor path`
in the main shell. Use **Save** or **Ctrl+S** to save and **Esc** to close.
It edits up to 32 KiB of ASCII text with LF line endings and confirms before
discarding unsaved edits.

The [Slint package](User/slint-demo/README.md) provides `slint-demo` and
`slint-editor FILE`. The applications use the resizable canvas and update
changed regions. Run them from a QEMU graphics console.

The [Nano-X package](User/nano-x/README.md) provides a separate desktop with a
file browser. Run `nano-x` to open it or `desktop` to open the LixinOS desktop.

Use `<lixin-gui.h>` for canvas drawing, text, buttons and pointer input.
Use `<lixin.h>` for `lx_save_file` and other file operations. Run
`python3 scripts/test-desktop-apps.py` on the Mac to exercise both apps across
two boots on a disposable disk.

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

The Mac bridge sends the task, test cases, and any repair diagnostics to DeepSeek over HTTPS. The API key is read on the Mac. The default model is `deepseek-flash`, which you can override with `DEEPSEEK_MODEL`. The script-generation workflow uses isolated, deterministic MicroPython tests with built-ins such as `input()` and `print()`.

Run `PYTHONDONTWRITEBYTECODE=1 python3 scripts/test-agent.py` for the QEMU integration test with deterministic DeepSeek responses and a temporary guest disk.

### Inspect LixinOS with the Agent

The boot banner and `uname -r` show the running kernel release, including its
`-rust` or `-tcc` suffix. `uname -a` and `cat /proc/version` also show the
system name and architecture. `sysinfo`
prints identity, memory, and disk information from the running guest.
Agent inspection can query the same OS identity with its `system` tool.
The Agent's platform guide is also available at `/src/agent/README.md`.

With the model bridge enabled, ask a question at the LixinOS prompt.

```sh
agent inspect 'Report the guest CPU, RAM, disk, and filesystem usage.'
agent inspect 'List the files in the current directory and report their sizes.'
```

The Agent chooses up to six read-only queries. CPU count and RAM capacity come from the QEMU boot device tree. Heap usage comes from the allocator, and disk capacity and snapshot generation come from the mounted VirtIO disk. The filesystem queries report counts, limits, filenames, types and sizes. Relative paths resolve from the current directory. The CPU query samples the guest over a 250 ms interval and excludes scheduler idle waits. The `cpuinfo` shell command reports the same measurements in serial and graphical sessions. Total free physical RAM is unavailable.

The Mac bridge forwards the question and requested observations to DeepSeek. Inspection sends file metadata when requested, with file contents excluded. It leaves the guest disk and command history unchanged.

### Read system data from /proc

LixinOS exposes a read-only subset of `/proc` through its file API. Use
`ls /proc`, `cat /proc/stat`, or MicroPython's `open('/proc/meminfo')`.
Every open captures a fresh snapshot. Generated files report size zero and
read normally through EOF. The volume's filesystem counts cover persistent
entries. If a disk already contains a real `/proc` entry, LixinOS preserves
it and uses that entry.

| File | Data |
| --- | --- |
| `/proc/stat` | `cpu` and `cpu0` counters in hundredths of a second. The system column aggregates busy time and the idle column counts scheduler waits. The other columns are zero. |
| `/proc/cpuinfo` | Scheduler CPU 0 and LixinOS architecture. |
| `/proc/meminfo` | Guest RAM and free allocator memory in KiB, printed as `kB` for parsers. |
| `/proc/uptime` | Seconds since the scheduler started and cumulative idle seconds. |
| `/proc/version` | LixinOS release and architecture. |

Use `cpuinfo` for a sampled utilization percentage and `ps` for process
details. The CPU counter layout follows the
[Linux kernel proc documentation](https://docs.kernel.org/filesystems/proc.html).

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

### Chat with the Agent

Start a foreground session with `agent chat /work` or run `agent chat`
from inside a project directory. Enter a task at `agent>`. The session
keeps context from the last eight turns until you leave with `/exit` or
**Ctrl+D**. Type `/prompt` to display the system message sent to the model.
Use the Up and Down arrow keys to recall recent prompts and slash commands
from this session. Down past the newest entry returns to your draft.
The header shows the workspace, bypass mode, and tool budget. Type `/help`
to see controls. A temporary indicator appears while a model reply is pending.

Type `/export` at `agent>` to save the current conversation in the guest
project directory as `agent-conversation.txt`. Later exports use numbered
filenames. Use `/export my-chat.txt` to choose a new filename. The transcript
includes prompts, replies, tool results available to the Agent, and whether
staged changes were saved. Older turns are included up to the 1 MiB file
limit. You can continue chatting after exporting.

The Agent can run installed guest programs such as `git`, `tcc`, and
your own LXE1 executables. Bare names resolve in `/bin`, and a path such
as `./demo` resolves from the project directory. The terminal shows the
step number, working directory and command arguments on one line, followed
by the first three lines of live output and any nonzero exit status.
Command lines appear in cyan and Agent replies in lilac. Approval prompts
are amber, and verified deliveries show separate checks and launch fields.
Programs use the normal terminal color and can style their own output.
Transcripts include all command exit statuses. Source reads, multiline
arguments and staged changes also show up to three lines by default.
At a confirmation prompt, use `v` to
expand the command or review preview. The model still receives its usual
bounded tool results.
For pipelines and redirection, the Agent can call guest `bash -c` with
one command line. For small one-off programs, it can call guest
`python -c` with the source as an argument.

Foreground chat starts with bypass on and announces it before the first
prompt. Noninteractive commands use normal guest filesystem permissions
and can write immediately. Type `/bypass` to toggle auto-approval for the
current session. With bypass off, recognized read-only calls to stock
commands can run directly. Other commands ask for approval. Project script
tools stage changes, test them and ask before saving.
For an interactive program such as an editor or an SSH session, the Agent
asks before handing you the terminal. The chat resumes with the program's
exit status. If an identical command already failed in the same turn, the
Agent gets its previous error until the workspace changes or you send a
follow-up.

Tool results carry a status and an exit code when available. For example,
`requires_terminal` asks the Agent to hand the terminal to the operator,
and `staged` means a file is awaiting review. The `apply` tool asks for
approval, commits passing staged scripts, and returns control to the Agent.

For delivery, the Agent can run an exact-output `test` or a `smoke` check
with scripted input and an expected output marker on a saved Python file.
The `verify` tool reads the file back from the mounted disk. A final receipt
shows the path, disk snapshot, check totals, and launch command when supplied.
Interactive checks are marked pending until a scripted check passes.
The transcript also records auto-approvals and model bridge errors.

**Ctrl+P** adds a follow-up while waiting for the model or a staged test.
**Ctrl+C** or **Ctrl+G** cancels a turn or interrupts a noninteractive
command. Noninteractive user code has a 60-second deadline. After terminal
handoff, use the application's own exit controls. The Mac bridge still
relays model requests.

### Local Git repositories

Run `cat /src/git/README.md` for the guest Git workflow and supported
commands. Inspection and project sessions, including `agent system`, load
the guest copy when they start and provide it to the model as reference data.

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
`LixinOS <user@lixinos.local>`. The Git port transfers working files, index data and objects in 16 KiB chunks. File size is bounded by the guest volume and available memory. Each command saves
its metadata in one snapshot.

From the project root on the Mac, export into a **new directory**:

```sh
python3 scripts/export-git.py /project ~/Desktop/lixinos-project
git -C ~/Desktop/lixinos-project status
git -C ~/Desktop/lixinos-project log --oneline
```

The exporter copies the worktree and its standard `.git` directory, including
the current index and untracked files. It reads the newest valid snapshot from the shared daily disk (or `data/lixinos.img` before migration); use `--disk /path/to/image` for a different disk.
Run `python3 scripts/test-git.py` for the disposable-disk interoperability test.
The build fetches pinned libgit2 sources, including its COPYING file, into the
user build cache.

## 内核版本归档

每次内核迭代使用 `python3 scripts/kernel-release.py publish` 生成带日期的 Git 标签，
同时保留 Rust 与 TCC 的源码、内核二进制和引导文件。用 `list` 查看版本，
用 `restore TAG 新目录` 恢复可启动的旧版。[发布和回滚说明](docs/kernel-releases.md)
