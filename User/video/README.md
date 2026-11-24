# LixinOS 视频播放器

从 **Apps → 工具 → 视频播放器** 打开。首次看到的热气球短片位于 `/lib/video/demo.mp4`，全长 12 秒，采用 H.264 编码。

- 在 Dock 的 **Files** 中双击视频可以直接播放。在播放器中按 **Ctrl+O** 或点击顶部路径栏，输入本地文件路径后按回车。
- **空格**或底部按钮切换播放和暂停。**← / →** 后退或前进五秒。点击进度条可跳到指定位置，播完后点「重播」从头播放。
- 拉伸或最大化窗口时，画面保持原有宽高比。当前版本只输出画面。

播放器在客户机内使用 FFmpeg 7.1.5 解封装和解码，由原生桌面窗口呈现。已编入 MP4/MOV、Matroska/WebM、AVI、MPEG-PS 容器，以及 H.264、MPEG-4 Part 2、MPEG-1/2 Video、VP8、VP9 和 MJPEG 解码器。能否播放取决于文件实际编码，例如采用 HEVC 编码的 MP4 目前无法播放。播放器读取本地文件。[FFmpeg 源码发布页](https://ffmpeg.org/download.html)

## 构建与安装

在项目根目录执行以下命令。安装器一次更新播放器、示例和负责文件关联的桌面程序，目标盘需要先关机。

```sh
bash User/video/build.sh
bash User/netsurf/build.sh
python3 User/video/install.py "$HOME/Library/Application Support/LixinOS/daily.img"
```

构建脚本校验上游源码的 SHA-256，使用 LixinOS 的 musl/GCC 工具链静态链接四个 FFmpeg 库。FFmpeg 的许可证随包安装到 `/lib/video/COPYING.LGPLv2.1`。播放器和桌面运行在用户态，同一份应用包可在 Rust 和 TCC 内核下使用。

在安装后的磁盘上，可运行 `python3 scripts/test-video.py --disk "$disk" --kernel rust`。将 `rust` 换成 `tcc` 即可验证另一内核。测试在临时磁盘副本上通过鼠标、键盘和 QEMU 帧缓冲检查实际播放。
