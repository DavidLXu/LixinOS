# NetSurf 渲染修复记录：苹果中国首页

2026 年 11 月 21 日，在 LixinOS 的苹方桌面镜像中打开苹果中国首页时，iPhone 主图一度空白，顶部导航盖住了标题，两个操作链接也被画成上下叠放的蓝色方框。修复集中在 NetSurf 3.11 的图片选择、CSS 解析和排版绘制。Rust 和 TCC 内核运行同一份用户态浏览器程序。

![修复后的 QEMU 桌面。顶部导航横排，iPhone 标题下的两个圆角按钮并排居中，产品图可见。](screenshots/netsurf-apple-cn-layout.png)

## 四处显示问题

| 页面现象 | 原因与修复 |
| --- | --- |
| iPhone 主图空白 | 首页在 `<picture>` 中放了带 `data-empty` 的内嵌 GIF。它先匹配了屏幕尺寸，浏览器就停止选图。现在 NetSurf 把这种 GIF 留作兜底，继续检查后续匹配的 `<source>`，再检查 `<img>` 的 `srcset` 和 `src`。 |
| 两个操作链接都有蓝色方框 | 按钮类通过 CSS 变量设置背景、文字和边框颜色，绘制端还需要处理圆角。现在“进一步了解”显示为蓝底白字，“选购 iPhone”保留一圈细蓝边，普通文字链接沿用自己的样式。 |
| 两个按钮上下堆叠 | 官网用 `:has(:nth-child(2))` 判断是否启用双列 Grid。NetSurf 现在能匹配这种选择器，用 `min-content` 算出两列各自所需的宽度，再把两列和中间的间距一起居中。按钮的水平、垂直内边距也计入尺寸。 |
| 顶部菜单竖排并盖住标题 | 导航的多层容器使用 `display: contents` 或继承这个值。现在子项仍从容器继承样式，同时直接参与外层 Flex 布局。`justify-content: space-between` 把剩余宽度分到菜单项之间，导航可以在一行内铺开。 |

图片候选的判定在 [picture.inc](../User/netsurf/picture.inc)。按钮涉及[样式表变量展开](../User/netsurf/root_lengths.c)和[圆角绘制](../User/netsurf/rounded.inc)。列宽与居中的计算在 [grid_layout.inc](../User/netsurf/grid_layout.inc)，选择器和布局树的改动分别在 [libcss 补丁](../User/netsurf/libcss.patch)和 [NetSurf 补丁](../User/netsurf/lixinos.patch)。

## 如何复测

**看实际网页。** 使用装有修复版 NetSurf 的 `data/desktop-pingfang.img` 启动 QEMU 图形桌面，在 LixinOS 的 Bash 中运行：

```sh
netsurf https://www.apple.com.cn/
```

最大化窗口，等产品图载入后，对照上图检查导航、按钮和主图。截图记录的是当日的桌面宽度布局；官网内容会随时间变化。

**运行固定样例。** 在 Mac 的项目根目录执行下面两条命令。测试脚本为每个内核启动一份临时磁盘副本，通过本地 HTTP 服务提供固定页面，并读取 QEMU 帧缓冲的实际像素。这两组回归在本次修复时均已通过。

```sh
python3 scripts/test-netsurf.py data/desktop-pingfang.img --kernel rust --release kernel-2026-11-20.5
python3 scripts/test-netsurf.py data/desktop-pingfang.img --kernel tcc --release kernel-2026-11-20.5
```

| 测试页面 | 检查内容 |
| --- | --- |
| `/hero` | 在真实图片前放入透明占位 GIF，检查横幅中央是否出现真实图片的色带。 |
| `/link-borders` | 分别取样主按钮的填色、次按钮的细边、圆角外露出的页面底色，以及普通链接的文字。 |
| `/navigation-layout` | 检查嵌套导航项是否同排且继承颜色，再检查双列按钮的左右边界、内边距、列间距和居中位置。 |

这里验证的 CSS 范围包括 `:has(:nth-child(An+B))` 这种后代条件、水平书写模式下单值的逻辑内边距，以及四角同半径的纯色按钮。完整测试见 [QEMU 回归脚本](../scripts/test-netsurf.py)，浏览器的构建与安装见 [NetSurf 移植说明](../User/netsurf/README.md)。
