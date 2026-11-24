# LixinOS 原生 GNOME 移植

LixinOS 已能在 QEMU 图形终端原生运行 GNOME Shell 49.0。进入图形终端后运行
`startgnome`。在概览搜索 **Log Out** 并按回车可以返回终端，再运行
`desktop` 或 `startxfce4`。三种会话可装在同一块盘上。

当前图形栈使用 Mutter 49.0、GJS 1.86.0 和 Mesa 24.3.4。图形启动器默认优先选择 VirGL，
Mutter 通过 VirtIO GPU 合成桌面。回退到 ramfb 时使用 softpipe。GNOME Shell
的音频服务、IBus 输入法守护进程和完整的 GDM 图形登录待接入。

## 已验证的用户态基础

- [Wayland 1.24](../wayland/README.md) 提供原生客户端、服务端、协议扫描器和
  共享像素传输。协议库另提供可加入 GLib 主循环的描述符和定时器查询接口。
- [D-Bus](../dbus/README.md) 支持独立会话总线。GLib 的 GDBus 使用 LixinOS
  内核的对端凭据认证。
- GLib 2.86 的 GObject Introspection 可从 typelib 解析并调用静态链接的函数。
  应用通过 `static-modules.h` 提供模块与符号表。
- Mesa 24.3.4 的 softpipe 以 CPU 执行 OpenGL ES 着色器，通过离屏 EGL
  表面读回像素。SDK 提供 EGL、GLES 和 GBM 静态库和 pkg-config 元数据。
- SpiderMonkey 140 和 GJS 1.86 已在 Rust 和 TCC 上执行 JavaScript、加载
  typelib，并通过 libffi 把 GLib 回调送回 JavaScript。回调跳板随程序链接，
  堆内存保持不可执行。
- Mutter 49 使用虚拟显示器和离屏 EGL。`--lixinos-display` 将 Clutter
  合成的画面呈现到 LixinOS 图形终端。两套内核均通过了 `xdg-shell` 客户端
  的帧回调测试和 QEMU 屏幕像素检查。

```sh
bash User/wayland/build.sh
bash User/dbus/build.sh
bash User/gnome/build-deps.sh
bash User/gnome/build-mesa.sh
bash User/gnome/build-mutter.sh
bash User/gnome/build-mozjs.sh
bash User/gnome/build-gjs.sh
bash User/gnome/prepare-gjs-runtime.sh
bash User/gnome/build-shell.sh
bash User/gnome/prepare-shell-runtime.sh
python3 User/gnome/package.py Kernel/target/user-build/gnome-port/package

python3 scripts/test-dbus.py --release kernel-2026-11-22.2 --gio
python3 scripts/test-egl.py --release kernel-2026-11-22.2
python3 scripts/test-gnome-gi.py --release kernel-2026-11-22.2
python3 scripts/test-gnome-js.py --release kernel-2026-11-22.6 --with-gjs
python3 scripts/test-mutter.py --release kernel-2026-11-22.6 --graphics
python3 scripts/test-gnome-shell.py --release kernel-2026-11-22.6 \
    --screenshots Kernel/target/user-build/gnome-port/session-screenshots --with-xfce
```

构建在 `Kernel/target/user-build/gnome-port` 下进行。sysroot 里的 ARM64
静态库由 LixinOS 的 musl 适配层链接。
在支持 Cocoa GLES 的 QEMU 上，可用 `python3 scripts/test-gnome-shell.py --virgl --release latest`
检查两套内核上首帧的 GL 渲染器、工作区像素、宿主 Metal 活动和注销。Mac 上的 Bison、Meson、Ninja 和
`gi-compile-repository` 负责生成构建数据。测试使用临时磁盘。

会话测试对 Rust 和 TCC 使用同一块临时盘。每套内核分别验证启动日志、QEMU
画面、键盘搜索注销以及 `desktop` 和 `startxfce4` 的画面。

## 装盘

退出正在使用目标磁盘的 QEMU，设置 `disk` 为该磁盘的路径，再从仓库根目录运行。

```sh
cache=Kernel/target/user-build
python3 User/gcc/install.py --toolchain dbus "$cache/dbus-port/package" "$disk"
python3 User/gcc/install.py --toolchain gnome "$cache/gnome-port/package" "$disk"
LIXINOS_DISK="$disk" ./scripts/run-kernel.sh --release kernel-2026-11-22.6 --window
```

`startgnome` 为 Shell 创建独立的会话目录和 D-Bus 会话总线。未配置系统总线时，
启动器会为该会话创建一条独立的总线。退出 Shell 时，启动器会回收这次启动的总线。
