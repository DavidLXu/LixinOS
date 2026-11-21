# LixinOS 系统手册

LixinOS 是一个提供 Rust 原版内核和 TCC 自举内核的 ARM64 操作系统，在 Apple Silicon Mac 的 QEMU 虚拟机里运行。你可以在系统里编辑 Python 和 C 源码、运行脚本、编译程序，再把程序保存在虚拟磁盘上。当前版本为 [0.4.0](0.4.0.md)。

## 从终端开始

在 Mac 的项目根目录运行 `./scripts/run-kernel.sh`，在弹出的中文窗口中选择 **Rust 原版内核**（默认）或 **TCC 自举内核**，点击 **启动**。看到 `lixin:/$` 就可以输入命令。安装共享盘后，两种内核都使用 `data/shared.img`。切换前在 Bash 输入 `q` 并回车关机。首次构建会下载交叉编译工具和上游源码。想直接在当前 Mac 终端操作，可以用 `./scripts/run-kernel.sh --serial`，输入 `1` 或 `2` 后回车选内核。要跳过菜单，可用 `./scripts/run-kernel.sh --kernel rust` 或 `./scripts/run-kernel.sh --kernel tcc`，两者均可搭配 `--split` 等启动模式。安装步骤和其他启动模式见[项目 README](../README.md)。

| 想做什么 | 在 LixinOS 里怎么做 |
|---|---|
| 写一个 Python 脚本 | 用 `nano hello.py` 输入 `print(6 * 7)`，按 Ctrl+O 保存、Ctrl+X 退出，再执行 `python hello.py` |
| 临时试一段 Python | 输入 `python` 进入 MicroPython，在空白的 `>>>` 提示符按 Ctrl+D 回到 shell |
| 编译并保留 C 程序 | 用 `nano hello.c` 编写源码，执行 `tcc hello.c -o hello`，再用 `./hello` 运行；只想试跑一次可用 `tcc -run hello.c` |
| 查看文件或系统状态 | `ls -lah` 显示隐藏文件、类型和大小；`htop` 显示内核堆和文件系统统计，按 q 返回 |
| 找回上一次的命令 | 按 ↑ 或 ↓ 浏览最近 64 条历史；Tab 补全命令或路径，连续按两次可列出候选 |
| 关机并保留工作 | 输入 `shutdown`。再次启动时使用同一块磁盘，文件、编译好的程序和命令历史就会恢复 |

## 查找应用

在图形终端输入 `desktop`，点左侧 **Apps** 按用途查找程序。「窗口」列出已经打开的窗口。桌面、Dock 和 Bash 的启动入口见[应用清单](applications.md)。

## 在图形桌面浏览网页

安装 NetSurf 后，在桌面点击 **Browser**，或在 Bash 运行 `netsurf https://www.apple.com.cn/`。苹果首页的产品图、链接样式和导航排版有对应的实机截图与双内核复测命令。[NetSurf 网页渲染修复记录](netsurf-apple-homepage.md)

## 在桌面玩游戏

在日常工作盘的 Bash 中输入 `desktop`，打开左侧 **Apps**。选 **游戏 → 2048** 后，用方向键、WASD 或鼠标拖动来合并方块，按 **R** 重新开局。选 **游戏 → 数字拼图** 则进入 GNOME 数字拼图：把右侧的方块拖进左侧棋盘，让相邻边上的数字相同。[2048 的源码与系统内重编译方法](../User/games/2048/README.md)

## 在两种内核上运行 CPython

在 Bash 输入 `python3` 即可进入 CPython 3.14.5，按 Ctrl+D 退出。输入 `python3 hello.py` 运行磁盘上的脚本，或用 `python3 -c "import sys; print(sys.implementation.name, sys.platform)"` 查看实现和平台，结果是 `cpython lixinos`。`python` 仍可运行 MicroPython。[支持范围与构建方法](../User/cpython/README.md)

## 用 Kilo 写 C 代码

在 shell 输入 `kilo main.c` 即可新建或打开文件。C/C++ 的关键字、字符串、数字和注释会显示成不同颜色。按 **Ctrl+F** 输入搜索词，上下方向键切换匹配行，回车确认，Esc 取消。**Ctrl+S** 保存，**Ctrl+Q** 退出。分屏时两个编辑器可以同时打开同一个文件，一侧保存后，另一侧再次保存会提示冲突。

Kilo、nano、htop，以及 shell 的输入、历史浏览、补全和命令解析都在用户态（EL0）运行。`cat`、`echo`、`grep`、`wc`、`pwd` 和 `clear` 是磁盘上的用户程序。内核负责校验并启动程序、维护窗格状态、隔离内存以及提交磁盘更改。编辑器只能保存启动时指定的文件。

## 用鼠标画矢量图

用 `./scripts/run-kernel.sh --window` 或 `--split` 启动，在 LixinOS 中输入 `paint sketch.svg`。省略文件名时使用当前目录的 `drawing.svg`。工具栏第一行选工具，第二行选颜色，滚轮或 **+ / -** 调线宽。鼠标左键按下落笔，松开抬笔；再次按下会开始独立的一笔。

| 工具 | 操作 |
|---|---|
| Pen、Line、Arrow | 拖出自由笔迹、直线或箭头。箭头头部随线宽一起变化。 |
| Curve | 先拖出两端，松开后移动鼠标预览弯曲程度，单击确定控制点。Esc 取消。 |
| Rect、Oval | 拖出外接矩形的两个对角，得到矩形或椭圆。 |

点 **Save**（Ctrl+S）保存，**Undo**（Ctrl+Z）撤销最近的图形，**Quit**（Q）退出。有未保存改动时再次按 Q 才会丢弃。保存的 SVG 可用 Mac 浏览器打开，也能在 LixinOS 中再次运行同一条 `paint` 命令继续画。重新编辑支持画板自身保存的 SVG 格式。在分屏模式中，先用 Ctrl+Tab 选择要操作的窗格。

## 在系统里用 Git，再导出到 Mac

进入项目目录后运行 `git init`。用 `git status` 看哪些文件有变化，`git diff` 看尚未暂存的行，`git add .` 暂存当前目录的改动。用 `git diff --staged` 核对本次提交的内容，执行 `git commit -m "first version"`，再用 `git log` 查历史。新仓库使用 `main` 分支。Git 的索引、对象和目录也占用文件系统的配额。

假设仓库在 LixinOS 的 `/project`，回到 Mac 的 LixinOS 项目根目录，执行下面两条命令。导出目标应是一个新目录，里面会保留提交、暂存区、工作文件和未跟踪文件。

```sh
python3 scripts/export-git.py /project ~/Desktop/lixinos-project
git -C ~/Desktop/lixinos-project status
```

## Shell 放在哪里？

默认登录 Shell 是 C 编写的 LixinOS Bash，安装在 `/bin/bash`。`sh` 和 `bash` 都能启动它，`bash --version` 可查看名称。用 `nano /src/bash/bash.c` 编辑源码，执行 `tcc /src/bash/bash.c -o /bin/bash` 安装新版。下次打开 Shell 或重新登录即可使用。

## 输入一条命令后，谁在工作？

以 `python hello.py` 为例，shell 先按当前目录找到脚本，把文件内容交给用户态的 MicroPython。解释器在 ARM64 的 EL0 执行 Python 代码，调用内核的终端接口输出结果。脚本结束后，控制权回到 shell。内核本身运行在 EL1，负责页表、异常处理和设备访问。[用户态 shell](../User/bash/bash.c) · [用户程序加载器](../Kernel/src/user.rs) · [MicroPython 移植层](../User/python/port.c)

启动器分配一个虚拟 CPU，使用共享盘时为 Rust 内核分配 12 GiB RAM，为 TCC 内核分配 24 GiB RAM。内核使用用户态区域上方的 RAM 作为堆，每个终端窗格为前台用户程序保留 4 MiB 区域，代码页与可写数据页使用不同权限。用 `--split` 启动时，两个窗格共享磁盘，各自保存工作目录、变量和前台程序状态；Ctrl+Tab 暂停当前窗格并恢复另一个。读 `htop` 时应把堆用量看作整个 guest 内存的一部分。[内核入口](../Kernel/src/main.rs) · [分屏实现](../Kernel/src/panes.rs)

## 重启后，哪些东西还在？

共享日常盘位于 `data/shared.img`。可以在 `/scratch` 下编译，再把产物复制到 `/artifacts`，换内核后仍可直接运行。正常关机会刷入 `/scratch` 的待保存数据。输入 `df` 查看剩余空间。镜像包含两个交替写入的 4 GiB 快照槽和 65 KiB 命令历史区，因此 Mac 上显示的逻辑大小约为 8 GiB。当前文件系统可使用一份快照的容量，扣除 512 字节头部后由文件内容和元数据共用，单个文件上限为 512 MiB。迁移保留原来的 `data/lixinos.img` 和 `data/tcc-os/system.img`，步骤见[项目 README](../README.md#shell-and-disk)。Python 交互变量和正在编辑但尚未保存的文字只存在于当前会话。[磁盘格式与恢复逻辑](../Kernel/src/disk.rs)

## 编译好的 C 程序是什么格式？

在 LixinOS 内运行 `tcc hello.c -o hello` 会生成 ARM64 **LXE1**。文件以 16 字节头部开头，依次是四字节 `LXE1` 标记以及三个小端 32 位字段，记录入口偏移、代码长度和数据长度。头部之后存放代码和完整的数据段。此前生成的 LXE2 程序需要从源码重新编译。

在 QEMU 主图形终端里用以下命令构建两个示例，再到桌面点击相应图标。

```sh
tcc /src/gui/calculator.c -o /bin/calculator
tcc /src/gui/text-editor.c -o /bin/text-editor
desktop
```

桌面文件浏览器也可以打开扩展名为 `.app` 的 LXE1 程序。应用在当前窗格前台运行，关闭后回到桌面。编辑器默认打开 `/notes.txt`，支持 32 KiB 以内的 ASCII 文本。应用和保存的文件会留在磁盘上。[图形头文件](../User/gui/lixin-gui.h) · [编译器](../User/tcc/port.c) · [加载器](../Kernel/src/user.rs)

可选的 `agent` 命令借助 Mac 端桥接调用模型服务，生成脚本后在 guest 中验证。编辑、编译和运行本地程序可以独立使用。桥接配置和调用示例见[项目 README](../README.md#generate-a-script-with-the-agent)。
