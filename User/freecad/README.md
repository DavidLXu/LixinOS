# FreeCAD on LixinOS

上游 FreeCAD 1.1.4 的 Qt 主窗口、Sketcher 和 Part 工作台已在 LixinOS 的 Rust 和 TCC 内核上运行。两套内核均通过原生 `.FCStd` 保存和重开测试。宿主交叉编译，建模、渲染和文件读写都在 ARM64 客体完成。

## 已验证的部分

| 客体测试 | 实际执行的代码 | 判定条件 |
| --- | --- | --- |
| 建模 | FreeCAD 的 PlaneGCS、Part 布尔运算和 OCCT 7.9.2 | 求解三角形约束，编辑带孔零件，重启后重读 BRep 并校验体积 |
| 三维视图依赖 | Mesa 24.3.4 softpipe、GLU、Coin 4.0.3 的 `SoGLRenderAction` | 在 EGL 兼容模式上下文绘制立方体，读取中心绿色和背景蓝色像素 |
| Qt 控件 | 上游 `src/Gui/SplitButton.cpp` 和 Qt 6.8.3 的 offscreen、Wayland 插件 | 按钮发出一次点击信号，切换状态，前后两帧图像不同 |

Wayland 测试启动客体 Mutter，通过 Qt 的 `isExposed()` 检查窗口显示，再通过 Qt API 点击按钮。`--view` 在 `QOpenGLWidget` 中绘制 Coin 场景，检查像素并等待 Qt 发出 `frameSwapped`，测试端随后关闭虚拟机。`--desktop` 另外测试从 FreeCAD 主窗口正常退出。

## 启动原版桌面

在装好 FreeCAD 的 LixinOS 日常盘中，从图形终端直接输入：

```sh
freecad
```

也可以输入 `freecad /路径/零件.FCStd`。在图形终端，命令会创建专用 Wayland 会话；在已有 Wayland 桌面中，它会连接当前会话。`startfreecad` 也可使用。用工作台下拉框选择 **Sketcher** 或 **Part**，用 **File → Save As** 保存模型。

## 构建和复验

在仓库根目录运行。沿用本项目的 ARM64 GNU 工具链、CPython、GTK3、GNOME 用户态 SDK 和 Mesa 构建环境。Qt 使用同版本宿主 SDK 里的 moc 等代码生成器，目标库单独交叉编译。将 `QT_HOST_PATH` 指向宿主 Qt 6.8.3，配置整个应用时还需要宿主 Qt Linguist 工具、SWIG 和 pybind11。

```sh
bash User/freecad/build.sh
bash User/freecad/build-coin.sh
bash User/freecad/build-qt-widget.sh

python3 scripts/test-freecad.py --release kernel-2026-11-24.4
python3 scripts/test-freecad.py --release kernel-2026-11-24.4 --coin
python3 scripts/test-freecad.py --release kernel-2026-11-24.4 --qt-widget
```

在已有建模工具包的客体里，`freecad-sketch-probe 6 5` 运行约束求解器，`freecad-part.win3` 启动一个调用 Part 运算的 Win3 小程序。该程序保存 OCCT 的 `.brep` 文件。

## 上游桌面入口

```sh
bash User/gnome/build-mutter.sh
bash User/freecad/build-desktop-deps.sh
bash User/freecad/build-qt-modules.sh
bash User/freecad/build-qt-wayland.sh
bash User/freecad/build-wayland-egl.sh
bash User/freecad/build-occt-desktop.sh
bash User/freecad/build-pivy.sh
bash User/freecad/build-shiboken.sh
bash User/freecad/build-pyside.sh
bash User/freecad/build-desktop.sh
```

Shiboken 的宿主生成器要求 6.8.3。可以用构建目录内的 Python 环境安装 `shiboken6-generator==6.8.3` 和 `shiboken6==6.8.3`，或者用 `SHIBOKEN_HOST_GENERATOR` 指向同版本生成器。

最后一条配置固定版本的上游源码，打开 GUI、Material、Part 和 Sketcher，然后编译上游可执行目标。构建目录为 `Kernel/target/user-build/freecad-port/original-desktop`。宿主 Python 运行代码生成器，ARM64 的 Python 头文件和静态库供目标程序使用。脚本会报告缺失的依赖。原版程序目标名为 `FreeCADMain`。

Qt 使用客体管道和 futex 唤醒事件循环。裸机交叉工具链为 C++ 线程局部变量生成的访问函数由 musl 的 pthread 存储实现。窗口内三维渲染使用 Mesa 的 Wayland EGL 后端，静态库和 Qt 着色器资源随程序链接。

上游的 FreeCADBase、FreeCADApp、FreeCADGui 和所选工作台编译为静态库。Python 在启动前注册 Materials、Part、Sketcher、Measure 和对应 GUI 扩展。工作台的 C++ 全局构造函数延迟到模块导入时运行。Pivy 的 Coin 包装器由 SWIG 生成，Qt 的 Python 包装器由 Shiboken 生成。

构建产物位于 `Kernel/target/user-build/freecad-port/original-desktop/bin/FreeCAD`。打包脚本生成客体入口 `/bin/freecad`，将资源与私有 Python 运行库放到 `/lib/freecad-desktop`。整程序测试从种子盘创建独立的大内存构建卷，启动 Mutter，再运行宏。普通盘的进程内存曾在初始化工作台时耗尽。

```sh
python3 scripts/test-freecad.py --release kernel-2026-11-24.4 --wayland
python3 scripts/test-freecad.py --release kernel-2026-11-24.4 --view
python3 scripts/test-freecad.py --release kernel-2026-11-24.4 --desktop
python3 scripts/test-freecad.py --release kernel-2026-11-24.4 --prepare-desktop Kernel/target/user-build/freecad-port/LixinOS-FreeCAD-Launcher.img
python3 scripts/test-freecad.py --release kernel-2026-11-24.4 --launcher --disk Kernel/target/user-build/freecad-port/LixinOS-FreeCAD-Launcher.img
```

最终验收在真实主窗口中执行 [test-workbenches.FCMacro](test-workbenches.FCMacro)。它依次激活 Sketcher 和 Part，创建带半径约束的圆和一个长方体，保存临时 `.FCStd`，关闭并重新打开文档，检查半径和体积。两套内核都从重新打开的文件中读出了 5 mm 半径和 288 mm³ 体积。另在 Rust 内核的 QEMU 图形窗口中，通过虚拟键盘向 FreeCAD 的 Qt 文本框输入了 `cad`。
