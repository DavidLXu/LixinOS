# LixinOS 应用清单

在图形终端输入 `desktop`。常用入口固定在左侧 Dock，点 **Apps** 打开全屏应用网格。用滚轮或按住图标区域上下拖动来浏览，点图标启动应用。顶部分组可以筛选应用，右上角「窗口」可找回已打开或最小化的窗口。点「完成」或按 Esc 回到桌面。

打开 Apps 即可输入应用中文名、英文名、命令、拼音全拼或首字母。例如，输入「设置」、`shezhi`、`xtsz` 或 `settings` 可找到系统设置。开始输入时会搜索全部分类，点分类可以缩小范围。方向键选择结果，回车启动，Esc 先清空搜索，再按一次退出。

## 从应用程序库打开命令行工具

点 **Apps → 终端**。每个图标下方显示实际命令，只列出当前盘上已安装的工具。点图标会打开一个新的终端标签并运行命令。程序结束后，输出留在窗口里，可以接着输命令。

| 想做什么 | 在「终端」里点 |
| --- | --- |
| 让 AI 帮忙编程 | Codex 编程助手、DeepSeek Harness |
| 试一行 Python | Python 解释器、MicroPython |
| 管理代码、查看 JavaScript 运行时版本 | Git 版本管理、Node.js 版本 |
| 查看进程、系统配置或磁盘空间 | 进程监视器、系统信息、磁盘用量 |
| 用文本界面浏览网页 | Links 文本浏览器 |

在「编辑」点 **LixinOS Code** 或 **矢量画图**，在「游戏」点 **Doom** 或 **ClassiCube**，会直接进入程序的图形界面。

在 **Apps → 游戏 → 极品飞车 II SE** 启动原版赛车，也可以在图形终端输入 `nfs2`。在主菜单选 **RACE**，回车开赛。[操作和安装说明](../User/games/nfs2/README.md)

在 **Apps → 游戏** 可以分别打开「红警2 开放遭遇战」（Vanguard）、「红警2 新引擎」（RA2 Engine）、「红警2 原版重编译」和资料片「尤里的复仇」。四个图标各自启动对应的全屏游戏。

## 桌面应用

下表按 2026-11-21 核对的日常工作盘整理。新安装到 `/bin` 的 `.win`、`.win2`、`.win3` 程序会在下次打开菜单时出现，自编应用如果还没有分类，会显示在「其他」。

| Apps 分类 | 菜单中的名称 | 用途 |
| --- | --- | --- |
| 系统 | 应用商店、系统设置、任务管理器 | 按需制作或移植应用，调整系统设置，查看进程和资源占用 |
| 编辑 | Lite XL、Mousepad、LibreOffice Writer、PDF 阅读器 | 编辑代码和文本文档，处理 Word 和 ODT 文件，双击 PDF 可翻页、缩放和滚动，缩略图侧栏可快速找回某一页 |
| 工具 | 视频播放器、日历、地图、秒表 | 播放本地视频、浏览月份、查看地图、计时 |
| 游戏 | 2048、数字拼图、Eyes | 合并数字方块、匹配拼图边缘，或看一双跟随指针的眼睛 |
| 开发 | Agent、GTK 3 示例 | 打开 Agent 面板，或试用 GTK 控件；首次启动 Agent 时系统会用 TCC 构建它 |

![日常工作盘的全屏应用网格，上方是分类筛选和窗口入口](screenshots/applications-menu.png)

Writer 窗口提供段落样式、字体、字号、粗体、斜体、下划线、文字颜色、对齐和项目符号，`Ctrl+F` 查找、`Ctrl+H` 替换。在终端运行 `writer --headless --convert-to pdf --outdir /home/lixin /home/lixin/report.docx`，可用 LibreOffice Writer 将 Word 文档导出为 PDF。双击导出的文件即可阅读，按 Page Up / Page Down 翻页，点击「适合宽度」可随窗口大小调整页面。[Writer 构建与操作说明](../User/writer/README.md) · [PDF 阅读器操作说明](../User/pdfview/README.md)

从 **Apps → 工具 → 视频播放器** 启动可立即播放示例短片，也可以从 Dock 的 **Files** 双击自己的 MP4 文件。空格暂停，左右方向键跳转五秒。当前版本播放画面，音轨暂不输出。[视频格式与构建说明](../User/video/README.md)

打开 **Apps → 工具 → 物理仿真器** 可绘制二维刚性连杆，用转动关节或移动关节连接，再运行或单步观察。连杆支持线段、经过三点的圆弧和参数曲线，默认重力向下 9.8 m/s²。[操作说明](../User/physics/README.md)

安装 DeepSeek Harness 图形客户端后，从 **Apps → 开发 → DeepSeek 图形助手** 打开会话，选择工作目录，发送任务，或查看工具执行记录。[DSH 使用说明](../User/dsh/README.md#lixinos-原生图形客户端)

## 在应用商店做新软件

打开 **Apps → 系统 → 应用商店**，选择现场生成的点子，改成自己的需求，再点「交给 Agent」。有现成的单文件 C 源码可以选「移植源码」。通过测试的作品可安装到 Apps，也可以在「我的软件」继续迭代、回滚或卸载。[商店使用说明](../User/app-store/README.md)

## Dock 快捷入口

| 入口 | 用途 |
| --- | --- |
| Files | 浏览和管理文件，双击文件打开关联应用 |
| Browser | 用 NetSurf 浏览网页。也可以在 Bash 输入 `netsurf https://www.apple.com.cn/` |
| Calc | 打开计算器 |
| Terminal | 打开终端运行命令 |
| Nano、Editor | 打开 Nano 或图形文本编辑器 |
| Eyes | 打开 Eyes，已有窗口时切回它 |
| Apps | 按用途找程序，或切换到已打开的窗口 |

## 从 Bash 启动

| 想做什么 | 可以运行 |
| --- | --- |
| 编辑文本或代码 | `nano hello.c`、`kilo hello.c`、`dte hello.c`，或 `text-editor hello.txt` |
| 绘制矢量图 | 在图形终端运行 `paint sketch.svg` |
| 玩贪吃蛇 | `python /src/games/snake.py` |
| 在终端浏览网页、连接远程机器 | `links`、`links2`、`ssh`、`sftp` |
| 编译与版本管理 | `tcc` 编译 C，`lust` 编译它支持的 Rust 子集，`git` 查看改动、提交和查询历史 |
| 运行 Python | `python` 启动 MicroPython，`python3` 启动 CPython，`pip`、`pip3` 管理 CPython 包 |
| 查看系统状态 | `htop`、`ps`、`sysinfo`、`cpuinfo`、`netinfo`、`df`、`du`、`uname` |
| 管理和查找文件 | `ls`、`tree`、`find`、`stat`、`cp`、`mv`、`mkdir`、`rm`、`touch` |
| 搜索、比较或处理文本 | `cat`、`grep`、`sed`、`diff`、`cmp`、`head`、`tail`、`sort`、`uniq`、`wc` |

在自己的盘上运行 `ls /bin` 可以查看已安装的程序文件。Dock 的计算器等内置窗口由 `desktop` 提供。系统操作示例见[系统手册](README.md)，窗口协议和自编应用的安装方式见[桌面开发说明](../User/desktop/README.md)。

### Kernel-scheduled pthreads

Programs linked with the current musl adapter use a scheduler slot per thread
on both kernels. A process shares its memory mappings and descriptors. Each
thread has a distinct TID, stack and TLS. With two vCPUs the scheduler places
the first worker on the other core. On one vCPU the timer preempts the threads.
Processes and threads share the system's 16 scheduler slots. Musl returns
`EAGAIN` when they are full.

Rebuild the adapter with `bash User/gcc/build.sh rt` and relink static
applications to select the new backend. The adapter detects older kernels and
restricted execution contexts and uses the cooperative backend there.
`python3 scripts/test-kernel-threads.py --rust PATH --build-tcc` builds the
musl probe and tests both kernels with one and two vCPUs on a temporary disk.
