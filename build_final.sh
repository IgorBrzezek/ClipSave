#!/bin/bash
# Build ClipSave (ANSI C + WebP) and package a Windows release zip.
# Run inside MSYS2 UCRT64 on Windows, e.g.:
#   bash build_final.sh
# Requirements: mingw-w64-ucrt-x86_64-gcc + static libwebp (build_webp_lib.sh)
# W=$PWD/libwebp-1.4.0 -> override with LIBWEBP_DIR
export PATH="/usr/bin:/bin:/ucrt64/bin:$PATH"
cd "$(dirname "$0")"

W="${LIBWEBP_DIR:-$PWD/libwebp-1.4.0}"
if [ ! -f "$W/build/libwebp.a" ]; then
    echo "ERROR: static libwebp not found at $W/build/libwebp.a"
    echo "Build it first with: bash dl_webp.sh && bash build_webp_lib.sh"
    exit 1
fi

# 1) Compile. webp_save.c wraps libwebp (VP8 lossy / VP8L lossless);
#    libwebp.a + sharpyuv are linked statically -> exe needs no DLLs.
gcc -O2 -municode clipsave.c webp_save.c -o clipsave.exe \
  -I"$W/src" -L"$W/build" -lgdiplus -lgdi32 -lole32 -luuid \
  -lwebp -lsharpyuv -lpthread -lm || exit 1

# 2) Pack the standalone binary for distribution (GitHub Releases).
#    Prefer python's zipfile (bsdtar -a writes a mislabeled tar; stock
#    UCRT64 ships no zip).  Fall back to PowerShell Compress-Archive.
mkdir -p release
rm -f release/clipsave-win64.zip
if python -m zipfile -c release/clipsave-win64.zip clipsave.exe 2>/dev/null; then
  echo "zip: created by python -m zipfile"
elif powershell -NoProfile -Command \
     "Compress-Archive -Path clipsave.exe -DestinationPath release/clipsave-win64.zip -Force" \
     2>/dev/null; then
  echo "zip: created by PowerShell Compress-Archive"
else
  echo "WARNING: no zip tool found; distribute release/clipsave.exe directly"
fi

echo "OK:"
ls -la clipsave.exe release/clipsave-win64.zip
python -m zipfile -l release/clipsave-win64.zip 2>/dev/null || true