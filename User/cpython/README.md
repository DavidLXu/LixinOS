# LixinOS 的 CPython

在 Rust 原版内核或 TCC 自举内核的 Bash 中运行 `python3`，进入 CPython 3.14.5。按 Ctrl+C 可以中断正在执行的 Python 循环，按 Ctrl+D 返回 Bash。`python` 使用系统原有的 MicroPython。

```sh
python3 --version
python3 -c "import sys; print(sys.implementation.name, sys.platform)"
echo 'def square(x): return x*x' > helper.py
echo 'from helper import square' > demo.py
echo 'print(square(12))' >> demo.py
python3 demo.py
```

两个内核运行同一套静态链接的 AArch64 用户态程序。普通文件可以在写模式和读写模式下定位、覆盖、截短或扩展，扩展的字节读为零。

普通 Python 脚本可以用 `socket.create_connection()` 解析域名并建立出站 IPv4 TCP 连接。`ssl` 使用静态链接的 OpenSSL 和随系统安装的根证书，`urllib.request.urlopen()` 可以验证服务端证书和主机名。UDP、IPv6 和 TCP 服务端监听仍待适配。

配对发布中的新版运行时也支持本机 `AF_UNIX` 流套接字。应用可以使用
`socketpair()`，或绑定地址后由另一进程连接。
[原生桌面会话](../../docs/native-desktop-sessions.md#unix-域流套接字)

`threading.Thread` 在同一进程内共享 Python 对象。每个线程有独立的栈和线程局部存储，在解释器检查点和已适配的阻塞调用处协作调度。长时间占用 CPU 的 C 扩展可能推迟线程切换。`subprocess.run()` 和 `Popen` 使用内核的进程、等待和管道接口，支持参数、工作目录、标准输入以及分别捕获标准输出和标准错误。可用 `Popen.kill()` 终止直接子进程。自定义子进程环境和任意描述符继承仍待支持。

## 在系统里用 pip

`pip`、`pip3` 和 `python3 -m pip` 都运行 CPython 3.14.5 随附的 pip 26.1.1。在任一内核的 Bash 中可以运行以下命令。

```sh
pip install six==1.17.0
python3 -c "import six; print(six.__version__)"
pip show six
pip check
pip uninstall -y six
```

pip 通过系统的 TCP 和 BearSSL 访问 PyPI，校验证书和主机名，在虚拟机内解析依赖、下载和解压 wheel。包安装在当前系统盘的 `/lib/python3.14/site-packages`。已有 wheel 可以离线安装，例如 `pip install --no-index /tmp/six-1.17.0-py2.py3-none-any.whl`。有多个依赖时，可以把它们放在同一目录，通过 `--no-index --find-links /tmp` 安装。

当前支持与这套标准库兼容的纯 Python wheel。当前格式的磁盘上每个文件的上限为 100 MiB，这个限制同时适用于下载的 wheel 和解压出的单个文件。pip 默认只选 wheel，关闭下载缓存和进度动画。第三方源码包构建和动态加载原生扩展仍待移植。pip 本身随系统更新，以保留 LixinOS 的网络适配。

## 构建和安装

项目的用户程序构建脚本在 Mac 上校验 CPython、musl、zlib 和 OpenSSL 源码包，构建用于生成冻结模块的宿主 Python，再交叉编译目标解释器。缓存目录为 `Kernel/target/user-build/cpython-port`。当前版本依赖宿主交叉工具链构建，运行脚本和导入模块都在虚拟机内执行。

Rust 启动器通过 QEMU 的 `fw_cfg` 提供首次安装包。空盘安装和从上一版发行的 CPython 升级都会把 pip、解释器和库一起写入一个磁盘快照。重新启动会沿用盘上的安装。按 [TCC 系统制盘步骤](../../Kernel/native/README.md) 制作的新盘也包含同一套文件。

`/bin/python3` 是加载程序。它从 `/lib/cpython-3.14.5` 加载并校验解释器的各段，每个文件最多 1 MiB。内核随后把代码页切换为只读可执行，把数据、栈和堆保留为可写且不可执行。纯 Python 模块位于 `/lib/python3.14`。完整备份可直接复制关机后的系统盘。

在项目根目录运行 `python3 scripts/test-cpython.py`，可以在临时磁盘上验证两个内核的脚本、交互、导入、异常回溯、文件更新的持久化、Ctrl+C 后继续交互、线程同步、子进程管道、HTTPS 证书校验，以及本地 wheel 的安装、导入和卸载。
