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
