#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
work="$cache/video-port"
musl="$cache/cpython-port"
rt="${LIXINOS_GCC_BUILD:-$root/Kernel/target/gcc-port}/rt/liblx.a"
[[ -x "$musl/cc" && -f "$rt" ]] || bash "$root/User/gcc/build.sh" rt
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
pkg="$work/package"
mkdir -p "$work/build" "$pkg/bin" "$pkg/lib/video"
cd "$work"
version=7.1.5
if [[ ! -f ffmpeg.tar.xz ]]; then
    curl -fL --retry 3 "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz" -o ffmpeg.tmp
    mv ffmpeg.tmp ffmpeg.tar.xz
fi
echo 'de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f  ffmpeg.tar.xz' | shasum -a 256 -c -
[[ -f "ffmpeg-$version/configure" ]] || tar -xf ffmpeg.tar.xz
cd build
if [[ ! -f ffbuild/config.mak || "$port/build.sh" -nt ffbuild/config.mak ]]; then
    "../ffmpeg-$version/configure" --enable-cross-compile --cross-prefix=aarch64-none-elf- \
      --arch=aarch64 --target-os=none --cc="$musl/cc" --disable-autodetect \
      --disable-programs --disable-doc --disable-network --disable-pthreads --disable-runtime-cpudetect \
      --disable-everything --disable-avdevice --disable-avfilter --disable-postproc --enable-swresample \
      --enable-avformat --enable-avcodec --enable-swscale --enable-small --disable-debug \
      --enable-protocol=file --enable-demuxer=mov,matroska,avi,mpegps \
      --enable-decoder=aac,mp3,vorbis,opus,pcm_s16le,h264,mpeg4,mpeg1video,mpeg2video,vp8,vp9,mjpeg \
      --enable-parser=h264,mpeg4video,mpegvideo,vp8,vp9,mjpeg \
      --extra-cflags="-ffunction-sections -fdata-sections" --extra-ldflags="-static -Wl,-T,$root/User/rust/large.ld" \
      --extra-libs="-Wl,--start-group $rt -lc -lgcc -Wl,--end-group"
fi
make -j"${JOBS:-8}" libavformat/libavformat.a libavcodec/libavcodec.a libswscale/libswscale.a libswresample/libswresample.a libavutil/libavutil.a
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" crt1.o
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" -c "$root/User/cpython/loader.c" -o crt0.o
"$musl/cc" -std=gnu11 -Wall -Wextra -Os -ffunction-sections -fdata-sections \
  -I"$root/User/tcc" -I. -I"../ffmpeg-$version" -nostartfiles -static \
  -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 -Wl,-T,"$root/User/rust/large.ld" \
  crt0.o crt1.o "$port/player.c" "$port/audio-track.c" "$root/User/gui/han-font.c" \
  -Wl,--start-group libavformat/libavformat.a libavcodec/libavcodec.a libswscale/libswscale.a \
  libswresample/libswresample.a libavutil/libavutil.a "$rt" -lm -lc -lgcc -Wl,--end-group -o player
aarch64-none-elf-strip -o "$pkg/bin/video.win3" player
cp "$port/demo.mp4" "../ffmpeg-$version/COPYING.LGPLv2.1" "$pkg/lib/video"
