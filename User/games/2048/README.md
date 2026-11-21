# 2048 桌面版

在 LixinOS 输入 `desktop`，从左侧 **Apps** 打开 **2048.win3**。用方向键、WASD 或在棋盘上拖动鼠标来滑动方块。两个相同的数字相撞时会合并，合并出的数字计入分数。只有有效移动才会生成新方块。拼出 2048 后可以继续挑战；按 **R** 或点击 **New (R)** 重新开局，按 **Q** 或关闭窗口退出。

游戏逻辑取自 Maurits van der Schee 的 [2048.c 1.0.3](https://github.com/mevdschee/2048.c/tree/90343eb209e84fa9687d596f5d5eecfdf7d4df51)，保留了移动、单次合并、计分和终局判定。窗口、数字绘制、输入和随机出块接入 LixinOS 用户态接口。[MIT 许可证](LICENSE)

![QEMU 中的 2048 棋盘和得分](../../../docs/screenshots/2048-desktop.png)

## 构建与安装

在 Mac 的项目根目录运行以下命令。构建脚本从桌面盘创建临时副本，在 QEMU 客体内调用 TCC 生成 LXE1 程序；安装命令把程序、源码和许可证写入已关机的目标盘。

```sh
disk="$HOME/Library/Application Support/LixinOS/daily.img"
python3 User/games/2048/build.py "$disk" --release kernel-2026-11-21.19
python3 User/gcc/install.py --toolchain 2048 Kernel/target/user-build/2048-package "$disk"
```

安装后也可以直接在 LixinOS 里修改源码、重新编译，再从 Apps 打开新版本：

```sh
tcc /lib/2048/game.c -o /bin/2048.win3
```
