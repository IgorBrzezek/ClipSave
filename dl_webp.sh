#!/bin/bash
# Download the libwebp source tarball next to this script (needed by
# build_webp_lib.sh).  Run inside MSYS2 UCRT64; requires curl or wget.
export PATH="/usr/bin:/bin:/ucrt64/bin:$PATH"
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR" || exit 1

URL="https://github.com/webmproject/libwebp/archive/refs/tags/v1.4.0.tar.gz"
OUT="libwebp-1.4.0.tar.gz"

if command -v curl >/dev/null 2>&1; then
  curl -sL -o "$OUT" "$URL"
  rc=$?
elif command -v wget >/dev/null 2>&1; then
  wget -q -O "$OUT" "$URL"
  rc=$?
else
  echo "ERROR: neither curl nor wget found."
  echo "Download $URL manually into $DIR as $OUT"
  exit 1
fi

echo "download rc=$rc size=$(stat -c%s "$OUT" 2>/dev/null)"