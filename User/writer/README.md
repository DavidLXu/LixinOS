# LibreOffice Writer

本目录使用 LibreOffice 25.2.7.2 上游源码。Writer 的文档模型、排版、Word 导入导出和 PDF 导出由上游实现。Win3 前端通过 LibreOfficeKit 绘制文档，并把键盘和鼠标输入交给 Writer。前端使用 LixinOS 已有的 GTK 3。

在项目根目录交叉构建。Mac 端需要 Xcode、GNU Make、Bison、Flex、gperf、NASM、Autoconf 和 Automake。源码包的 SHA-256 记录在 `source.sha256` 中。配置阶段分开选择 Mac 工具和 AArch64 来宾依赖，构建目录位于 `Kernel/target/user-build/libreoffice`。

```sh
bash User/writer/configure.sh
bash User/writer/build.sh
```

单独重建 Writer 核心模块可以运行 `bash User/writer/build.sh Library_sw`。该目标生成 `guest/instdir/program/libswlo.a`。完整应用还需要可执行文件、UNO 服务与类型注册表、配置和界面资源。打包脚本会检查这些运行时文件。

Writer 使用系统里的 GTK 字体。可执行映像、UNO 注册表、用户界面资源和首次启动配置一起打包在 `Kernel/target/user-build/libreoffice/package`。应用安装在 `/bin/writer.win3`，终端命令为 `writer`，用户配置保存在 `/home/lixin/.config/lixinos-writer`。

窗口提供新建、打开、保存、另存为、导出 PDF、撤销和重做。第二行是排版工具栏：段落样式（正文、标题 1-3 等）、字体、字号、粗体、斜体、下划线、文字颜色、四种对齐方式和项目符号。工具栏会跟随当前选区更新；选择文字后可以直接修改，也可以先把插入点放进已有标题再套用样式。

`Ctrl+F` 打开查找栏，`Ctrl+H` 把焦点移到替换框；回车执行“查找下一个”或“替换当前匹配”，`Tab` 可在查找和替换控件之间移动。`Ctrl+B`、`Ctrl+I`、`Ctrl+U` 分别是粗体、斜体和下划线。保存格式为 ODT 或 DOCX。终端可以调用上游的无窗口转换功能。

这一轮接入的是文字和段落排版。图片、表格、页边距、批注和修订仍没有前端入口；打开包含这些内容的文档时，排版引擎仍会显示它们。

```sh
writer --version
writer --headless --convert-to pdf --outdir /home/lixin /home/lixin/report.docx
```

在宿主机运行 `python3 scripts/test-writer.py` 可启动隔离的测试盘，分别在 Rust 和 TCC 内核中验证查找替换、段落样式、字体、字号、下划线、居中对齐、DOCX 保存和文档导出。PDF 的文字校验使用 Poppler 的 `pdftotext`。
