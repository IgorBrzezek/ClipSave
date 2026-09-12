#!/bin/bash
# Build a static libwebp (libwebp.a + libsharpyuv.a) from source into
# ./libwebp-1.4.0/build.  Run inside MSYS2 UCRT64.
# Requires: cmake, make, tar  +  libwebp-1.4.0.tar.gz (bash dl_webp.sh).
export PATH="/usr/bin:/bin:/ucrt64/bin:$PATH"
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR" || exit 1

if [ ! -f libwebp-1.4.0.tar.gz ]; then
    echo "ERROR: libwebp-1.4.0.tar.gz not found in $DIR"
    echo "Fetch it first:  bash dl_webp.sh"
    exit 1
fi

tar xzf libwebp-1.4.0.tar.gz || exit 1
cd libwebp-1.4.0 || exit 1
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
  -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
  -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
  -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF \
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_USE_THREAD=ON > cmake_cfg.log 2>&1
echo "cfg rc=$?"
cmake --build build --config Release -j4 > cmake_build.log 2>&1
echo "build rc=$?"
tail -3 cmake_build.log
echo "--- archive files ---"
ls -la build/libwebp*.a build/libsharpyuv*.a 2>/dev/null