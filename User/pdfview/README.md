# LixinOS PDF 阅读器

在桌面应用菜单选择「PDF 阅读器」，或在桌面文件管理器中双击 PDF。窗口使用 GTK 3 / Win3，Xpdf 4.05 在来宾进程中解析文件并通过 Splash 和 FreeType 绘制每一页。嵌入的字体直接从内存加载。没有嵌入字体的文档会使用系统字体替代。

| 操作 | 方式 |
| --- | --- |
| 打开文件 | 点击「打开…」，或按 Ctrl+L 输入路径后回车 |
| 切换页 | 点击「上一页」「下一页」，使用页码框，或按 Page Up / Page Down |
| 找回某一页 | 左侧缩略图侧栏按页显示页面版式，点击缩略图直接跳转 |
| 收起侧栏 | 点击「缩略图」，或按 Ctrl+B；再次点击或按键重新展开 |
| 调整大小 | 点击 −、+ 或「适合宽度」 |
| 阅读长页面 | 使用鼠标滚轮或滚动条 |

阅读器会在 `/home/lixin/.pdfview-state` 记录每份文档最后停留的页码和滚动位置。重新打开同一路径的 PDF 时自动回到上次读到的位置。

在项目根目录运行以下命令可以重新构建并验证。测试会用 ReportLab 生成两页 PDF，在两种内核下检查实际窗口的像素变化、缩略图侧栏的收起与展开，以及关闭后重新打开时的页码恢复。

```sh
bash User/pdfview/build.sh
python3 scripts/test-pdfview.py
```

来宾命令行也可以按页导出 PNG。页码从 1 开始。

```sh
pdfview.win3 --render /home/lixin/book.pdf 1 /home/lixin/page.png
```
