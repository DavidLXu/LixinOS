# Desktop window applications

Run `desktop` in a graphical shell. **Apps** in the dock (or Ctrl+G) opens
a full-screen application grid. Choose **终端** for installed CLI tools such as Codex or Python. Clicking one opens a terminal tab, runs the displayed command and leaves Bash available when it finishes. Graphical games and editors open directly. Scroll with the wheel or drag vertically, filter
by category at the top, and click an icon to open it. Click **完成** or press Esc to return.
See the [应用清单](../../docs/applications.md) for launcher names and shell commands. In the
**窗口** tab, click a row to raise the window or restore it from the dock.
Ctrl+] cycles through the visible windows. The number identifies the external client slot. The Eyes dock shortcut
opens Eyes or brings its most recent instance forward.

Launcher and dock icons come from `/lib/netsurf/launcher-icons.bin`, a
pre-rendered bank of 76x76 and 40x40 RGBA images. Regenerate it from the
artwork in `User/desktop/icons/` with:

```sh
python3 User/desktop/launcher-icons.py /path/to/launcher-icons.bin
```

The desktop falls back to its 16x16 vector-style glyphs if the file is absent.
See `User/desktop/icons/README.md` for artwork sources and licences.

## 系统设置

在 Desktop 打开 **Apps → 系统 → 系统设置**。常规窗口默认显示「启动与连接」，点左侧分类可切换面板。窄窗口通过左上角返回分类列表。点一行进入编辑，输入新值或点「切换选项」，然后点「保存」。左上角返回会放弃本次输入。滚轮或拖动滚动条可以浏览长列表。上下键或 J/K 选择，回车打开。在编辑页面用 Tab 切换焦点、空格激活按钮、Ctrl+B 返回。

| 页面 | 可用项目 | 生效时间 |
| --- | --- | --- |
| 登录与终端 | 默认编辑器、主目录、起始目录、PATH、临时目录、语言环境，以及自定义登录项 | 新终端或 `shell new` |
| 开发环境 | 编译器、Cargo、Python、pip 和 TLS 证书 | 新终端中启动的程序 |
| Agent 设置 | 每次项目任务的工具次数（1–60）、单次结果预览字节数（64–2048） | 下次 Agent 任务 |
| 启动与连接 | 状态栏时区、单窗格或双窗格、共享盘启动菜单的首选内核、SSH 模式的本机端口、模型名称 | 下次通过 Mac 启动器开机 |
| 系统信息 | 查看内核、架构、内存、处理器、存储和网络，以及 Mac 上的磁盘、备份和密钥入口 | 打开相应项目时读取 |

登录项保存在 `/home/lixin/.profile`。常规变量可直接改值，带空格的值沿用 Shell 引号。复合语句可整行编辑，其他行及原有注释保留。Agent 使用 `/agent-policy/settings.py`。启动偏好随磁盘保存在 `/.desktop-boot`，命令行选项和 Mac 环境变量优先。时区可以从常用选项中选择，也可以输入城市时区名。SSH 端口填 0 由系统分配，模型填 `auto` 使用桥接默认值。

在 Mac 上构建时需要 `rsvg-convert` 和 Python Pillow，用来将图标与插画嵌入应用。为另一块已关机的桌面盘安装：

```sh
bash User/desktop/build-settings.sh
python3 User/gcc/install.py --toolchain settings Kernel/target/user-build/settings-package /path/to/disk.img
```

## Files

点文件窗口底部的「图标」切换到图标视图，PNG 和 JPEG 会显示缩略图。点击排序栏切换顺序。点「列表」恢复详细列表，选中的文件会保留。

Open **Files** from the dock. Click a row to select it, then double-click or
press Enter to open it. Up and Down select adjacent entries. Scroll the wheel one row at a time
or drag the scrollbar to reach a distant entry. Left and Right revisit previously
opened folders. Click a column heading to sort by name, type, byte size or
modification time. Click again to reverse the order. Dates use UTC, as in
`ls -l`. A dash marks an unavailable timestamp or a directory size. Wide windows show a places
sidebar. **操作** exposes all file actions, including navigation on narrow windows.

单击路径栏中的文件夹名可回到那一级。侧栏的「+ 收藏」或 Ctrl+D 收藏当前目录，再用一次即可移除。收藏会保存在日常盘，编辑器的打开和另存为对话框也能使用。窄窗口可点标题栏的文件夹图标展开位置列表。侧栏可用滚轮浏览，最多保存 24 个目录。前进和后退会恢复该次访问的筛选、排序、所选文件和滚动位置。

Use Ctrl+L for a path, Ctrl+F to filter names in the current directory,
Ctrl+N to create a folder and Ctrl+R to rename the selection. Type over the
highlighted name or path, then press Enter. Esc clears an active filter.
Shift-click selects a range, Ctrl-click toggles an item, and Ctrl+A selects the
filtered list. The footer shows the selection count and the total byte size of
selected regular files. Copy, cut, paste and trash apply to the entire selection.
A conflicting paste leaves the destination unchanged. Right-click a row to open
the same commands available under **操作**.

**恢复到原位置** in `/Trash` restores items after a restart. If a destination is
occupied, enter a different filename or cancel. Items trashed by older versions
can still be cut and pasted elsewhere. Ctrl+Z reverses the most recent rename
or move, including a batch move, in the current Files session.

「用文本编辑器打开」和「用 Nano 打开」作用于当前文件。「始终用…」会记住该扩展名的选择，重启后仍然生效。图形编辑器可以输入、粘贴、编辑和保存 UTF-8 中文正文。Ctrl+O 打开，Ctrl+S 保存，Ctrl+T 另存为。文档编辑上限为 32 KiB。

## 可复用的桌面控件

在系统内运行 `tcc /src/desktop/widgets-demo.c -o /bin/widgets-demo.win3`，然后从 **Apps → 开发 → 控件示例** 打开它。输入中文，按 Tab 移到「确定」，按空格或回车显示输入的文字。`window-widgets.h` 提供 RGB565 按钮、中文标签和单行输入框。`widgets.h` 提供独立于画布的焦点、按钮按下与释放、UTF-8 选择与编辑，以及列表滚动和滚动条计算。`file-dialog.h` 可供前台画布应用复用，调用方在用户确认路径后负责读写文件。

## Build a window app inside LixinOS

    tcc /src/desktop/counter.c -o /bin/counter.win
    desktop

Select **Apps → 开发 → 计数器示例** twice. Click inside either counter or
type +, - or 0 in its focused window. Each instance has its own counter.
The desktop scans /bin when Applications opens. It lists nxeyes and files
ending in .win, .win2 or .win3. Files can also launch them from another directory.

## Fixed surfaces (.win)

Use window-client.h for WindowEvent and WindowFrame. Read one complete event
from stdin, then write one complete 128 × 72 RGB565 frame to stdout. A read or
write may be short. Stop at EOF or a pipe error. Send diagnostic text to stderr.
The first event requests the initial frame. Pointer coordinates are local to the
surface. The desktop captures a button drag until release and sends keyboard
bytes to the focused client. Esc closes the focused client. The compositor
keeps the last complete frame and scales it as the window is resized.

Four external clients can run alongside Files, Browser and Calculator. Terminal
sessions and the foreground editor use their existing popup mechanism. Fixed surfaces redraw in response to input.

## Adaptive surfaces (.win2)

Open **Apps → 工具 → 秒表**. Click the play/pause button or press Space to
start and stop it. R resets it. Resize the window or maximize it to see the
digits change size while the button keeps its size at the right edge.
To build your own copy in the guest, run
`tcc /src/desktop/stopwatch.c -o /bin/my-watch.win2`.

The filename extension selects the wire protocol. Both protocols use two pipes
and allow one outstanding event per window. For .win2, read one WindowEvent
from stdin and reply on stdout with one WindowUpdate header (five little-endian
16-bit fields) followed by w × h tightly packed RGB565 pixels in row order.

| Event | Client action |
| --- | --- |
| CONFIGURE | x and y hold the new content width and height in logical pixels. Return the entire new surface, starting at (0, 0). This is the first event and is repeated on size changes. |
| POINTER | x and y are content coordinates at the most recently delivered size. value is the primary button state. |
| KEY | value holds the focused window's keyboard byte. |
| TIMER | Advance the animation using the monotonic clock and return the changed region. |

On ordinary events, x, y, w and h in the reply specify the changed rectangle.
Four zeros acknowledge the event with no pixel transfer. Set timer_ms in each
reply to request periodic events (minimum 16 ms), or use zero to disable them.
Timers run while the window is shown. Delivery may be delayed by rendering or
scheduling. A restored stopwatch uses the elapsed time to catch up.

The desktop retains each completed surface. While a resized frame is in transit
it scales the last complete frame. It applies a patch only when all its pixels
have arrived. Small unobstructed updates go directly to the affected screen
rows. Where windows or desktop controls overlap, normal compositing preserves
their stacking order.

## Editor windows (.win3)

[Lite XL](../lite-xl/README.md) uses the extended window protocol. It keeps
the eight-byte WindowEvent input record and adds WHEEL, CLOSE and PASTE. WHEEL
carries the signed vertical wheel steps in value and the pointer's local
coordinates in x and y. CLOSE lets the application prompt to save and
exit when ready. Esc and Ctrl+G go to the focused .win3 app, and the
desktop Apps menu remains available from the dock.
If a client has not acknowledged a close request after two seconds, click its
close button again to show a warning. One more click forces that window to
exit and discards unsaved changes in it.
For PASTE, value is the length of the UTF-8 text that follows the event.
The desktop sends one event for the entire clipboard, up to 4096 bytes.

A .win3 reply starts with a sixteen-byte WindowReply. When encoded_size
is zero the rectangle follows as packed RGB565. Otherwise the rectangle
is encoded_size bytes of little-endian 16-bit pairs, with a positive run
length followed by its RGB565 pixel. Runs are in row order and must cover
the rectangle exactly. The desktop accepts runs only when they are smaller
than the raw rectangle. The final clipboard_size bytes are UTF-8 text.
For a nonzero size up to 4096, the desktop publishes the text after
receiving the complete reply.

Viewer clients negotiate additional features through CONFIGURE.value. When
`WINDOW_CLIENT_VIEWER_AVAILABLE` is set, the high bits of encoded_size can
request modifier input, toggle fullscreen, or mark the trailing bytes as a
window title (up to 199 bytes). With modifier input, KEY.x carries Shift,
Ctrl and Alt in bits 0–2. POINTER.value carries left in bit 0, modifiers in
bits 1–3, and right and middle in bits 4–5 on a host advertising
`WINDOW_CLIENT_RAW_BUTTONS_AVAILABLE`. A client opting into file drops on
a host advertising `WINDOW_CLIENT_FILE_DROP_AVAILABLE` receives the guest
path as a length-prefixed payload following a FILE_DROP event.

The NetSurf desktop advertises `WINDOW_CLIENT_OPEN_URL_AVAILABLE`. A client
can set `WINDOW_CLIENT_OPEN_URL` on a reply and send an HTTP or HTTPS URL
in its trailing payload (up to 4095 bytes). Desktop opens the address in
its browser and raises the browser window.

## Task manager and calendar

Open **Apps → 系统 → 任务管理器** to view the same live PID, parent PID, state and
foreground/background information as `ps`. The list refreshes every second.
Use J/K (lowercase) or the two header arrows to scroll. Click PID, CPU or 内存 to sort, and click again to reverse the order. P/C/M (lowercase) select the same columns. CPU defaults to busiest first.

Open **Apps → 工具 → 日历** for the Gregorian month containing the guest RTC date
(UTC). The highlighted cell marks today. Click the header arrows or press
`[` and `]` to browse months. Click the month title or press T (lowercase)
to return to today.

Rebuild either application at the guest shell:

```sh
tcc -I/src/tinycc/sdk /src/desktop/tasks.c -o /bin/tasks.win2
tcc -I/src/tinycc/sdk /src/desktop/calendar.c -o /bin/calendar.win2
```

## Desktop app workbench

From a LixinOS terminal with the model bridge connected:

```sh
agent desktop new /work/my-app
agent desktop /work/my-app
```

Describe the app and give concrete examples, such as “Build a focus timer.
Space pauses it, R resets it, and resizing preserves the remaining time.”
The Agent edits `app.c` and `tests.json`, runs the guest TCC, and tests the
result with scripted resize, pointer, keyboard and timer events. A failing
step reports the case, event and mismatched pixel or timer value.

On either kernel, the desktop panel and `agent desktop` run the same
`/src/agent/native.py` worker. It journals the user task, tool intent and
tool result in the project's `.agent-trace-*.jsonl` files. Use
`agent resume /work/my-app` or **Continue** after a restart. An interrupted
write is reconciled against the file on disk before the model picks its next
action. The worker sends a bounded projection of recent tool results and
conversation to the model. The full records stay available in the project.

Use `/status` to check the candidate and `/publish` to install its verified
`/bin/my-app.win2`. Open it from **Desktop → Apps → 其他**. Come back to the same
project to ask for an improvement. Each release carries forward its existing
regression tests. Use `/rollback` to switch to the previous installed
binary. Your draft remains available for further editing.

The project contains the build inputs, receipts and release history under
`.desktop`. Keep the directory as long as you use the installed app. You
can also run `python3 -I /src/agent/desktop.py check` from the project to
reproduce the tests without a model. This starter targets one C window
client with the included drawing helpers. The tests exercise its window
protocol and sampled pixels. Inspect full-size layout in Desktop.

The resource view in tasks.win2 requests a sample once a second while visible.
CPU is guest busy time over the interval between samples. Proc RAM is the sum
of private program image and arena allocations, in MiB. Its graph uses guest
RAM capacity as 100%; shared kernel allocations and kernel stacks are excluded.
Each graph retains 60 samples. A new PID shows -- until its next CPU sample.
Widen the window to see parent PID, state and foreground/background mode.

## Online map

Open **Apps → 工具 → 地图**. Drag the map in any direction. Click the two zoom
buttons or press + / - to change levels (0–19). The initial view is Shanghai.
The guest fetches visible tiles over TCP. The QEMU launcher forwards each
request to scripts/map-tile.py on the Mac, which uses curl for HTTPS and
Python Pillow for decoding. Restart the VM with the current launcher after
installing the app. The bridge caches tiles under ~/Library/Caches/LixinOS/osm
for seven days and can reuse a cached tile when the provider is unreachable.
Map data © OpenStreetMap contributors, https://www.openstreetmap.org/copyright.

Guest build: `tcc -I/src/tinycc/sdk /src/desktop/map.c -o /bin/map.win2`.

## 桌面壁纸

右键桌面空白处，选择 **指定图片 PNG / JPG**，输入客机里的图片路径（如 `/home/lixin/Pictures/海岸.jpg`），回车或点击「确定」。绝对路径可以包含空格，直接输入即可。图片按比例铺满桌面，超出部分居中裁剪。菜单中也可随时切换回「暮色」「青山」「海岸」。

Desktop 将成功选中的路径保存在 `/home/lixin/.wallpaper`，下次启动时重新加载原图。请把原图留在这个位置。单图上限为 32 MiB、16,777,216 像素。选中损坏或不存在的文件时可重新输入路径，或按 Esc 取消。

Desktop 和 Links 共用 [stb_image](../gui/stb_image.h)，版本固定在上游提交 `f58f558c120e9b32c217290b80bad1a0729fbb2c`。日常盘使用带 NetSurf 的 Desktop。在 Mac 上为另一块已关机的盘构建并安装这个版本：

```sh
bash User/netsurf/build.sh
python3 User/gcc/install.py --toolchain netsurf Kernel/target/user-build/netsurf-port/package /path/to/disk.img
```

## Agent window

Open **Apps → 开发 → Agent**. LixinOS compiles `/bin/agent.win3` with the guest TCC
the first time you open it. Enter the directory of an existing
`agent desktop` project, then send a task. The session list and timeline
read the same persisted traces as the terminal. Click a saved session to
inspect it, or use **Continue** to resume unfinished work. While it runs,
**Stop** cancels the worker. A message sent during a run is queued for the
next turn.

**Review** shows the source diff and the candidate's verified tests.
**Preview** runs the installed app and the verified candidate side by side.
**Publish** and **Rollback** take a second click and apply to the displayed
version. The task worker uses the desktop workbench's workspace tools.
Turns saved with command approval enabled can be resumed from the terminal.
