/*
 * webp_save.h - save a bitmap as a WebP image file
 *
 * Tiny single-frame WebP writer built on libwebp.  It accepts a tightly
 * packed, top-down, 24-bit RGB bitmap (3 bytes per pixel, red first)
 * and writes a standard WebP file (VP8 for lossy, VP8L for lossless).
 *
 * libwebp is linked statically, so the resulting exe needs NO codec DLLs
 * at runtime (unlike the old VP9/WebM encoder).  Build/link notes:
 *   gcc -O2 -municode webp_save.c -lwebp -lsharpyuv -lpthread -lm
 *
 * Version: 1.0
 */

#ifndef WEBP_SAVE_H
#define WEBP_SAVE_H

#define WEBP_MIN_QUALITY 1
#define WEBP_MAX_QUALITY 100
#define WEBP_DEFAULT_QUALITY 80

#include <windows.h>

/* Encode the RGB frame (w*h*3 bytes, order R,G,B, rows top-down) as a
 * single-frame WebP image written to `path`.
 *
 *   quality : 1..100 (WEBP_MIN_QUALITY..WEBP_MAX_QUALITY), same scale as
 *             JPEG: 1 = smallest file / lowest quality,
 *                  100 = best quality / largest file.
 *             Used only when `lossless` is 0.
 *   lossless: non-zero => WebP lossless mode (VP8L, perfect pixels,
 *             larger file).  Ignores `quality`.
 *
 * Returns 1 on success, 0 on failure.  On failure a short English
 * message is printed to stderr.
 */
int save_webp(const WCHAR* path, const BYTE* rgb, int w, int h,
              int quality, int lossless);

#endif /* WEBP_SAVE_H */