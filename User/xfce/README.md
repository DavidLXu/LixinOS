# LixinOS 上的 Xfce 会话

在 QEMU 图形终端运行 `startxfce4`。顶栏的 **Files** 会在当前工作目录打开 Thunar。选中文本文件后按回车即可用 Mousepad 编辑，按 Ctrl+S 保存。**Applications** 返回启动器，**Next window** 或 Ctrl+] 切换已打开的窗口。点 **Close window** 请求应用退出。如果应用长时间没有响应，按提示再次点击可强制退出，未保存的修改会丢失。关闭应用后点 **Log out** 返回终端，然后可以运行 `desktop` 进入原桌面。两个会话共享工作盘上的文件和应用。

会话在用户态通过 LixinOS 的窗口协议管理一个面板和最多四个应用窗口，前台一次显示一个应用。启动器读取 Garcon 菜单条目，同时查找 `/bin` 下的 `.win`、`.win2` 和 `.win3` 程序。当前集成了 xfce4-panel 4.20.8 的面板容器和时钟、xfdesktop 4.18.1 的背景绘制、Garcon 4.20.0 和 Thunar 4.18.11。窗口布局和会话管理由 LixinOS 提供。完整的 xfwm4、xfce4-session、Xfconf 设置服务和外部面板插件尚未移植。

## 构建和装盘

在 Mac 上从仓库根目录运行以下命令。使用带有 GTK 3 字体资源及 Mousepad 的工作盘，安装时先退出使用该盘的 QEMU。

```sh
bash User/xfce/build.sh
daily="$HOME/Library/Application Support/LixinOS/daily.img"
python3 User/gcc/install.py --toolchain xfce Kernel/target/user-build/xfce-port/package "$daily"
LIXINOS_DISK="$daily" ./scripts/run-kernel.sh --window
```

构建脚本复用 GTK 3 / Ristretto 的交叉编译工具链，按 SHA-256 校验上游源码。安装包只写入自己的可执行文件和 `/lib/xfce` 资源。其他工作盘若缺少 GTK 字体或 Mousepad，请先安装项目的 [GTK 3 包](../gtk3/README.md)。

## 图形回归测试

测试在 APFS 工作盘副本上启动 QEMU，通过键盘和指针操作 Thunar、Mousepad 和两个桌面入口，最后从 shell 读取文件以核对保存结果。在仓库根目录执行：

```sh
python3 scripts/test-xfce.py --disk "$daily" --kernel rust
python3 scripts/test-xfce.py --disk "$daily" --kernel tcc
```
