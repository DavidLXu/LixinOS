# LixinOS 系统手册

LixinOS 是一个用 Rust 编写的 ARM64 操作系统，在 Apple Silicon Mac 的 QEMU 虚拟机里运行。你可以在系统里编辑 Python 和 C 源码、运行脚本、编译程序，再把程序保存在虚拟磁盘上。当前版本为 [0.0.1](0.0.1.md)。

## 从终端开始

在 Mac 的项目根目录运行 `./scripts/run-kernel.sh`，等到窗口出现 `lixin:/$` 后，就可以输入 guest 命令。首次构建会下载交叉编译工具和上游源码。想直接在当前 Mac 终端操作，可以用 `./scripts/run-kernel.sh --serial`。安装步骤和其他启动模式见[项目 README](../README.md)。

| 想做什么 | 在 LixinOS 里怎么做 |
|---|---|
| 写一个 Python 脚本 | 用 `nano hello.py` 输入 `print(6 * 7)`，按 Ctrl+O 保存、Ctrl+X 退出，再执行 `python hello.py` |
| 临时试一段 Python | 输入 `python` 进入 MicroPython，在空白的 `>>>` 提示符按 Ctrl+D 回到 shell |
| 编译并保留 C 程序 | 用 `nano hello.c` 编写源码，执行 `tcc hello.c -o hello`，再用 `./hello` 运行；只想试跑一次可用 `tcc -run hello.c` |
| 查看文件或系统状态 | `ls -lah` 显示隐藏文件、类型和大小；`htop` 显示内核堆和文件系统统计，按 q 返回 |
| 找回上一次的命令 | 按 ↑ 或 ↓ 浏览最近 64 条历史；Tab 补全命令或路径，连续按两次可列出候选 |
| 关机并保留工作 | 输入 `shutdown`。再次启动时使用同一块磁盘，文件、编译好的程序和命令历史就会恢复 |

## 输入一条命令后，谁在工作？

以 `python hello.py` 为例，shell 先按当前目录找到脚本，把文件内容交给用户态的 MicroPython。解释器在 ARM64 的 EL0 执行 Python 代码，调用内核的终端接口输出结果。脚本结束后，控制权回到 shell。内核本身运行在 EL1，负责页表、异常处理和设备访问。[Shell](../Kernel/src/shell.rs) · [用户程序加载器](../Kernel/src/user.rs) · [MicroPython 移植层](../User/python/port.c)

启动器为 guest 配置一个虚拟 CPU 和 128 MiB RAM。内核使用 8 MiB 堆，每个终端窗格为前台用户程序保留 4 MiB 区域，代码页与可写数据页使用不同权限。用 `--split` 启动时，两个窗格共享磁盘，各自保存工作目录、变量和前台程序状态；Ctrl+Tab 暂停当前窗格并恢复另一个。读 `htop` 时应把堆用量看作整个 guest 内存的一部分。[内核入口](../Kernel/src/main.rs) · [分屏实现](../Kernel/src/panes.rs)

## 重启后，哪些东西还在？

默认磁盘是 Mac 项目目录下的 `data/lixinos.img`，容量 8 MiB。内核在内存中维护目录树和文件内容。保存文件时，它先写另一份快照的数据并刷盘，再写入带校验信息的头部；启动时选择最新的有效快照。命令历史单独存放在磁盘上。当前上限是 128 个条目（含根目录），单个文件 16 KiB。Python 交互变量和正在编辑但尚未保存的文字只存在于当前会话。[磁盘格式与恢复逻辑](../Kernel/src/disk.rs)

## 编译好的 C 程序是什么格式？

`tcc hello.c -o hello` 在 guest 里生成 ARM64 机器码，保存为 **LXE1**。文件以 16 字节头部开头，之后依次存放代码和初始数据。头部的前四个字节是字符 `LXE1`，其余三个字段都是小端 32 位整数，依次表示 `main` 在代码段内的字节偏移、代码长度和数据长度。

运行 `./hello` 时，加载器校验头部、文件长度和入口范围，装入代码、恢复初始数据，再通过用户态运行库调用 `main(void)`。版本 1 为 `puts`、`printf` 和 `putchar` 约定了固定入口，因此源码删除后，保存的程序仍然可以执行。维护旧程序兼容性时，除了文件头，还要保留这些调用约定。**0.0.1 是系统版本，LXE1 中的 1 是可执行文件格式版本。** [TinyCC 写入端](../User/tcc/port.c) · [加载端](../Kernel/src/user.rs) · [运行库入口](../User/tcc/runner.c)

可选的 `agent` 命令借助 Mac 端桥接调用模型服务，生成脚本后在 guest 中验证。编辑、编译和运行本地程序可以独立使用。桥接配置和调用示例见[项目 README](../README.md#generate-a-script-with-the-agent)。
