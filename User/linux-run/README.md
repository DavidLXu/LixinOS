# 在 LixinOS 上运行 ARM64 Linux 动态程序

`linux-run /bin/linux-hello` 在 LixinOS 用户态执行 Linux PIE。运行器准备 Linux 参数栈和辅助向量，从 Debian glibc 2.36 加载器的入口开始解释 AArch64 指令。加载器自行查找共享库并处理重定位，程序的输出和退出状态传回 LixinOS shell。

## 启动微信

腾讯的微信 4.1.13.29 ARM64 主程序和 Debian glibc 在 LixinOS 用户态运行。`wechat` 启动本机的 Mutter 49 和 D-Bus 会话，将微信窗口显示在当前图形终端。微信的 Linux 线程由运行器调度，Wayland 通过 LixinOS 的 Unix socket 和共享内存传递画面。glibc 的 NSS 模块调用 LixinOS 解析域名，IPv4 连接使用 LixinOS 的 TCP 栈。

在仓库根目录用准备好的独立镜像启动，然后在 LixinOS 图形终端输入 `wechat`。

```sh
LIXINOS_GPU=ramfb LIXINOS_DISK="$PWD/Kernel/target/linux-user/wechat.img" \
  ./scripts/run-kernel.sh --kernel rust --release kernel-2026-11-25.7 --window
```

有微信数据的旧盘继续使用 `/wx-state`。新构建盘的首次启动使用 `/scratch/wx-state`，把配置和缓存的小写入合并到检查点。两个目录都用 `home/` 保存用户文件、`run/` 放会话 socket、`tmp/` 放临时文件。包内的 Mutter 安装在 `/lib/wechat-ui`。可以保留这份镜像供下次启动。

在已有本仓库的 GNOME/Mutter 构建树、GNOME 字体与键盘数据包和 D-Bus 包的 Mac 上，可用自己的官方安装包重建镜像。输出路径必须是新的文件名。图形测试用 APFS 克隆启动镜像，用 OpenCV 从 QEMU 帧缓冲解码登录码、检查域名，再点击齿轮验证设置页。测试不需要微信账户。

```sh
python3 scripts/prepare-wechat.py \
  --deb /path/to/WeChatLinux_arm64.deb \
  --output Kernel/target/linux-user/wechat.img
python3 scripts/test-wechat-window.py Kernel/target/linux-user/wechat.img --kernel rust
```

在 Apple Silicon 的 QEMU 上，Rust 内核已通过图形测试。从新建镜像输入 `wechat`，屏幕上的二维码能解码出 `weixin.qq.com` 的登录地址，点击右上角齿轮能打开网络代理设置。在同一镜像的副本上，`ls -lah`、执行 Python 脚本和用 TinyCC 编译运行 C 程序也已通过。同一内核发行版的 TCC 内核也显示了微信登录窗口，7 分钟的图形测试内没有读到可扫码的二维码。建议当前使用 Rust 内核。确认登录需要由账户持有人在手机上操作。聊天、语音通话、小程序和文件传输仍待实测。

## 微信主程序的依赖检查

在装有测试包和依赖的 LixinOS 临时盘中运行

```sh
linux-run /lib/ld-linux-aarch64.so.1 --list /opt/wechat/wechat
echo $?
```

已用 Apple Silicon 上的 QEMU 验证腾讯微信 4.1.13.29 ARM64，主程序为原包中的 174,290,624 字节 ELF。在 `kernel-2026-11-25.7` 的 Rust 和 TCC 内核上，基础盘都由加载器报告缺少 `libglib-2.0.so.0`，退出码为 127。装入 Debian 依赖后，两边都列出 49 个共享库和加载器自身，退出码为 0。12 个库来自微信目录，其余 37 个来自 Debian。这个检查覆盖主程序的直接和间接启动依赖；微信窗口、扫码登录以及辅助进程需要单独验证。

运行器有 768 MiB 低地址区，另用稀疏映射承载较大的虚拟地址预留，按 4 KiB 页装入文件数据，并分别检查读、写、执行权限。Linux 程序关闭文件后，尚未读取的映射仍可使用原文件句柄。接近 LixinOS 进程的句柄上限时，运行器优先读完剩余页最少的已关闭文件。在上述完整依赖检查中，每个内核实际装入 1,436 个文件页，共 5,768,408 字节。这个数字只统计运行器的文件分页读取。

## 在 Mac 上复测

先构建运行器，再在仓库根目录运行以下命令。将 `/path/to/WeChatLinux_arm64.deb` 换成腾讯 4.1.13.29 ARM64 安装包的路径。[微信 Linux 官方下载页](https://linux.weixin.qq.com/)

```sh
bash User/linux-run/build.sh
python3 scripts/test-linux-run.py --release kernel-2026-11-25.7

python3 scripts/test-wechat-loader.py --release kernel-2026-11-25.7 \
    --deb /path/to/WeChatLinux_arm64.deb

python3 scripts/test-wechat-loader.py --release kernel-2026-11-25.7 \
    --deb /path/to/WeChatLinux_arm64.deb --with-debian-libs
```

每次测试都创建临时盘，默认依次引导两个内核。微信测试校验腾讯安装包的 SHA-256，仅从包中提取主程序及检查所需的同目录库。完整测试根据 `wechat-libs.lock` 下载并校验 Debian 软件包，缓存放在 `Kernel/target/linux-user/wechat-debs`。腾讯安装包的后续版本需要重新核对依赖和哈希。

## 运行器范围

`linux-run --trace PROGRAM` 可显示加载路径、实际打开的文件、系统调用和分页读取统计。运行器支持共享地址空间的 Linux 线程、TLS、futex、poll/epoll、Unix socket、memfd 和部分文件系统操作。通过 `linux-run --processes [--state DIR] PROGRAM` 启用独立子进程、等待回收和单线程子进程重新装载同一程序。该模式支持用户 ID 映射、嵌套 PID 命名空间、无 IPv4 接口的私有网络命名空间、`SIGCHLD` 投递、Unix 数据包凭据和共享内存文件描述符传递。普通启动方式沿用原来的线程模式。共享库从 LixinOS 文件系统以只读方式打开，支持 Debian 的 ARM64 库目录和目标程序同目录的库。未实现的系统调用返回 `-ENOSYS`。

构建脚本校验源码和 Debian 归档，输出到 `Kernel/target/linux-user/package`。测试程序使用 Clang 的 `aarch64-linux-gnu` 目标和 Debian 的启动文件。CPU 指令解释沿用 [arm64emu-user 的解释器](https://github.com/sylirre/arm64emu-user/tree/90fe88ca71794d29eecddc4b8925d68b79a2c342/src/core)。普通程序回归测试还会执行 Debian 的 GNU Hello，检查参数、输出重定向、退出码，以及移走和恢复 libc 后的行为。
