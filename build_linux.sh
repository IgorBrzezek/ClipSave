#!/bin/bash
# ClipSave cross-compile: Linux -> Windows clipsave.exe
# Requires sudo (installs the MinGW-w64 toolchain and builds libwebp for win64).
# Usage: bash build_linux.sh
set -e
cd "$(dirname "$0")"

echo "== [1/4] Install MinGW-w64 toolchain and build tools =="
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    gcc-mingw-w64-x86-64 cmake make pkg-config git

echo "== [2/4] Fetch libwebp =="
if [ ! -d libwebp ]; then
    git clone --depth 1 -b v1.4.0 https://github.com/webmproject/libwebp
fi

echo "== [3/4] Build libwebp x86_64-win64-gcc (static) =="
cd libwebp
cmake -B build \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_BUILD_TYPE=Release \
    -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
    -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
    -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
    -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF \
    -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_USE_THREAD=ON
cmake --build build --config Release -j"$(nproc)"
cd ..

echo "== [4/4] Compile clipsave.exe =="
x86_64-w64-mingw32-gcc -O2 -municode clipsave.c webp_save.c \
    -Ilibwebp/src -Llibwebp/build -lgdiplus -lgdi32 -lole32 -luuid \
    -lwebp -lsharpyuv -lpthread -lm -o clipsave.exe

echo "OK: $(pwd)/clipsave.exe"
ls -la clipsave.exe