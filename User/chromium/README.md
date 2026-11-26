# LixinOS Chromium 窗口移植

这里为 Chromium 150.0.7871.100 的 `//content/shell:content_shell` 提供 ARM64 Linux ABI 构建配置。Blink、V8、Skia 和 Ozone/Wayland 负责页面、脚本、绘制和窗口输入。运行目标是 LixinOS 来宾内的 Wayland 桌面。

## 构建

在对应版本的完整 Chromium 源码树上运行以下命令。源码树需要同步上游 DEPS、工具链和 ARM64 Linux sysroot。脚本会给该源码树应用仓库内的补丁，并使用其中的 `out/LixinOS` 目录。构建工具使用 depot_tools 的 `gn` 和 `autoninja`，也可以通过 `GN` 和 `AUTONINJA` 指定可执行文件。

```sh
User/chromium/build.sh /path/to/chromium/src --configure-only
User/chromium/build.sh /path/to/chromium/src
```

第一条命令生成并检查传递依赖清单 `out/LixinOS/content_shell.deps.txt`。第二条编译同一个目标。构建按体积优化，去掉调试符号，排除完整 Chrome、X11、PDF 查看器、打印、扩展、网页测试运行器、测试用追踪处理器、网络和 GPU 测试服务、浏览器测试控制接口、实验分组测试配置、独立崩溃报告程序、EGL/GLES/Vulkan 运行库、DevTools 前端和布局测试字体。Blink 在来宾中初始化 JavaScript 上下文，省去为 Mac 再编译一套 Blink 生成上下文快照的步骤。桌面窗口沿用上游的两个 Views 委托实现。构建清单会检查整套 Views、Aura 和合成器测试库，以及用于桌面录屏的 PipeWire portal 已被排除。部署时需要为页面提供实际字体。

软件渲染配置也排除了 VA-API 硬件视频编解码、端侧模型执行服务和它依赖的 Dawn 原生后端。桌面协议注册沿用 Content Shell 的委托，网页权限查询继续使用原有的来源和 Content Shell 策略。这个目标不再编译 Chrome 专用的协议注册许可弹窗、权限预测服务及完整的表单自动填充后端。

本地 ARM64 构建的主程序剥离符号后为 144,248,024 字节（137.6 MiB），比本项目首次构建的 259,853,528 字节减少 44.5%。包含 ICU 数据、V8 快照、页面资源、中文字体和依赖库的部署目录共 40 个文件，合计 172,188,691 字节。安装到本次 Mutter 和微信模板盘时复用了 27 个已有系统库，盘内文件数据净增 163,386,530 字节（155.8 MiB）。

## 打包

链接成功后，以下命令生成软件窗口所需的来宾文件树，并为中文页面添加文泉驿微米黑。打包脚本用目标工具链的 llvm-strip 剥离主程序符号，再根据 ELF 的 DT_NEEDED 逐层选取共享库。来宾已有的运行库可以复用，新增库安装到 /opt/chromium，由主程序通过 `$ORIGIN` 查找。脚本分别报告主程序字节数和新增文件总大小。将 ARM64_LIBS 换成与来宾兼容的 AArch64 运行库目录。只有链接符号的构建桩库不能用于部署。

```sh
curl -fLO https://deb.debian.org/debian/pool/main/f/fonts-wqy-microhei/fonts-wqy-microhei_0.2.0-beta-3.1_all.deb
python3 User/chromium/stage.py CHROMIUM_SRC/out/LixinOS /tmp/lixinos-chromium-root \
    --font-package fonts-wqy-microhei_0.2.0-beta-3.1_all.deb \
    --guest Kernel/target/linux-user/package --library-dir ARM64_LIBS \
    --readelf CHROMIUM_SRC/third_party/llvm-build/Release+Asserts/bin/llvm-readelf
```

`--font-package` 会校验 Debian 包的 SHA-256，并将字体安装到 Chromium 私有目录。目标盘已有中文字体时可省略该选项。

在已有 Mutter、D-Bus 和字体的构建盘上，可以安装到一份新的本地磁盘副本。原盘与新盘需位于支持克隆的同一 APFS 卷。安装器逐字节比较模板盘的系统库，复用相同版本，并报告新盘上实际增加的文件数据量。

```sh
bash User/linux-run/build.sh
python3 scripts/prepare-chromium.py MUTTER_DISK STAGED_LINUX_ROOT chromium.img
LIXINOS_GPU=ramfb LIXINOS_DISK="$PWD/chromium.img" ./scripts/run-kernel.sh --kernel rust --release kernel-2026-11-26.4 --window
```

来宾命令 `chromium` 打开空白页。`chromium file:///home/chromium/example.html` 打开来宾私有主目录中的 `example.html`。新打包的构建盘使用 `/scratch/chromium-state/home`，已有用户数据的安装继续使用 `/chromium-state/home`。构建卷会合并 `/scratch` 下的小写入。应用请求 `fsync`、启动器正常退出或系统正常关机时，内核会保存待写的数据。启动器创建私有状态目录，启动 Mutter 和 D-Bus，并通过 Wayland 共享内存输出画面。纯软件合成服务与浏览器共用进程，页面渲染器和网络服务仍运行在独立的来宾进程中。

## 来宾验证

精简版在 `kernel-2026-11-26.4` 的 Rust 和 TCC 内核上均已显示本地中文页面，并通过公网 HTTPS 加载了 example.com 的正文。在 TCC 上，远端可能在首次证书验证期间关闭空闲连接。对于尚未发送、没有请求体的 GET 或 HEAD，浏览器发现连接已关闭时会移除失效的 HTTP/2 会话，并允许一次新连接。实测返回 HTTP 200。Linux 兼容层支持 Chromium 内存分配器使用 `MADV_DONTNEED` 回收低地址私有匿名页。双内核测试检查了重新读取时的零值和相邻页的数据。

两个内核均通过了本地文件和 HTTP 页面的中文实际字形、点击、键盘输入、滚动和最大化重绘测试。关闭并重新打开 Chromium 后，两边都能从 localStorage 读回之前输入的字符。同盘微信也在两个内核上显示了可解码的登录二维码，鼠标可以打开网络设置。本次 QEMU 测试从输入命令到页面首次绘制，本地文件在 Rust 上用时 24.3 秒，TCC 上用时 115.2 秒。HTTP 页面分别用时 27.7 秒和 127.3 秒。Rust 上访问带自签名证书的本地服务器时返回 `ERR_CERT_AUTHORITY_INVALID`，服务器没有收到 HTTP 请求。

窗口使用软件合成，此构建省去了 WebGPU、硬件视频加速、PDF 查看器、打印和扩展。Linux 兼容层仍缺少 `inotify` 文件变更通知和完整的 seccomp-BPF 系统调用过滤。

在成对发布的 `kernel-2026-11-26.4` 上，Rust 和 TCC 均通过了共享内存容量回归。对 `/dev/shm` 分配 2 MiB 后，`statfs` 和 `fstatfs` 会报告内核剩余容量的相应变化。Linux 程序可以读取内核提供的 `/proc/cpuinfo`，通过 `/proc/self/task` 枚举当前进程的活动线程，并在 join 后观察到线程目录消失。父进程可从 `/proc/<pid>/task/<tid>/status` 读取嵌套 PID 命名空间里的线程号。进程退出时，兼容层会唤醒仍在等待 futex 的线程。两个内核均通过了父进程等待多线程子进程退出的回归。

在成对发布的 `kernel-2026-11-26.3` 上，Rust 和 TCC 都可以运行该 Chromium 版本附带的 SQLite，创建表、提交数据并在关闭后重新打开数据库读回原值。两个内核还验证了不同进程间的共享读锁、字节范围冲突查询、阻塞等待，以及同一进程关闭另一份文件描述符后解除记录锁。

在成对发布的 `kernel-2026-11-26.1` 上，Rust 和 TCC 都通过了真实 IPv4 连接的 TCP 预读测试。测试先检查空队列返回 EAGAIN，再对同一字节预读两次，随后读取完整数据并检查对端关闭后的 EOF。这对内核也通过了跨页文件读取和 poll/epoll 事件缓冲区回归。

在成对发布的 `kernel-2026-11-25.18` 上，Rust 和 TCC 已通过 128 个同时映射的共享缓冲区、文件描述符关闭后的数据校验、碎片地址空间中的跨页表映射和释放测试。同版本 Debian Chromium 已在 Rust 内核的 Mutter 窗口显示本地 HTML。QEMU 的鼠标单击、键入字符和滚轮动作均已触发相应的页面 JavaScript，并在实际屏幕上显示预期颜色。TCC 内核上的同版本程序也已完成以上三项交互，并在窗口缩放后触发页面重绘。两个内核还通过了私有目录重命名、普通文件规范路径和继承文件描述符上限的回归。

在成对发布的 `kernel-2026-11-25.16` 上，两个内核均已通过同时打开 72 个文件、跨进程共享文件偏移和微信登录窗口点击测试。Debian Chromium 已连接 Mutter 并创建 Wayland 表面。

在 `kernel-2026-11-25.8` 的 Rust 和 TCC 临时盘上，同版本 Debian ARM64 `chromium-shell` 已完成动态链接、创建独立子进程、映射用户和组身份、启动 zygote 并通过套接字交换进程身份。Debian 主程序为 199,101,344 字节。

`linux-run --processes` 开启 Chromium 所需的进程接口。启动图形程序时，用 `--state DIR` 提供其私有主目录和临时目录，并让来宾的 Mutter 在 `DIR/run` 上提供 Wayland 服务。

```sh
bash User/linux-run/build.sh
python3 scripts/test-linux-run.py --release kernel-2026-11-26.4 --check-shared-space --check-descriptor-capacity --check-shared-mappings --check-tcp-peek --check-record-locks
```

回归检查覆盖大 PIE、独立地址空间、共享映射、子进程退出回收、同一程序的 `execve`、`SIGCHLD`、命名空间以及带凭据和文件描述符的 Unix 数据包。图形测试会复制所给的磁盘，并把离线页面放入浏览器的私有目录。它通过 QEMU 显示输出检查 CSS、JavaScript、点击、打字、滚动和窗口大小变化。

```sh
python3 scripts/test-chromium-window.py MUTTER_DISK STAGED_LINUX_ROOT --release kernel-2026-11-26.4
```

如果磁盘已经安装了启动器，省略 `STAGED_LINUX_ROOT` 即可通过来宾的 `chromium` 命令测试。加上 `--check-cjk` 会在页面画布上比较“中”“文”与缺字字形的实际像素。加上 `--http` 将从宿主机的回环 HTTP 服务加载同一页面，并验证服务端确实收到了来宾的请求。`--check-profile` 通过窗口关闭应用，重新启动后检查网页能否从 localStorage 读回实际输入的字符。模板盘需要 Mutter、D-Bus 和字体。测试默认使用 `/bin/mutter` 与 `/opt/chromium/content_shell`，可用 `--compositor` 和 `--shell` 指定其他来宾路径。
