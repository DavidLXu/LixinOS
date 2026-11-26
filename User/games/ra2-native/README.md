# LixinOS 上的两个《红色警戒 2》社区移植

| 命令 | 上游 | 在没有零售文件时可以验证什么 |
| --- | --- | --- |
| `ra2-engine` | [RA2 Engine 0.9.0](https://github.com/omarelarab/ra2-engine/tree/a8cfdccfef8592c13a6517c3fb76d834b1534027)，C++17、SDL2 | 启动上游界面，列出缺失的文件，鼠标点击 EXIT 返回 Shell。`--version` 和 `--help` 可用。 |
| `ra2-recomp` | [redalert2-recomp](https://github.com/sp00nznet/redalert2-recomp/tree/c973ad7f6e04079f89a46ad38a905bcda7070e32)，静态重编译 | 缺少翻译产物时构建上游 PE 加载器。它能检查零售程序版本、映射镜像并绑定导入。运行游戏还要先从该零售程序生成代码。 |
| `ra2-recomp-check` | 同一个重编译宿主及其 [pcrecomp 运行层](https://github.com/sp00nznet/pcrecomp/tree/7c07a0b42059b6dc4313d9b4ebded6f7a72db8f3) | 默认验证高位虚拟地址和 PE 加载、绑定、重定位。`draw` 运行上游 DirectDraw 自测。`host` 执行一段独立测试程序，预期返回 `eax=0000002A` 和退出码 42。 |

原有的 `ra2` 命令启动 Vanguard 开放素材遭遇战。这两个新程序各自保留上游菜单和游戏逻辑，零售文件由玩家提供。

## 在 LixinOS 内启动

在图形终端输入 `ra2-engine`。选择 **Single Player → Skirmish → Start Game** 进入遭遇战。选中盟军基地车后按 **D** 展开建造场。

输入 `ra2-recomp` 可启动从零售版 `game.exe` 重编译的《红色警戒 2》。菜单语言来自导入的游戏文件。这次使用的游戏目录提供繁体中文菜单。默认游戏从 `/lib/ra2-content` 读取零售素材，把游戏内写入的设置和存档放在 `/scratch/ra2-profile`。游戏会以几个字节为单位写入设置，构建盘在这个目录可以就地写入，正常关机会保存快照。显式启用的模组继续使用各自的目录。

## 宿主机构建

构建复用 LixinOS 的 AArch64 GCC、musl、C++ 运行库和 FreeType。源代码按提交固定到本地构建缓存。两个程序共用 SDL2 软件渲染、鼠标和键盘适配。进入图形窗口后可用 Ctrl+Q 发送退出事件。

```sh
bash User/games/ra2-native/build.sh ra2-engine
bash User/games/ra2-native/build.sh ra2-recomp
bash User/games/ra2-native/build.sh ra2-recomp-check
python3 User/games/ra2-native/install.py "$HOME/Library/Application Support/LixinOS/daily.img"
```

安装程序在空闲系统盘的下一份快照里更新三个基础命令。如果已构建资料片，也会安装 `yr-recomp`。构建检查程序时，pcrecomp 将一个自带的 x86 算术函数翻译成 C，再交叉编译成 AArch64。独立的小型 PE 与游戏数据分开打包。

## 有原版文件后的构建

`ra2-engine` 从 `/lib/ra2-content` 读取文件，也支持 `ra2-engine --data 目录`。上游最低要求是 `ra2.mix`、`ra2md.mix`、`language.mix` 和 `multimd.mix`。原版战役、语音和音乐还使用相应的可选档案。上游 0.9.0 支持盟军和苏军，尤里阵营和资料片战役仍受上游版本限制。

重编译版需要受上游支持的 Steam 或 EA 安装。零售程序的时间戳决定能否使用对应版本的代码补丁。对选中的程序运行分析、生成 C、编译，再把结果与本地游戏数据一起装到系统盘。

```sh
export RA2_DIR="/path/to/your/Red Alert 2"
RA2_TARGET=game bash User/games/ra2-native/retail-lift.sh "$RA2_DIR"
# 如果同一目录含有 gamemd.exe，可同时构建资料片
bash User/games/ra2-native/build.sh yr-recomp
python3 User/games/ra2-native/install.py "$HOME/Library/Application Support/LixinOS/daily.img" --retail "$RA2_DIR"
```

`yr-recomp` 启动《尤里的复仇》。它有独立的构建目录和软件包。两款游戏共用零售素材，资料片的设置和存档写入 `/scratch/yr-profile`。

## 验证范围

以下脚本对临时克隆盘启动 QEMU，检查上游版本、资源页与鼠标退出、PE 导入绑定、DirectDraw 和重编译入口的返回值。

```sh
python3 scripts/test-ra2-native.py --kernel rust --disk /path/to/test.img
python3 scripts/test-ra2-native.py --kernel tcc --disk /path/to/test.img
```

为命令添加 `--retail "$RA2_DIR"`，脚本还会导入本地素材、映射零售 PE，并在 RA2 Engine 中进入遭遇战、选中盟军基地车、展开建造场。Rust 和 TCC 内核均通过这场测试，地图为 `xmp18s3.map`，两次的最终游戏状态哈希均为 `b847ee78c25b9020`。

为测试命令添加 `--recomp-skirmish` 可用 QEMU 鼠标依次点击原版主菜单、单人游戏和遭遇战按钮。测试会等待引擎加载地图，并确认模拟推进到至少第 100 帧。这项测试需要已安装的《红色警戒 2》零售素材和从 `game.exe` 生成的程序。

重编译版已在 Rust 和 TCC 内核分别从原版繁体中文菜单进入 `MP02T2.MAP` 遭遇战，两个内核均通过第 100 帧检查，并在 QEMU 画面上显示部队和操作侧栏。长时间对局、完整战役和联机尚未验证。SDL 音频使用静音后端，重编译版会跳过 Bink 过场。

资料片可用同一个测试脚本加 `--recomp-skirmish yr` 验证。Rust 和 TCC 内核均已从 `gamemd.exe` 的繁体中文主菜单载入「火線衝突」地图 `XMP29U2.MAP`，画面显示部队和操作侧栏，模拟均推进超过 100 帧。
