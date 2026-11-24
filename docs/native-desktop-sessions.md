# 原生桌面会话

LixinOS 在同一块工作盘上保留独立的桌面入口。现有的 `desktop`
继续从图形终端启动。Xfce 和原生 GNOME Shell 使用独立的安装目录和启动命令。
桌面会话接管前台显示时，由启动该会话的图形终端接收输入。应用安装和会话切换
不需要替换 `/bin/desktop`。

在图形终端运行 `startgnome` 进入 GNOME Shell，或运行 `startxfce4` 进入 Xfce。
注销返回终端后可以运行 `desktop` 进入原桌面。在配对发布 `kernel-2026-11-22.6` 上，
Rust 和 TCC 都通过了 GNOME 图形启动、注销和同盘启动另两个桌面的测试。
详见 [GNOME 装盘与验证](../User/gnome/README.md) 和 [Xfce 会话与安装说明](../User/xfce/README.md)。

## VirtIO GPU 输出

图形窗口和分屏默认优先使用 VirGL。旧版内核或缺少 VirGL 的 QEMU 自动使用 `ramfb`。
要手动使用软件显示，可设置 `LIXINOS_GPU=ramfb`。选择 VirtIO GPU 2D 时，在宿主终端运行下面的命令。
Rust 和 TCC 共用 MMIO 槽位 7 上的显示驱动。可将 `rust` 换成 `tcc`，
或将 `--window` 换成 `--split`。

```sh
LIXINOS_GPU=virtio ./scripts/run-kernel.sh --kernel rust --release latest --window
python3 scripts/test-virtio-gpu.py --release latest
```

驱动读取显示信息，将内核像素缓冲区绑定到虚拟显示器，合并变化区域后发送
`TRANSFER_TO_HOST_2D` 和 `RESOURCE_FLUSH`。测试启动只有 VirtIO 显示设备的
QEMU，检查两个窗口尺寸及画布两次提交后的像素。

选择 VirGL 时，Mesa 将 GLES 命令通过 LixinOS 的进程私有上下文提交给
VirtIO GPU。应用可用 `glReadPixels` 从虚拟 GPU 读回结果。启动器优先使用
项目缓存里的 [支持 VirGL 的 QEMU 构建](https://github.com/Benehiko/vee/releases/tag/qemu-11.1.0-vee2)。
也可用环境变量 `LIXINOS_QEMU` 指定其他支持 Cocoa GLES 和
`virtio-gpu-gl-device` 的 QEMU 可执行文件。

```sh
./scripts/run-kernel.sh --kernel rust --window
```

安装了 Mesa 测试包的客户系统可运行 `lixin-egl-check --headless` 查看渲染器和着色器像素。
在宿主上可以用 `python3 scripts/test-egl.py --virgl` 对两个发布内核做同样的检查，
并读取 QEMU 进程在 macOS 上的 Metal 活动。已安装的 GNOME Shell 静态链接了构建它时的 Mesa。
装入重新链接的 GNOME 包后，在图形终端运行 `startgnome`。Mutter 首次提交桌面画面时
会输出 `LixinOS compositor renderer: virgl`。在宿主上运行
`python3 scripts/test-gnome-shell.py --virgl --release latest` 可重复验证。

## Unix 域流套接字

Rust 和 TCC 内核通过同一用户态接口提供 `AF_UNIX` / `SOCK_STREAM`。
支持 `socketpair`，抽象名和路径名的 `bind`、`listen`、`connect`、
`accept`，以及 `poll`、流数据的 `sendmsg` 和 `recvmsg`、半关闭和
`SO_PEERCRED`。连接队列最多四个。路径地址在内核内注册，监听描述符
关闭时释放；当前无需在盘上创建套接字节点。

## Wayland 共享缓冲区的底层接口

两套内核都能通过 `memfd_create` 创建匿名文件，用 `ftruncate` 或
`posix_fallocate` 分配空间，经 Unix 流套接字的 `SCM_RIGHTS` 传给另一个
进程。两端用 `mmap(MAP_SHARED)` 映射同一组物理页。映射保留自己的引用，
关闭全部描述符后也可继续使用，直到解映射或进程退出。

这一版支持只读和可读写映射、4 KiB 对齐的文件偏移、每个匿名文件最多
16 MiB。映射期间改变文件大小会返回 `EBUSY`，`munmap` 释放一次完整
映射。一次辅助消息最多传八个匿名文件描述符。文件封印、一般文件或设备
描述符传递、部分解映射和 `mremap` 仍待实现。

上游 libwayland 已接入用户态事件循环。独立客户端经 Wayland 协议提交
共享像素缓冲区，最小服务端将像素呈现到原生画布。运行 `wayland-demo`
可查看画面，或用 `wayland-demo --headless` 验证协议和定时器。
详见 [Wayland 构建和图形测试](../User/wayland/README.md)。GNOME 路线已
验证 [D-Bus 会话总线](../User/dbus/README.md)、Mesa 软件 EGL、GJS 与静态
GI 回调。配对发布 `kernel-2026-11-22.6` 上的两套内核都已用 Mutter 合成
Wayland 客户端，并在 QEMU 显示真实帧。详见 [GNOME 移植状态](../User/gnome/README.md)。
当前可运行的 Nautilus 使用原生 GTK 后端。

在项目根目录对最新配对发布运行：

```sh
python3 scripts/test-local-sockets.py --release latest
```

测试使用一次性磁盘，从 Rust 切换到 TCC，并在每套内核上检查跨进程通信、
对端凭据、非阻塞返回、缓冲区满时的行为以及关闭后的地址重用。共享缓冲区
测试覆盖非零文件偏移、双向写入、关闭描述符后继续访问，以及丢弃辅助数据
时回收未接收的描述符。
