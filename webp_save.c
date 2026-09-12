/*
 * webp_save.c - write a single-frame WebP image file
 *
 * Pipeline:
 *   1. libwebp encodes the packed RGB24 frame directly:
 *        - lossy     -> WebPEncodeRGB()          (VP8, quality 0..100)
 *        - lossless  -> WebPEncodeLosslessRGB()  (VP8L, perfect pixels)
 *   2. The resulting buffer (already a complete RIFF/WEBP file) is
 *      written to disk.
 *
 * No EBML/Container muxing and no color conversion are needed: WebP is a
 * still-image format, so media players and image viewers render the file
 * as a picture (no duration/timeline like the old WebM output).
 *
 * Build/link notes (MSYS2 ucrt64, libwebp built statically from source):
 *   gcc -O2 -municode webp_save.c -lwebp -lsharpyuv -lpthread -lm
 *
 * Version: 1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "webp/encode.h"
#include "webp_save.h"

int save_webp(const WCHAR* path, const BYTE* rgb, int w, int h,
              int quality, int lossless) {
    FILE* f = NULL;
    BYTE* out = NULL;
    size_t len = 0;
    int rc = 0;

    if (!path || !rgb || w <= 0 || h <= 0) {
        fprintf(stderr, "webp: bad arguments\n");
        return 0;
    }

    if (lossless) {
        len = WebPEncodeLosslessRGB(rgb, w, h, w * 3, &out);
    } else {
        float q = (float)quality;
        if (q < WEBP_MIN_QUALITY) q = (float)WEBP_MIN_QUALITY;
        if (q > WEBP_MAX_QUALITY) q = (float)WEBP_MAX_QUALITY;
        len = WebPEncodeRGB(rgb, w, h, w * 3, q, &out);
    }

    if (len == 0 || !out) {
        fprintf(stderr, "webp: encoding failed\n");
        goto done;
    }

    f = _wfopen(path, L"wb");
    if (!f) {
        fprintf(stderr, "webp: cannot open %ls\n", path);
        goto done;
    }
    if (fwrite(out, 1, len, f) != len || ferror(f)) {
        fprintf(stderr, "webp: write failed\n");
        goto done;
    }
    rc = 1;

done:
    if (f) fclose(f);
    WebPFree(out);
    return rc;
}