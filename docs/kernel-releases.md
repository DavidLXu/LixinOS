# 两种内核的发布与回滚

每次完成任一内核的迭代，在项目根目录发布一对内核。

```sh
python3 scripts/kernel-release.py publish
python3 scripts/kernel-release.py list
```

本地 Git 注解标签采用 `kernel-YYYY-MM-DD.N`。日期取发布机本地时间，序号从 1 开始。
发布器用独立 Git 索引保存当前工作区的项目源码（包括未提交且未被 Git 忽略的文件），在临时目录构建两种内核。
Rust 的串口、SSH、终端、窗口和分屏二进制分别归档。TCC 的 ELF 和引导程序由新启动的 LixinOS 从源码编译。
发布器启动 Rust 串口版和 TCC 版，核对启动横幅与 `uname -r`，成功后才创建标签。
标签指向的提交包含源码、二进制、SHA-256 清单、Rust 启动用的 CPython 包和新生成的 TCC 启动盘。
项目工作分支和暂存区可继续开发。

从虚拟机内部修改 TCC 后，用 `q` 保存并关机，再把磁盘传给发布器。它导入该盘的
`/src/kernel`（含 lwIP），同当前 Rust 源码配对，在新盘上重建。普通开发编译和虚拟机内的
临时编译通过上述发布命令登记为可回滚版本。

```sh
python3 scripts/kernel-release.py publish --tcc-source-disk data/tcc-os/system.img
```

从项目主目录运行 `./scripts/run-kernel.sh` 时，选择器显示最新的本地成对发布标签。
Rust 和 TCC 都会用该标签的 ELF 启动。启动器校验并按需缓存二进制。已初始化的「我的工作盘」优先；尚未初始化时才寻找
`data/shared.img` 和项目自带的磁盘。TCC 的发布模式直接加载发布的 ELF，盘内
`/boot/kernel.elf` 仍供自举模式使用。

双核版本默认启动两个 vCPU。Rust 和 TCC 在每个核心上独立调度。同一进程在其分配的核心上进入系统调用，管道读端可以等待另一核心的写端将它唤醒。共享内核服务通过一把公平锁串行访问，磁盘提交也按顺序执行。阻塞任务让出核心，空闲核心在等待时释放锁。单线程程序可以通过独立工作进程利用第二核。用 `cpuinfo` 查看已上线的核心数，`cat /proc/stat` 查看每核累计时间。用户程序可用 `console_call(29, 10)` 查询进入系统调用的核心编号。

要用同一内核与单核对照，可以运行 `LIXINOS_CPUS=1 ./scripts/run-kernel.sh --kernel rust --release latest --serial`，或把 `rust` 换成 `tcc`。

尚未设置工作盘时，如果 macOS 把共享盘移到了 iCloud，图形选择器会列出可用的本地桌面盘名称。
点击「启动本地桌面盘」后，本次使用该盘自己的文件和历史。要继续使用共享盘，
请在 Finder 中对 `data/shared.img` 选择「立即下载」，等下载完成后重新启动。
显式指定 `LIXINOS_DISK` 的命令始终使用指定的路径。

```sh
./scripts/run-kernel.sh --kernel tcc --source
LIXINOS_DISK=/path/to/copy.img ./scripts/run-kernel.sh --kernel rust --release latest --serial
```

`--source` 使用源码构建的 Rust 内核，或用引导程序加载盘内的 TCC 内核。
指定其他镜像时，可以加 `--release latest` 或 `--release TAG` 选择发布内核。
恢复目录默认使用该目录归档的一对内核。

回滚到一个新的目录，把示例标签替换为要运行的版本。

```sh
python3 scripts/kernel-release.py restore kernel-2026-11-19.1 ../LixinOS-restored
cd ../LixinOS-restored
./scripts/run-kernel.sh --kernel rust --serial
./scripts/run-kernel.sh --kernel tcc --serial
```

恢复命令从标签检出源码，校验二进制与解压后的 TCC 启动盘。恢复目录里的启动器直接使用归档的 ELF。
Rust 首次启动会创建独立系统盘。TCC 启动盘来自发布时的干净构建环境。交互中用 `uname -r`
查看标签与内核种类，用 `q` 回车关机。日常盘里的个人文件和单独的 Rust 自举大盘需要单独备份。
