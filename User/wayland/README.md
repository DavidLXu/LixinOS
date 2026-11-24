# LixinOS Wayland 用户态移植

上游 Wayland 1.24.0 的客户端、服务端和协议扫描器可在本项目构建。服务端通过
LixinOS 的 `poll`、单调时钟和非阻塞管道处理描述符、定时器与退出唤醒。客户端
和服务端分别运行在两个来宾进程中，通过 Unix 套接字进行 Wayland 协议通信。

`wayland-demo` 启动子进程 `wayland-shm-client`。客户端向服务端提交
`wl_shm` 像素缓冲区，服务端校验所有像素，再提交到 QEMU 图形终端的原生画布。
客户端等待服务端发回 `wl_buffer.release`。图形终端中按 Esc 或 Q 回到 shell。
串口可运行 `wayland-demo --headless`，检查同一组协议交互、像素和定时器。

## 构建和安装

在项目根目录执行，安装前关闭使用目标磁盘的 QEMU。

```sh
bash User/wayland/build.sh
disk=/path/to/lixinos.img
python3 User/gcc/install.py --toolchain wayland Kernel/target/user-build/wayland-port/package "$disk"
LIXINOS_DISK="$disk" LIXINOS_TCC_DISK="$disk" ./scripts/run-kernel.sh --release kernel-2026-11-22.2 --window
```

安装包包含两个可执行文件和 `/lib/wayland` 下的许可与安装记录。
同一块盘可以安装 `desktop`、`startxfce4` 和 `wayland-demo`，从 shell 启动所需的
入口。每个图形终端由当前前台程序接管。

构建结果 `Kernel/target/user-build/wayland-port/sdk` 提供 ARM64 静态库、libffi、
cursor 和 wayland-egl 辅助库、头文件、pkg-config 元数据、Wayland 核心协议 XML 和在宿主机运行的
`bin/wayland-scanner`。目标程序沿用 [LixinOS 应用运行时](../gcc/README.md) 链接。

## 验证

```sh
python3 scripts/test-wayland.py --release kernel-2026-11-22.2 --graphics
```

测试在一次性磁盘上分别启动 Rust 和 TCC。每套内核重复两次串口协议测试，图形
测试还读取 QEMU 帧缓冲区核对客户端像素。附加 `--disk PATH --with-xfce`
可在磁盘副本上依次启动 Wayland 演示、Xfce 和原桌面。测试会在副本上补齐缺少的
[GTK 3](../gtk3/README.md) 与 [Xfce](../xfce/README.md) 安装包。

## 当前接口范围

合成器示例发布 `wl_compositor` v1 和 `wl_shm`，渲染固定尺寸的 XRGB8888
缓冲区。服务端支持直接创建具名 socket，或通过 `wl_display_add_socket_fd` 接入已有监听。
应用可用 `wl_event_loop_dispatch` 直接派发，或通过 `lixin-wayland-loop.h` 的
接口将描述符与定时器接入外层 `poll`。聚合事件描述符和信号源仍待实现。当前共享内存
支持固定大小的缓冲池，在线增大缓冲池仍需内核映射扩展能力。

GNOME Shell 还需要 Mutter 的显示与输入后端、EGL 渲染、GJS 和会话服务。
