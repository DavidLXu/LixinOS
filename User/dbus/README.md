# LixinOS 原生 D-Bus 会话总线

本移植构建上游 D-Bus 1.14.10 的 `dbus-daemon`、`dbus-send` 和
`dbus-uuidgen`，以及使用 LixinOS 进程接口的 `dbus-run-session`。守护进程
用 Unix 流套接字和内核对端凭据完成 EXTERNAL 认证。启动器等待总线发布地址，
将 `DBUS_SESSION_BUS_ADDRESS` 传入应用，并在应用退出后回收总线。

在项目根目录构建。安装前退出使用目标磁盘的 QEMU。

```sh
bash User/dbus/build.sh
python3 User/gcc/install.py --toolchain dbus Kernel/target/user-build/dbus-port/package /path/to/lixinos.img
```

在来宾 shell 中启动一个独立会话并查询总线。

```sh
dbus-run-session -- dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames
```

交叉编译的 `libdbus-1.a`、头文件和 pkg-config 文件位于
`Kernel/target/user-build/dbus-port/sdk`。当前配置覆盖同用户的会话总线，
系统总线和自动服务激活仍待移植。

```sh
python3 scripts/test-dbus.py --release kernel-2026-11-22.2
```

附加 `--gio` 会使用 [GNOME 构建的 GLib](../gnome/README.md) 再验证一次
真实的 GDBus 调用。测试从一次性磁盘分别启动 Rust 和 TCC。
