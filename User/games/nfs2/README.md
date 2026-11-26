# 《极品飞车 II SE》

在 LixinOS 图形终端运行 `nfs2`。主菜单用上下键选择 **RACE**，按回车进入比赛。默认单人比赛驾驶 McLaren F1，在 Proving Grounds 与七辆电脑赛车同场起步。上键加速，下键刹车，左右键转向。Esc 进入游戏菜单，Ctrl+Q 直接返回终端。

游戏运行 [nfs-recompiled 的 NFS2SEN 软件渲染版本](https://github.com/motor-dev/nfs-recompiled/tree/03a1c9390176de344b69616db282e0bc5737fa30)。原版 x86 游戏逻辑通过静态重编译生成 AArch64 程序。画布显示原版 8 位调色板和 RGB565 帧，SDL2 接收键盘输入，DirectSound 混音通过系统的 48 kHz 双声道 PCM 接口输出。

## 构建和安装

在宿主机准备含有 `FEDATA`、`GAMEDATA` 和 `install.win` 的 PC Special Edition 安装目录。构建脚本固定重编译器和 SSE2NEON 的提交，复用仓库的 AArch64 GCC、musl、C++ 运行库和 SDL2。安装到已关机的 LixinOS 构建盘。

```sh
bash User/games/nfs2/build.sh "/path/to/Need for Speed II SE"
python3 User/games/nfs2/install.py "$HOME/Library/Application Support/LixinOS/daily.img"
./scripts/run-kernel.sh --kernel rust --release latest --window
```

安装器统一素材文件名的大小写，并为仅配置 Glide 的安装补上软件渲染器的赛道目录。游戏数据位于 `/lib/nfs2/game`，玩家设置、比赛记录和临时赛道文件位于 `/scratch/nfs2`。再次安装会保留玩家资料。安装包只在本机构建缓存中生成。这次验证使用的素材来自[社区维护的 SE 安装目录](https://github.com/faai5200/NeedForSpeed2SE_Windows11/tree/0013ea2b330511b8a34e020a8f99cf04f47e2f26)。

## QEMU 回归

测试克隆已安装的工作盘，从原版菜单选择比赛，读取 QEMU 画面中的速度表，按住油门转向再松开，同时采集和检查游戏音频。下列命令使用默认比赛设置，支持用 `--screenshot 文件.png` 保存行驶画面。

```sh
python3 scripts/test-nfs2.py --kernel rust --disk data/shared.img
python3 scripts/test-nfs2.py --kernel tcc --disk data/shared.img
```
