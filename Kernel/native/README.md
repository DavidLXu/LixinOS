# 在 LixinOS 内用 TCC 重建内核

此版本在 Apple Silicon Mac 上的 QEMU AArch64 `virt` 中运行，使用一个 vCPU 和 1536 MiB RAM。内核、板级驱动和磁盘引导程序均来自 C 或 AArch64 汇编源码。网络使用 lwIP 的 C 源码。两个编译清单随磁盘安装在 `/src/kernel`。

## 启动和更新

在项目根目录执行以下命令。原版启动器和新启动器使用各自的磁盘。

```sh
./scripts/run-tcc-kernel.sh --window
```

`--split` 打开双终端，Ctrl+Tab 切换输入焦点。`--serial` 使用当前终端，`--terminal` 打开 Mac 终端，`--ssh` 使用现有的 Mac SSH 控制台桥。Bash 中输入 `q` 并回车即可关机。

在虚拟机中修改源码后执行以下命令。编译成功会将新的 ELF 写入双快照磁盘，下次启动时引导程序读取该文件。编译报错时，已安装的 ELF 继续可用。

```sh
tcc --kernel @/src/kernel/sources -o /boot/kernel.elf
q
```

宿主目录中的 `kernel.elf` 是制盘时导出的初始副本。QEMU 的启动参数加载固定的 `loader.elf`，后者从磁盘加载 `/boot/kernel.elf`。引导程序也可以在系统内用 `tcc --bootloader @/src/kernel/loader-sources -o /boot/loader.elf` 重建。更换 QEMU 使用的引导程序需要单独导出这个文件。

## 从零制盘

选取空目录保存三个输出。首次制盘通过原版 LixinOS 安装用户程序、源码和编译环境，随后在虚拟机内生成引导程序和内核。 使用旧版引导程序的磁盘继续以原有布局启动。要使用 100 MiB 文件，可先用 Rust 内核启动旧磁盘的副本以迁移布局，再在副本中重建内核并导出新版引导程序。以下脚本先验证自举、重启和图形输入，再保存产物。

```sh
mkdir -p data/tcc-os
python3 scripts/test-native-kernel.py \
    --save-elf data/tcc-os/kernel.elf \
    --save-bootloader data/tcc-os/loader.elf \
    --save-disk data/tcc-os/system.img
```

脚本会在新内核上重编 TCC，再用第二代编译器重建内核和引导程序，逐字节比较两代产物。它还会修改内核启动文字，写盘，关机，再从同一个引导程序启动并检查新文字。已有用户态 ELF 和 LXE1 程序沿用系统提供的发行版本。测试另外在新系统内编译 MicroPython、C 示例和图形按钮程序。

## 与原系统的兼容情况

已通过原有测试的功能包括用户态 Bash 和系统内替换 Bash、抢占调度、后台作业、管道、文件重定向、Git 提交与导出、原生 TCP 和 SSH 客户端、SSH 控制台重连、双终端、桌面拖动与最小化、剪贴板、窗口缩放、编辑器和画图。文件系统接受 LIXDISK4 磁盘上的 LIXINFS1、LIXINFS2 和带时间戳的 LIXINFS3 快照。容量回归在系统内创建一百万个文件，并在重启后检查每个名字。

可以让现有回归使用新内核和独立测试盘。每次测试都会从指定镜像复制一份工作副本。

```sh
export LIXINOS_KERNEL="$PWD/data/tcc-os/loader.elf"
export LIXINOS_TEST_SEED="$PWD/data/tcc-os/system.img"
python3 scripts/test-core.py
python3 scripts/test-desktop.py
python3 scripts/test-terminal.py
python3 scripts/test-resize.py --split
python3 scripts/test-native-agent.py
python3 scripts/test-million-files.py
```

原生 QEMU 窗口和分屏终端使用与 Rust 内核相同的 Menlo 灰度字形。每个 ASCII 字符占 22 × 44 个物理像素。窗口大小改变时，终端重新计算行数和列数。串口模式使用宿主终端的字体设置。

## Agent

在 Bash 中运行 `mkdir /work` 和 `agent chat /work` 可以进入项目对话。模型桥沿用启动器的 DeepSeek 配置。QEMU 通过独立的 virtio 端口与宿主桥通信，密钥保存在 Mac 上。回归测试使用本地模拟模型。

对话、工具调用和审批在用户态运行。前端位于 `/src/agent/native.py`，项目策略程序为 `/bin/agent-run`，可以在系统内用 `tcc /src/agent/run.c -o /bin/agent-run` 替换。内核提供模型传输、隔离测试与事务提交。隔离测试使用 MicroPython，单例限时两秒，输出上限为 2048 字节。

`agent inspect` 读取系统指标和文件元数据。`agent run` 暂存项目脚本，通过测试后显示预览并等待批准。`agent system` 可暂存系统文件，并在系统内用 TCC 编译候选程序。输入 `agent system-rollback` 可撤销最近一次已批准的系统改动。脚本版本记录支持跨重启回滚，重命名脚本时版本记录随文件迁移。

在 `agent chat` 中，Bypass 默认开启，本地命令可以直接改动工作目录内的文件。输入 `/bypass` 可切换为逐次询问。暂存脚本的保存和交互终端接管都需要确认。终端接管支持 Bash、Python、Nano 和 Kilo，命令输出通过管道返回。

TCC 内核目前使用针对单 vCPU、1536 MiB 的物理内存布局。启动器保留独立的 Rust 和 TCC 磁盘。
