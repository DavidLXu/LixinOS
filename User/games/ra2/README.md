# Vanguard 开放素材遭遇战（LixinOS 命令 `ra2`）

在 QEMU 图形终端输入 `ra2`，可以在 Proving Grounds 地图上与一个苏军电脑对手进行遭遇战。默认安装包含开放地图、单位和建筑美术，可以离线游玩。游戏逻辑来自 [Vanguard](https://github.com/harshilmathur/openra2/tree/8ba59f0bcd48ba0c89892c0455eeca7da4408f4c)。LixinOS 版用 QuickJS 运行模拟，用原生画布绘制地图并接收输入。当前提供苏军单人遭遇战，尚无战役、联机、存档或声音。

LixinOS 还提供两个独立的上游移植入口 `ra2-engine` 和 `ra2-recomp`。前者读取玩家自己的零售档案，后者还需从对应版本的游戏程序生成代码。详见[两个社区移植的构建与验证说明](../ra2-native/README.md)。

## 操作

| 操作 | 键盘或鼠标 |
| --- | --- |
| 展开基地车 | 选中基地车，按 `D` 或空格。开局已选中。 |
| 生产和放置建筑 | 点右侧按钮或按对应数字。进度条走完后按钮变绿并显示星号，再点按钮，然后在基地附近点击有效地块。 |
| 建立经济 | 先造电厂，再造矿厂。选中采矿车，点击已发现的矿区可下达采矿指令。 |
| 训练部队 | 电厂建好后可造兵营和步兵，矿厂建好后可造战车工厂和坦克。 |
| 移动和攻击 | 点选己方单位，或拖框选中多支部队。点击地面移动，点击可见敌人攻击。按 `A` 后点击地面，部队会在推进途中主动交战。 |
| 移动画面 | 方向键或 `H J K L`，`F` 回开局位置。 |
| 退出 | `Q` 回 Shell。 |

## 构建与验证

构建使用项目现有的 AArch64 GCC 和 CPython 移植的 musl sysroot，宿主需要 Git、Node/npm、Python 和 Pillow。构建脚本按提交获取 Vanguard 和 QuickJS，生成游戏包。安装时关闭正在使用目标盘的虚拟机。

```sh
bash User/games/ra2/build.sh
python3 User/gcc/install.py --toolchain ra2 Kernel/target/user-build/ra2-port/package "$HOME/Library/Application Support/LixinOS/daily.img"
```

`ra2 --check` 在系统内验证模拟。以下测试分别启动两个内核，在临时克隆的盘上检验模拟、显卡输出、键盘展开基地车、鼠标点击生产栏和退出。

```sh
python3 scripts/test-ra2.py --disk "$HOME/Library/Application Support/LixinOS/daily.img" --kernel rust
python3 scripts/test-ra2.py --disk "$HOME/Library/Application Support/LixinOS/daily.img" --kernel tcc
```

如果已有包含 `ra2.mix` 和 `ra2md.mix` 的《尤里的复仇》安装，可以设置 `RA2_DIR` 指向目录，`RA2_MAP` 指定上游可读取的地图名，再运行构建脚本。导出器调用 Vanguard 的资源读取器，将选定地图与苏军单位打包。此次双内核验证使用开放素材。游戏包中的 `/lib/ra2/ASSETS.md` 记录了开放素材的来源。
