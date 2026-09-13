/*
 * ClipSave - Windows clipboard monitor (ANSI C)
 * Captures images copied to the clipboard and saves them as files.
 * Mechanism: AddClipboardFormatListener (zero-polling, ~0% CPU).
 * Requires: Windows 10/11, GDI+ (built-in).
 *
 * Author:   Igor Brzeżek
 * Version:  0.9
 * GitHub:   https://github.com/IgorBrzezek/ClipSave
 *
 * Build:  gcc -O2 -municode clipsave.c webp_save.c -lgdiplus -lgdi32 \
 *             -lole32 -luuid -lwebp -lsharpyuv -lpthread -lm -o clipsave.exe
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#include <wincon.h>
#include <objidl.h>
#include <objbase.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <conio.h>
#include "webp_save.h"

#pragma comment(lib, "gdiplus")
#pragma comment(lib, "ole32")
#pragma comment(lib, "uuid")
#pragma comment(lib, "webp")
#pragma comment(lib, "sharpyuv")

/* ── Constants ─────────────────────────────────────────────── */

#define APP_NAME L"ClipSave"
#define HIDDEN_CLASS L"ClipSaveHiddenWnd"

#define AUTHOR L"Igor Brzezek"
#define VERSION L"0.9"
#define DATE L"13.09.2026"
#define GITHUB L"https://github.com/IgorBrzezek/ClipSave"


/* ── LUT tables for RGB565 quantization ───────────────────── */

static BYTE LUT_R5[256], LUT_G6[256], LUT_B5[256];
static int lut_init = 0;

static void init_lut(void) {
    int i;
    if (lut_init) return;
    for (i = 0; i < 256; i++) {
        LUT_R5[i] = (BYTE)((i >> 3) << 3);
        LUT_G6[i] = (BYTE)((i >> 2) << 2);
        LUT_B5[i] = (BYTE)((i >> 3) << 3);
    }
    lut_init = 1;
}

/* ── Configuration ────────────────────────────────────────── */

typedef struct {
    WCHAR directory[MAX_PATH];
    WCHAR fmt[8];
    int    bpp;
    WCHAR  name_mode[128];
    int    overwrite;
    int    compression;
    int    webp_quality;     /* 1..100, only used when fmt == webp       */
    int    webp_lossless;    /* non-zero => WebP lossless (VP8L)          */
} Config;

static Config cfg = {
    L".",
    L"png",
    24,
    L"DATETIME",
    0,
    -1,
    WEBP_DEFAULT_QUALITY,
    0,
};

#define HOTKEY_ID 1
static int g_active = 1;
static int g_color = 0;
static int g_beep = 0;
static SHORT g_status_row = -1;

/* ── Scrollable file list ────────────────────────────────── */

#define MAX_ENTRIES 4096
static WCHAR* entries[MAX_ENTRIES];
static int entry_count = 0;
static int scroll_pos = 0;

/* ── Hotkey configuration ─────────────────────────────────── */

typedef struct {
    int    mod;
    UINT   vk;
    WCHAR  display[64];
} HotkeyCfg;

static HotkeyCfg hotkey = { MOD_CONTROL | MOD_SHIFT, VK_F11, L"Ctrl-Shift-F11" };

/* ── GDI+ init ────────────────────────────────────────────── */

static ULONG_PTR gdiplus_token = 0;

static void gdiplus_init(void) {
    GdiplusStartupInput si;
    si.GdiplusVersion           = 1;
    si.DebugEventCallback       = NULL;
    si.SuppressBackgroundThread = FALSE;
    si.SuppressExternalCodecs   = FALSE;
    GdiplusStartup(&gdiplus_token, &si, NULL);
}

static void gdiplus_shutdown(void) {
    if (gdiplus_token) GdiplusShutdown(gdiplus_token);
}

/* ── Get encoder CLSID by MIME type ───────────────────────── */

static int get_encoder_clsid(const WCHAR* mime, CLSID* clsid) {
    UINT n = 0, sz = 0;
    if (GdipGetImageEncodersSize(&n, &sz) != Ok) return 0;
    if (sz == 0) return 0;
    ImageCodecInfo* enc = (ImageCodecInfo*)malloc(sz);
    if (!enc) return 0;
    GdipGetImageEncoders(n, sz, enc);
    int found = 0;
    for (UINT i = 0; i < n; i++) {
        if (wcscmp(enc[i].MimeType, mime) == 0) {
            *clsid = enc[i].Clsid;
            found = 1;
            break;
        }
    }
    free(enc);
    return found;
}

/* ── Simple FNV-1a hash for deduplication ─────────────────── */

static UINT64 image_hash(GpBitmap* bmp) {
    UINT w, h;
    if (GdipGetImageWidth((GpImage*)bmp, &w) != Ok) return 0;
    if (GdipGetImageHeight((GpImage*)bmp, &h) != Ok) return 0;

    GpRect r = { 0, 0, (int)w, (int)h };
    BitmapData data;
    if (GdipBitmapLockBits(bmp, &r, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok)
        return 0;

    UINT64 hash = 14695981039346656037ULL;
    BYTE* px = (BYTE*)data.Scan0;
    size_t total = (size_t)w * h * 4;
    size_t step = total > 16384 ? total / 4096 : 1;

    for (size_t i = 0; i < total; i += step) {
        hash ^= px[i];
        hash *= 1099511628211ULL;
    }

    GdipBitmapUnlockBits(bmp, &data);
    return hash;
}

/* ── Color depth conversion ───────────────────────────────── */

static GpBitmap* apply_bpp(GpBitmap* src) {
    UINT w, h;
    if (GdipGetImageWidth((GpImage*)src, &w) != Ok ||
        GdipGetImageHeight((GpImage*)src, &h) != Ok)
        return NULL;

    GpRect r = { 0, 0, (int)w, (int)h };
    BitmapData src_data;
    if (GdipBitmapLockBits(src, &r, ImageLockModeRead, PixelFormat32bppARGB, &src_data) != Ok)
        return NULL;

    BYTE* px = (BYTE*)src_data.Scan0;
    int stride = src_data.Stride;
    GpBitmap* result = NULL;

    if (cfg.bpp == 8) {
        /* Grayscale L mode - use BT.601 luminance */
        GdipCreateBitmapFromScan0(w, h, 0, PixelFormat24bppRGB, NULL, &result);
        if (result) {
            BitmapData dst_data;
            GpRect dr = { 0, 0, (int)w, (int)h };
            if (GdipBitmapLockBits(result, &dr, ImageLockModeWrite, PixelFormat24bppRGB, &dst_data) == Ok) {
                BYTE* dst = (BYTE*)dst_data.Scan0;
                int ds = dst_data.Stride;
                for (UINT y = 0; y < h; y++) {
                    BYTE* sp = px + y * stride;
                    BYTE* dp = dst + y * ds;
                    for (UINT x = 0; x < w; x++) {
                        BYTE b  = sp[x * 4 + 0];
                        BYTE g  = sp[x * 4 + 1];
                        BYTE rr = sp[x * 4 + 2];
                        BYTE lum = (BYTE)((rr * 77 + g * 150 + b * 29 + 128) >> 8);
                        dp[x * 3 + 0] = lum;
                        dp[x * 3 + 1] = lum;
                        dp[x * 3 + 2] = lum;
                    }
                }
                GdipBitmapUnlockBits(result, &dst_data);
            }
        }
    }
    else if (cfg.bpp == 16) {
        /* RGB565 quantization */
        init_lut();
        GdipCreateBitmapFromScan0(w, h, 0, PixelFormat24bppRGB, NULL, &result);
        if (result) {
            BitmapData dst_data;
            GpRect dr = { 0, 0, (int)w, (int)h };
            if (GdipBitmapLockBits(result, &dr, ImageLockModeWrite, PixelFormat24bppRGB, &dst_data) == Ok) {
                BYTE* dst = (BYTE*)dst_data.Scan0;
                int ds = dst_data.Stride;
                for (UINT y = 0; y < h; y++) {
                    BYTE* sp = px + y * stride;
                    BYTE* dp = dst + y * ds;
                    for (UINT x = 0; x < w; x++) {
                        BYTE b  = sp[x * 4 + 0];
                        BYTE g  = sp[x * 4 + 1];
                        BYTE rr = sp[x * 4 + 2];
                        dp[x * 3 + 0] = LUT_B5[b];
                        dp[x * 3 + 1] = LUT_G6[g];
                        dp[x * 3 + 2] = LUT_R5[rr];
                    }
                }
                GdipBitmapUnlockBits(result, &dst_data);
            }
        }
    }
    else if (cfg.bpp == 'P') {
        /* 8-bit palette: draw 24bpp copy into 8bpp indexed bitmap via GDI+ */
        GpBitmap* tmp = NULL;
        GdipCreateBitmapFromScan0(w, h, 0, PixelFormat24bppRGB, NULL, &tmp);
        if (tmp) {
            BitmapData dst_data;
            GpRect dr = { 0, 0, (int)w, (int)h };
            if (GdipBitmapLockBits(tmp, &dr, ImageLockModeWrite, PixelFormat24bppRGB, &dst_data) == Ok) {
                BYTE* dst = (BYTE*)dst_data.Scan0;
                int ds = dst_data.Stride;
                for (UINT y = 0; y < h; y++) {
                    BYTE* sp = px + y * stride;
                    BYTE* dp = dst + y * ds;
                    for (UINT x = 0; x < w; x++) {
                        dp[x * 3 + 0] = sp[x * 4 + 0];
                        dp[x * 3 + 1] = sp[x * 4 + 1];
                        dp[x * 3 + 2] = sp[x * 4 + 2];
                    }
                }
                GdipBitmapUnlockBits(tmp, &dst_data);
            }
            /* Create 8bpp indexed and draw with dithering */
            GdipCreateBitmapFromScan0(w, h, 0, PixelFormat8bppIndexed, NULL, &result);
            if (result) {
                GpGraphics* g = NULL;
                GdipGetImageGraphicsContext((GpImage*)result, &g);
                if (g) {
                    GdipDrawImageRectRectI(g, (GpImage*)tmp,
                        0, 0, (int)w, (int)h,
                        0, 0, (int)w, (int)h,
                        UnitPixel, NULL, NULL, NULL);
                    GdipDeleteGraphics(g);
                }
            }
            GdipDisposeImage((GpImage*)tmp);
        }
    }
    else {
        /* 24 bpp - copy ARGB to 24bpp RGB */
        GdipCreateBitmapFromScan0(w, h, 0, PixelFormat24bppRGB, NULL, &result);
        if (result) {
            BitmapData dst_data;
            GpRect dr = { 0, 0, (int)w, (int)h };
            if (GdipBitmapLockBits(result, &dr, ImageLockModeWrite, PixelFormat24bppRGB, &dst_data) == Ok) {
                BYTE* dst = (BYTE*)dst_data.Scan0;
                int ds = dst_data.Stride;
                for (UINT y = 0; y < h; y++) {
                    BYTE* sp = px + y * stride;
                    BYTE* dp = dst + y * ds;
                    for (UINT x = 0; x < w; x++) {
                        dp[x * 3 + 0] = sp[x * 4 + 0];
                        dp[x * 3 + 1] = sp[x * 4 + 1];
                        dp[x * 3 + 2] = sp[x * 4 + 2];
                    }
                }
                GdipBitmapUnlockBits(result, &dst_data);
            }
        }
    }

    GdipBitmapUnlockBits(src, &src_data);
    return result;
}

/* ── Save image via GDI+ ──────────────────────────────────── */

static int save_image(GpBitmap* img, const WCHAR* path, const WCHAR* fmt) {
    CLSID clsid;
    const WCHAR* mime;

    if (wcscmp(fmt, L"jpg") == 0)
        mime = L"image/jpeg";
    else if (wcscmp(fmt, L"bmp") == 0)
        mime = L"image/bmp";
    else
        mime = L"image/png";

    if (!get_encoder_clsid(mime, &clsid))
        return 0;

    EncoderParameters enc_params;
    ULONG quality_val = (ULONG)cfg.compression;
    enc_params.Count = 0;

    if (wcscmp(fmt, L"jpg") == 0) {
        enc_params.Count = 1;
        enc_params.Parameter[0].Guid = EncoderQuality;
        enc_params.Parameter[0].Type = EncoderParameterValueTypeLong;
        enc_params.Parameter[0].NumberOfValues = 1;
        enc_params.Parameter[0].Value = &quality_val;
    }

    GpStatus st = GdipSaveImageToFile(img, path, &clsid,
        (enc_params.Count > 0) ? &enc_params : NULL);
    return (st == Ok);
}

/* ── Save as WebP (image) ───────────────────────────────────────── */

/* Locks the 24bpp GDI+ bitmap (memory order B,G,R, padded stride) and
   hands a tightly packed top-down R,G,B buffer to the WebP encoder. */
static int save_webp_path(GpBitmap* img, const WCHAR* path) {
    UINT w, h;
    if (GdipGetImageWidth((GpImage*)img, &w) != Ok) return 0;
    if (GdipGetImageHeight((GpImage*)img, &h) != Ok) return 0;

    GpRect r = { 0, 0, (int)w, (int)h };
    BitmapData d;
    if (GdipBitmapLockBits(img, &r, ImageLockModeRead,
                           PixelFormat24bppRGB, &d) != Ok)
        return 0;

    BYTE* sp = (BYTE*)d.Scan0;
    int stride = d.Stride;
    BYTE* rgb = (BYTE*)malloc((size_t)w * h * 3);
    int ok = 0;
    if (rgb) {
        for (UINT y = 0; y < h; y++) {
            BYTE* src = sp + (size_t)y * stride;
            BYTE* dst = rgb + (size_t)y * w * 3;
            for (UINT x = 0; x < w; x++) {
                dst[x * 3 + 0] = src[x * 3 + 2];
                dst[x * 3 + 1] = src[x * 3 + 1];
                dst[x * 3 + 2] = src[x * 3 + 0];
            }
        }
        ok = save_webp(path, rgb, (int)w, (int)h,
                       cfg.webp_quality, cfg.webp_lossless);
        free(rgb);
    }
    GdipBitmapUnlockBits(img, &d);
    return ok;
}

/* ── Filename generation ──────────────────────────────────── */

static int counter = 0;

static void make_filename(WCHAR* out, size_t out_sz, const WCHAR* fmt, const WCHAR* name_mode) {
    time_t t;
    struct tm tm;
    WCHAR stamp[64], date_str[16], time_str[16];

    time(&t);
    localtime_s(&tm, &t);

    swprintf(date_str, 16, L"%04d%02d%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    swprintf(time_str, 16, L"%02d%02d%02d",
        tm.tm_hour, tm.tm_min, tm.tm_sec);

    if (_wcsicmp(name_mode, L"DATETIME") == 0) {
        swprintf(stamp, 64, L"%s_%s_000", date_str, time_str);
        swprintf(out, out_sz, L"%s\\clip_%s.%s", cfg.directory, stamp, fmt);
        return;
    }

    WCHAR buf[256];
    wcscpy_s(buf, 256, name_mode);
    WCHAR tmp[512];
    WCHAR* p;

    swprintf(stamp, 64, L"%s_%s", date_str, time_str);
    while ((p = wcsstr(buf, L"[DT]"))) {
        *p = 0; wcscpy_s(tmp, 512, buf);
        wcscat_s(tmp, 512, stamp); wcscat_s(tmp, 512, p + 4);
        wcscpy_s(buf, 256, tmp);
    }
    swprintf(stamp, 64, L"%s_%s", time_str, date_str);
    while ((p = wcsstr(buf, L"[TD]"))) {
        *p = 0; wcscpy_s(tmp, 512, buf);
        wcscat_s(tmp, 512, stamp); wcscat_s(tmp, 512, p + 4);
        wcscpy_s(buf, 256, tmp);
    }
    while ((p = wcsstr(buf, L"[D]"))) {
        *p = 0; wcscpy_s(tmp, 512, buf);
        wcscat_s(tmp, 512, date_str); wcscat_s(tmp, 512, p + 3);
        wcscpy_s(buf, 256, tmp);
    }
    while ((p = wcsstr(buf, L"[T]"))) {
        *p = 0; wcscpy_s(tmp, 512, buf);
        wcscat_s(tmp, 512, time_str); wcscat_s(tmp, 512, p + 3);
        wcscpy_s(buf, 256, tmp);
    }
    while ((p = wcsstr(buf, L"[N"))) {
        WCHAR* end = wcschr(p, L']');
        if (!end) break;
        int ncount = 0;
        WCHAR* np = p + 1;
        while (*np == L'N') { ncount++; np++; }
        if (ncount < 1) { p = end + 1; continue; }
        *p = 0;
        swprintf(stamp, 64, L"%0*d", ncount, counter + 1);
        wcscpy_s(tmp, 512, buf);
        wcscat_s(tmp, 512, stamp); wcscat_s(tmp, 512, end + 1);
        wcscpy_s(buf, 256, tmp);
    }

    swprintf(out, out_sz, L"%s\\%s.%s", cfg.directory, buf, fmt);
}

/* ── Clipboard event handler ───────────────────────────────── */

static void redraw_entries(void);

/* ── Saved-image history for duplicate suppression ───────────── */
/* Windows 11 / Snipping Tool re-publishes the same screenshot to the
   clipboard twice (two WM_CLIPBOARDUPDATE messages with byte-identical
   pixels, the second one arriving up to a few seconds later once Clipboard
   History / delay rendering finishes).  The old code treated that echo as a
   new event, printing a second "[already saved]" line for a single capture.
   We now remember the images we actually saved and silently drop any
   update that is byte-identical to one saved DUP_WINDOW_MS ago; the same
   content copied again later is still reported, but with the *original*
   file name and number. */

#define DUP_HISTORY   32
#define DUP_WINDOW_MS 5000

typedef struct {
    UINT64 hash;
    int    number;              /* entry number printed in the [+] line */
    WCHAR  basename[MAX_PATH];
    DWORD  when;                /* GetTickCount() at save time          */
} SavedImage;

static SavedImage saved[ DUP_HISTORY ];
static int saved_pos = 0;       /* ring write position                  */
static int saved_n   = 0;       /* number of records stored (<= DUP_HISTORY) */

static void note_saved(UINT64 hash, const WCHAR* basename, int number) {
    saved[saved_pos].hash = hash;
    saved[saved_pos].number = number;
    wcscpy_s(saved[saved_pos].basename, MAX_PATH, basename);
    saved[saved_pos].when = GetTickCount();
    saved_pos = (saved_pos + 1) % DUP_HISTORY;
    if (saved_n < DUP_HISTORY) saved_n++;
}

/* Returns:
 *   0 - genuinely new image, proceed to save.
 *   1 - identical image saved DUP_WINDOW_MS ago: this is the delayed
 *       Windows 11 echo of the capture just saved -> drop silently.
 *   2 - identical content copied again later: report it as an
 *       "[already saved]" line, with *found_number / *found_basename
 *       pointing at the real file that holds the same pixels.        */
static int check_duplicate(UINT64 hval, int* found_number, WCHAR* found_basename) {
    DWORD now = GetTickCount();
    for (int i = 0; i < saved_n; i++) {
        if (saved[i].hash == hval) {
            if ((DWORD)(now - saved[i].when) < DUP_WINDOW_MS)
                return 1;
            *found_number = saved[i].number;
            wcscpy_s(found_basename, MAX_PATH, saved[i].basename);
            return 2;
        }
    }
    return 0;
}

/* ── Robust clipboard reading ──────────────────────────────── */
/* WM_CLIPBOARDUPDATE is *posted* (delivered asynchronously), at a moment
   the source application may still hold the clipboard open - so a
   listener often cannot open it (ERROR_ACCESSDENIED) - and delay-rendered
   image data may not be ready yet.  Retry instead of giving up on the
   first try; this is what makes Snipping Tool / Win+Shift+S / Ctrl+Shift+S
   captures reliable.

   Windows 11 also adds an observer race: Clipboard History (cbdhsvc),
   Cloud Clipboard and other listeners re-open the clipboard right after
   the source closes, so the format set can be observed in a transient,
   partial state.  Therefore the whole open/check/read/close cycle is
   repeated across the retry window (not just OpenClipboard).

   The registered "PNG" format is Snipping Tool's native, lossless
   representation and is always preferred; it never goes through the lossy
   DIB synthesis path (BI_BITFIELDS masks are misplaced when CF_DIB is
   synthesized from a CF_DIBV5 bitmap).  The DIB/BITMAP read is kept only
   as a last resort for sources that never publish PNG. */

#define CLIP_OPEN_RETRIES  20      /* max OpenClipboard attempts        */
#define CLIP_OPEN_SLEEP_MS 50      /* ms between OpenClipboard attempts */
#define CLIP_DATA_ATTEMPTS 3       /* max data-read attempts per open  */
#define CLIP_DATA_SLEEP_MS 150     /* ms between data-read attempts     */

static UINT g_png_format = 0;

/* Offset (bytes) from the start of a packed DIB to its pixel bits. */
static DWORD dib_pixel_offset(const BITMAPINFOHEADER* bih) {
    DWORD off = bih->biSize;
    /* Only a legacy BITMAPINFOHEADER (biSize == 40) stores the BI_BITFIELDS
       color masks *after* the header.  In a BITMAPV4HEADER / BITMAPV5HEADER
       (used by CF_DIBV5, which Snipping Tool publishes) the masks are
       embedded inside the header itself, so nothing extra is appended.
       Adding 3 DWORDs unconditionally shifted the pixel bits by 12 bytes
       and produced a misplaced image whenever the DIB/BITMAP fallback was
       used on a V4/V5 BI_BITFIELDS bitmap. */
    if (bih->biSize == sizeof(BITMAPINFOHEADER) &&
        bih->biBitCount > 8 && bih->biCompression == BI_BITFIELDS)
        off += 3 * sizeof(DWORD);
    if (bih->biClrUsed > 0)
        off += bih->biClrUsed * (DWORD)sizeof(RGBQUAD);
    else if (bih->biBitCount <= 8)
        off += ((DWORD)1 << bih->biBitCount) * (DWORD)sizeof(RGBQUAD);
    return off;
}

/* Convert a packed DIB block (CF_DIB / CF_DIBV5) into a GpBitmap. */
static GpBitmap* gdip_from_packed_dib(const BYTE* dib, SIZE_T dib_size) {
    const BITMAPINFO* bmi = (const BITMAPINFO*)dib;
    GpBitmap* out = NULL;
    HBITMAP hbm = NULL;
    void* bits = NULL;
    HDC hdc;
    DWORD off;
    UINT w, h;
    size_t pitch, need;

    if (dib_size < sizeof(BITMAPINFOHEADER)) return NULL;
    if (bmi->bmiHeader.biWidth <= 0 || bmi->bmiHeader.biHeight == 0) return NULL;
    if (bmi->bmiHeader.biPlanes != 1) return NULL;
    if (bmi->bmiHeader.biCompression != BI_RGB &&
        bmi->bmiHeader.biCompression != BI_BITFIELDS)
        return NULL;   /* RLE / JPEG / PNG packed DIBs are not supported */

    w = (UINT)bmi->bmiHeader.biWidth;
    h = (UINT)labs(bmi->bmiHeader.biHeight);
    off = dib_pixel_offset(&bmi->bmiHeader);
    pitch = ((w * bmi->bmiHeader.biBitCount + 31) / 32) * 4;
    if (pitch == 0) return NULL;
    need = (size_t)off + pitch * h;
    if (need > dib_size) return NULL;   /* truncated clipboard data */

    hdc = GetDC(NULL);
    hbm = CreateDIBSection(hdc, bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (hbm && bits) {
        memcpy(bits, dib + off, pitch * h);
        GdipCreateBitmapFromHBITMAP(hbm, NULL, &out);
    }
    ReleaseDC(NULL, hdc);
    if (hbm) DeleteObject(hbm);
    return out;
}

/* Decode the registered "PNG" clipboard format into a self-contained
   GpBitmap (the stream is freed here; later GDI+ use is stream-free). */
static GpBitmap* gdip_from_clip_png(void) {
    HANDLE h;
    SIZE_T sz;
    void* data;
    HGLOBAL own = NULL;
    IStream* stream = NULL;
    GpBitmap* decoded = NULL;
    GpBitmap* out = NULL;

    if (!g_png_format) return NULL;
    h = GetClipboardData(g_png_format);
    if (!h) return NULL;

    data = GlobalLock(h);
    if (!data) return NULL;
    sz = GlobalSize(h);
    if (sz < 8 || sz > (SIZE_T)64 * 1024 * 1024) { GlobalUnlock(h); return NULL; }

    own = GlobalAlloc(GMEM_MOVEABLE, sz);
    if (own) {
        void* dst = GlobalLock(own);
        if (dst) {
            memcpy(dst, data, sz);
            GlobalUnlock(own);
        } else {
            GlobalFree(own);
            own = NULL;
        }
    }
    GlobalUnlock(h);
    if (!own) return NULL;

    /* The stream takes ownership of our private copy, so the stream can
       be destroyed here after we copy the decoded pixels into a fresh
       bitmap ("Bitmap and Image constructor dependencies", KB 814675). */
    if (CreateStreamOnHGlobal(own, TRUE, &stream) == S_OK) {
        if (GdipCreateBitmapFromStream((IStream*)stream, &decoded) == Ok) {
            UINT w = 0, hgt = 0;
            GdipGetImageWidth((GpImage*)decoded, &w);
            GdipGetImageHeight((GpImage*)decoded, &hgt);
            if (w > 0 && hgt > 0 &&
                GdipCreateBitmapFromScan0(w, hgt, 0, PixelFormat32bppARGB, NULL, &out) == Ok) {
                GpGraphics* g = NULL;
                if (GdipGetImageGraphicsContext((GpImage*)out, &g) == Ok) {
                    GdipDrawImageRectRectI(g, (GpImage*)decoded,
                        0, 0, (INT)w, (INT)hgt,
                        0, 0, (INT)w, (INT)hgt,
                        UnitPixel, NULL, NULL, NULL);
                    GdipDeleteGraphics(g);
                }
            }
            GdipDisposeImage((GpImage*)decoded);
        }
        stream->lpVtbl->Release(stream);
    }
    return out;
}

/* Returns 1 if the (open) clipboard holds any image-like format. */
static int clipboard_has_image_format(void) {
    UINT fmt = 0;
    while ((fmt = EnumClipboardFormats(fmt)) != 0) {
        if (fmt == CF_BITMAP || fmt == CF_DIB || fmt == CF_DIBV5 ||
            (g_png_format && fmt == g_png_format))
            return 1;
    }
    return 0;
}

/* Read the image from the (open) clipboard.  Tries the registered PNG
   format first (native Snipping Tool / browser format, avoids lossy DIB
   synthesis), then CF_DIBV5 / CF_DIB with correct pixel-offset handling,
   then CF_BITMAP. */
static GpBitmap* clipboard_image_from_open_clipboard(void) {
    GpBitmap* out;
    HANDLE h;
    void* data;
    SIZE_T sz;

    out = gdip_from_clip_png();
    if (out) return out;

    h = GetClipboardData(CF_DIBV5);
    if (!h) h = GetClipboardData(CF_DIB);
    if (h) {
        data = GlobalLock(h);
        if (data) {
            sz = GlobalSize(h);
            out = gdip_from_packed_dib((const BYTE*)data, sz);
            GlobalUnlock(h);
            if (out) return out;
        }
    }

    {
        HBITMAP hbm = (HBITMAP)GetClipboardData(CF_BITMAP);
        if (hbm) {
            GpBitmap* b = NULL;
            if (GdipCreateBitmapFromHBITMAP(hbm, NULL, &b) == Ok && b)
                return b;
        }
    }
    return NULL;
}

static void on_clipboard(void) {
    if (!g_active) return;

    /* Snapshot configuration at entry — guard against corruption */
    WCHAR local_fmt[8];
    WCHAR local_name[128];
    wcscpy_s(local_fmt, 8, cfg.fmt);
    wcscpy_s(local_name, 128, cfg.name_mode);

    if (!g_png_format)
        g_png_format = RegisterClipboardFormatW(L"PNG");

    /* The WM_CLIPBOARDUPDATE notification is *posted* and can be delivered
       before the source application has closed the clipboard (OpenClipboard
       fails with ERROR_ACCESSDENIED) or while the format set is still being
       populated.  The whole open/check/read/close cycle is therefore repeated
       across the entire retry window; the previous code gave up as soon as a
       single OpenClipboard call had succeeded, silently dropping Snipping
       Tool captures whenever that "first successful open" saw the clipboard
       in a transient, incomplete state.

       The native registered "PNG" format is preferred: it is Snipping Tool's
       exact, lossless pixels.  We keep waiting for it for as long as it is
       advertised (so capture always looks the same), while a synthesized
       DIB/BITMAP read is used only for sources that never publish PNG. */
    GpBitmap* src_bmp = NULL;      /* native PNG image (preferred)            */
    GpBitmap* fallback = NULL;     /* DIB/BITMAP image (used only if no PNG)  */
    int png_advertised = 0;
    int attempt;
    for (attempt = 0; attempt < CLIP_OPEN_RETRIES; attempt++) {
        if (!OpenClipboard(NULL)) {
            Sleep(CLIP_OPEN_SLEEP_MS);
            continue;
        }
        if (g_png_format && IsClipboardFormatAvailable(g_png_format))
            png_advertised = 1;
        if (clipboard_has_image_format()) {
            int r;
            for (r = 0; r < CLIP_DATA_ATTEMPTS && !src_bmp; r++) {
                src_bmp = gdip_from_clip_png();
                if (!src_bmp && !fallback)
                    fallback = clipboard_image_from_open_clipboard();
                if (src_bmp)
                    break;
                if (!fallback) {
                    Sleep(CLIP_DATA_SLEEP_MS);   /* delay-rendered data */
                    continue;
                }
                break;   /* have a fallback, keep waiting for PNG next open */
            }
        }
        CloseClipboard();
        if (src_bmp)
            break;
        if (fallback && !png_advertised)
            break;       /* PNG can never arrive - use the DIB/BITMAP now  */
        Sleep(CLIP_OPEN_SLEEP_MS);
    }

    if (src_bmp) {
        if (fallback) GdipDisposeImage((GpImage*)fallback);
    } else {
        src_bmp = fallback;
    }
    if (!src_bmp) return;

    /* Deduplication */
    UINT64 hval = image_hash(src_bmp);
    int dup_number = 0;
    WCHAR dup_base[MAX_PATH];
    int dup = check_duplicate(hval, &dup_number, dup_base);

    if (dup == 1) {
        /* Same pixels republished within DUP_WINDOW_MS - the Windows 11
           echo of the capture we just saved.  Stay silent so a single
           screenshot produces a single list entry. */
        GdipDisposeImage((GpImage*)src_bmp);
        return;
    }
    if (dup == 2) {
        /* Identical content copied again (or a very late echo): report it,
           naming the real file that was saved before. */
        if (g_beep) Beep(600, 200);
        WCHAR dup_line[512];
        swprintf(dup_line, 512, L"  %s[%d] %s%s  [already saved]",
            g_color ? L"\x1b[33m" : L"",
            dup_number, dup_base,
            g_color ? L"\x1b[0m" : L"");
        if (entry_count < MAX_ENTRIES) {
            size_t len = wcslen(dup_line);
            entries[entry_count] = (WCHAR*)malloc((len + 1) * sizeof(WCHAR));
            if (entries[entry_count]) {
                wcscpy_s(entries[entry_count], len + 1, dup_line);
                entry_count++;
            }
        }
        scroll_pos = 0;
        redraw_entries();
        fflush(stdout);
        GdipDisposeImage((GpImage*)src_bmp);
        return;
    }

    /* Apply color depth */
    GpBitmap* final_img = apply_bpp(src_bmp);
    GdipDisposeImage((GpImage*)src_bmp);

    if (!final_img) {
        return;
    }

    /* Build path */
    WCHAR path[MAX_PATH];
    make_filename(path, MAX_PATH, local_fmt, local_name);

    /* Overwrite check */
    if (!cfg.overwrite) {
        FILE* f = _wfopen(path, L"rb");
        if (f) {
            fclose(f);
            const WCHAR* base = wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            if (g_beep) Beep(400, 600);
            {
                HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
                CONSOLE_SCREEN_BUFFER_INFO ci;
                GetConsoleScreenBufferInfo(h, &ci);
                COORD pp = {0, ci.dwCursorPosition.Y};
                SetConsoleCursorPosition(h, pp);
                DWORD nn;
                FillConsoleOutputCharacterW(h, L' ', ci.dwSize.X, pp, &nn);
                WCHAR pbuf[520];
                swprintf(pbuf, 520, L"  [?] File exists: %s. Overwrite? [y/N] ", base);
                WriteConsoleW(h, pbuf, (DWORD)wcslen(pbuf), &nn, NULL);
            }
            int _ch = _getwch();
            if (_ch != L'y' && _ch != L'Y') {
                GdipDisposeImage((GpImage*)final_img);
                redraw_entries();
                fflush(stdout);
                return;
            }
        }
    }

    /* Save */
    {
        int is_webp = (wcscmp(local_fmt, L"webp") == 0);
        int saved = is_webp ? save_webp_path(final_img, path)
                            : save_image(final_img, path, local_fmt);
        if (!saved) {
            if (g_color)
                wprintf(L"  \x1b[31m[!] Save error: %s - WRITE ERROR!\x1b[0m\n", path);
            else
                wprintf(L"  [!] Save error: %s\n", path);
            GdipDisposeImage((GpImage*)final_img);
            return;
        }
    }

    counter++;
    if (g_beep) Beep(1200, 100);
    UINT img_w, img_h;
    GdipGetImageWidth((GpImage*)final_img, &img_w);
    GdipGetImageHeight((GpImage*)final_img, &img_h);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    UINT64 sz = 0;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        sz = ((UINT64)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    const WCHAR* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;

    note_saved(hval, base, counter);

    WCHAR line[512];
    if (sz < 1048576)
        swprintf(line, 512, L"  [+] %s[%d] %s%s  [%ux%u px, %.1f KB]",
            g_color ? (wcscmp(local_fmt, L"jpg") == 0 ? L"\x1b[35m" :
                       wcscmp(local_fmt, L"png") == 0 ? L"\x1b[36m" :
                       wcscmp(local_fmt, L"webp") == 0 ? L"\x1b[32m" : L"\x1b[33m") : L"",
            counter, base,
            g_color ? L"\x1b[0m" : L"",
            img_w, img_h, sz / 1024.0);
    else
        swprintf(line, 512, L"  [+] %s[%d] %s%s  [%ux%u px, %.1f MB]",
            g_color ? (wcscmp(local_fmt, L"jpg") == 0 ? L"\x1b[35m" :
                       wcscmp(local_fmt, L"png") == 0 ? L"\x1b[36m" :
                       wcscmp(local_fmt, L"webp") == 0 ? L"\x1b[32m" : L"\x1b[33m") : L"",
            counter, base,
            g_color ? L"\x1b[0m" : L"",
            img_w, img_h, sz / 1048576.0);

    if (entry_count < MAX_ENTRIES) {
        size_t len = wcslen(line);
        entries[entry_count] = (WCHAR*)malloc((len + 1) * sizeof(WCHAR));
        if (entries[entry_count]) {
            wcscpy_s(entries[entry_count], len + 1, line);
            entry_count++;
        }
    }
    scroll_pos = 0;
    redraw_entries();
    fflush(stdout);

    GdipDisposeImage((GpImage*)final_img);
}

/* ── Window procedure ─────────────────────────────────────── */

static HWND g_hwnd = NULL;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLIPBOARDUPDATE) {
        on_clipboard();
        return 0;
    }
    if (msg == WM_HOTKEY && wp == HOTKEY_ID) {
        g_active = !g_active;
        if (g_status_row >= 0) {
            HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(hCon, &csbi)) {
                COORD saved = csbi.dwCursorPosition;
                COORD target = { 0, g_status_row };
                SetConsoleCursorPosition(hCon, target);
                if (g_color)
                    wprintf(L"    \x1b[1;37mClipSave - clipboard monitor active: %s\x1b[0m  ",
                        g_active ? L"\x1b[32mON" : L"\x1b[31mOFF");
                else
                    wprintf(L"    ClipSave - clipboard monitor active: %s  ",
                        g_active ? L"ON" : L"OFF");
                SetConsoleCursorPosition(hCon, saved);
            }
        }
        fflush(stdout);
        return 0;
    }
    if (msg == WM_DESTROY) {
        RemoveClipboardFormatListener(hwnd);
        UnregisterHotKey(hwnd, HOTKEY_ID);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ── Ctrl+C handler ──────────────────────────────────────────────── */

static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (g_hwnd) PostMessageW(g_hwnd, WM_DESTROY, 0, 0);
        return TRUE;
    }
    return FALSE;
}

/* ── Message loop ────────────────────────────────────────────────── */

static void message_loop(void) {
    MSG msg;
    while (1) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (_kbhit()) {
            int ch = _getch();
            if (ch == 0xE0 || ch == 0) {
                ch = _getch();
                if (ch == 72 && scroll_pos < entry_count) scroll_pos++;
                else if (ch == 80 && scroll_pos > 0) scroll_pos--;
                else continue;
                redraw_entries();
            }
        }
        Sleep(50);
    }
}

/* ── Create listener window ──────────────────────────────────────── */

static int create_listener_window(void) {
    HINSTANCE hi = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hi;
    wc.lpszClassName = HIDDEN_CLASS;

    if (!RegisterClassExW(&wc)) {
        wprintf(L"ERROR: RegisterClassExW error %lu\n", GetLastError());
        return 0;
    }

    g_hwnd = CreateWindowExW(0, HIDDEN_CLASS, APP_NAME, 0,
        0, 0, 0, 0, NULL, NULL, hi, NULL);
    if (!g_hwnd) {
        wprintf(L"ERROR: CreateWindowExW error %lu\n", GetLastError());
        return 0;
    }

    if (!AddClipboardFormatListener(g_hwnd)) {
        wprintf(L"ERROR: AddClipboardFormatListener error %lu\n", GetLastError());
        return 0;
    }

    return 1;
}

/* ── Screen / scroll management ──────────────────────────── */

static void clear_screen(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    COORD top = {0, 0};
    DWORD n;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(h, &csbi);
    FillConsoleOutputCharacterW(h, L' ', csbi.dwSize.X * csbi.dwSize.Y, top, &n);
    SetConsoleCursorPosition(h, top);
}

#define HEADER_ROWS 12  /* 11 header lines + 1 blank */

static void redraw_entries(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(h, &csbi);
    int visible = csbi.srWindow.Bottom - HEADER_ROWS + 1;
    if (visible < 1) visible = 1;
    int max_s = entry_count - visible;
    if (max_s < 0) max_s = 0;
    if (scroll_pos > max_s) scroll_pos = max_s;
    int start = entry_count - visible - scroll_pos;
    if (start < 0) start = 0;

    SHORT w = csbi.dwSize.X;
    DWORD n;
    COORD pos = {0, (SHORT)HEADER_ROWS};
    for (int i = 0; i < visible; i++) {
        FillConsoleOutputCharacterW(h, L' ', w, pos, &n);
        SetConsoleCursorPosition(h, pos);
        int idx = start + i;
        if (idx >= 0 && idx < entry_count) {
            WriteConsoleW(h, entries[idx], (DWORD)wcslen(entries[idx]), &n, NULL);
        }
        pos.Y++;
    }
}

/* ── Startup display ─────────────────────────────────────────────── */

static void print_banner(void) {
    const WCHAR* bpp_str;
    if (cfg.bpp == 8)       bpp_str = L"grayscale";
    else if (cfg.bpp == 16) bpp_str = L"RGB565";
    else if (cfg.bpp == 24) bpp_str = L"full RGB";
    else if (cfg.bpp == 'P') bpp_str = L"palette 256";
    else                    bpp_str = L"?";

    WCHAR display_dir[MAX_PATH];
    DWORD len = GetFullPathNameW(cfg.directory, MAX_PATH, display_dir, NULL);
    if (len == 0 || len >= MAX_PATH)
        wcscpy_s(display_dir, MAX_PATH, cfg.directory);

    if (g_color) {
        HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(hCon, &mode))
            SetConsoleMode(hCon, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    wprintf(L"  %s=================================================================\n",
        g_color ? L"\x1b[90m" : L"");
    if (g_color) {
        wprintf(L"    \x1b[1;37mClipSave - clipboard monitor active: %s\x1b[0m\n",
            g_active ? L"\x1b[32mON\x1b[1;37m" : L"\x1b[31mOFF\x1b[1;37m");
    } else {
        wprintf(L"    ClipSave - clipboard monitor active: %s\n", g_active ? L"ON" : L"OFF");
    }
    {
        HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(hCon, &csbi))
            g_status_row = (SHORT)(csbi.dwCursorPosition.Y - 1);
    }
    wprintf(L"    %sv%s - %s - %s\n",
        g_color ? L"\x1b[90m" : L"", VERSION, AUTHOR, GITHUB);
    wprintf(L"  %s=================================================================\n",
        g_color ? L"\x1b[90m" : L"");
    wprintf(L"  %sDirectory%s : %s%s%s\n",
        g_color ? L"\x1b[33m" : L"",
        g_color ? L"\x1b[0m" : L"",
        g_color ? L"\x1b[36m" : L"",
        display_dir,
        g_color ? L"\x1b[0m" : L"");
    wprintf(L"  %sFormat%s    : %s%ls%s",
        g_color ? L"\x1b[33m" : L"",
        g_color ? L"\x1b[0m" : L"",
        g_color ? L"\x1b[36m" : L"",
        cfg.fmt,
        g_color ? L"\x1b[0m" : L"");
    wprintf(L"   %sBPP%s: ", g_color ? L"\x1b[33m" : L"", g_color ? L"\x1b[0m" : L"");
    if (cfg.bpp == 'P') wprintf(L"P"); else wprintf(L"%d", cfg.bpp);
    wprintf(L" (%s)\n", bpp_str);
    if (wcscmp(cfg.fmt, L"webp") == 0) {
        wprintf(L"  %sWebP%s    : %s%d%s %s\n",
            g_color ? L"\x1b[33m" : L"",
            g_color ? L"\x1b[0m" : L"",
            g_color ? L"\x1b[36m" : L"",
            cfg.webp_quality,
            g_color ? L"\x1b[0m" : L"",
            cfg.webp_lossless ? L"lossless" : L"(lossy, 1=small .. 100=best)");
    }
    wprintf(L"  %sNames%s     : %s%s%s\n",
        g_color ? L"\x1b[33m" : L"",
        g_color ? L"\x1b[0m" : L"",
        g_color ? L"\x1b[36m" : L"",
        cfg.name_mode,
        g_color ? L"\x1b[0m" : L"");
    wprintf(L"\n");
    wprintf(L"  %sWaiting for images in clipboard...  (Ctrl+C = exit)%s\n",
        g_color ? L"\x1b[90m" : L"", g_color ? L"\x1b[0m" : L"");
    wprintf(L"  Use %s to switch ON|OFF capturing\n", hotkey.display);
    wprintf(L"  %s-----------------------------------------------------------------%s\n",
        g_color ? L"\x1b[90m" : L"",
        g_color ? L"\x1b[0m" : L"");
    fflush(stdout);
}

/* ── Hotkey / Keys parsing ───────────────────────────────────────── */

static void die(const WCHAR* msg);

static const WCHAR* mod2_name(const WCHAR* low) {
    if (wcscmp(low, L"alt") == 0)        return L"Alt";
    if (wcscmp(low, L"leftalt") == 0)    return L"LeftAlt";
    if (wcscmp(low, L"rightalt") == 0)   return L"RightAlt";
    if (wcscmp(low, L"shift") == 0)      return L"Shift";
    if (wcscmp(low, L"leftshift") == 0)  return L"LeftShift";
    if (wcscmp(low, L"rightshift") == 0) return L"RightShift";
    return L"?";
}

static void parse_keys(const WCHAR* str) {
    WCHAR buf[128];
    wcscpy_s(buf, 128, str);

    WCHAR* parts[3];
    int n = 0;
    parts[n++] = buf;
    for (int i = 0; buf[i]; i++) {
        if (buf[i] == L'-') {
            buf[i] = 0;
            if (n >= 3) die(L"--keys: too many parts, use format MOD-MOD-KEY");
            parts[n++] = buf + i + 1;
        }
    }
    if (n != 3) die(L"--keys: expected 3 parts separated by '-' (e.g. CTRL-LEFTALT-F12)");

    WCHAR low[3][64];
    for (int i = 0; i < 3; i++) {
        wcscpy_s(low[i], 64, parts[i]);
        _wcslwr_s(low[i], wcslen(low[i]) + 1);
    }

    if (wcscmp(low[0], L"ctrl") != 0 && wcscmp(low[0], L"leftctrl") != 0 && wcscmp(low[0], L"rightctrl") != 0)
        die(L"--keys: first part must be CTRL, LeftCTRL, or RightCTRL");

    int mod2 = 0;
    if (wcscmp(low[1], L"alt") == 0 || wcscmp(low[1], L"leftalt") == 0 || wcscmp(low[1], L"rightalt") == 0)
        mod2 = MOD_ALT;
    else if (wcscmp(low[1], L"shift") == 0 || wcscmp(low[1], L"leftshift") == 0 || wcscmp(low[1], L"rightshift") == 0)
        mod2 = MOD_SHIFT;
    else
        die(L"--keys: second part must be ALT, LeftALT, RightALT, SHIFT, LeftSHIFT, or RightSHIFT");

    UINT vk;
    if (low[2][0] == L'f' && wcslen(low[2]) >= 2) {
        int fn = _wtoi(low[2] + 1);
        if (fn < 1 || fn > 12) die(L"--keys: F-key must be F1..F12");
        vk = VK_F1 + fn - 1;
    } else if (wcslen(low[2]) == 1 && low[2][0] >= L'a' && low[2][0] <= L'z') {
        vk = (UINT)(low[2][0] - L'a' + L'A');
    } else if (wcslen(low[2]) == 1 && low[2][0] >= L'0' && low[2][0] <= L'9') {
        vk = (UINT)low[2][0];
    } else {
        die(L"--keys: third part must be F1..F12, A..Z, or 0..9");
    }

    hotkey.mod = MOD_CONTROL | mod2;
    hotkey.vk   = vk;

    WCHAR keyname[16];
    if (vk >= VK_F1 && vk <= VK_F12)
        swprintf(keyname, 16, L"F%d", vk - VK_F1 + 1);
    else
        swprintf(keyname, 16, L"%c", (WCHAR)vk);

    swprintf(hotkey.display, 64, L"Ctrl-%s-%s", mod2_name(low[1]), keyname);
}

/* ── Argument parsing ──────────────────────────────────────────────── */

static void die(const WCHAR* msg) {
    wprintf(L"ERROR: %s\n", msg);
    exit(2);
}

static void HelpHeader() {
	wprintf(L"Author: Igor Brzezek, Version: %s, Date: %s, GitHub: https://github.com/IgorBrzezek/ClipSave\n\n", VERSION, DATE);
}

static void parse_args(int argc, WCHAR* argv[]) {
    for (int i = 1; i < argc; i++) {
        const WCHAR* arg = argv[i];

        if (wcscmp(arg, L"--help") == 0) {
            wprintf(L"\n");
            wprintf(L"ClipSave - Windows Clipboard Monitor\n\n");
			HelpHeader();
            wprintf(L"DESCRIPTION\n");
            wprintf(L"  Listens for clipboard image changes via AddClipboardFormatListener.\n");
            wprintf(L"  Saves images as PNG/JPEG/BMP/WebP with optional color depth conversion.\n\n");
            wprintf(L"USAGE\n");
            wprintf(L"  clipsave.exe [options]\n\n");
            wprintf(L"OPTIONS\n");
            wprintf(L"  -h               Short help\n");
            wprintf(L"  --help           This extended documentation\n");
            wprintf(L"  -d DIRECTORY     Target directory (default: .)\n");
            wprintf(L"  -f FORMAT        png | jpg | bmp | webp (default: png)\n");
            wprintf(L"  -c N             JPG: quality 0-100 (default: 95)\n");
            wprintf(L"                   PNG: compression 1-10, 1=fast..10=slow (default: 6)\n");
            wprintf(L"  --webpq N        WebP: quality 1-100 (default: 80)\n");
            wprintf(L"                   1 = smallest file / worst quality,\n");
            wprintf(L"                   100 = best quality / largest file\n");
            wprintf(L"  --webplossless   WebP: lossless encoding (perfect pixels)\n");
            wprintf(L"  --bpp N          Color depth: 8 | P | 16 | 24 (default: 24)\n");
            wprintf(L"                     8  - grayscale (mode L)\n");
            wprintf(L"                     P  - 8-bit palette, 256 colors (mode P)\n");
            wprintf(L"                     16 - RGB565 (5-6-5 bits per channel)\n");
            wprintf(L"                     24 - full RGB (8 bits per channel)\n");
            wprintf(L"  --name MODE      Naming scheme:\n");
            wprintf(L"                     DATETIME  - clip_YYYYMMDD_HHMMSS.fmt\n");
            wprintf(L"                     pattern   - custom with [N][NN][D][T][DT][TD]\n");
            wprintf(L"  --overwrite      Overwrite existing files without asking\n");
            wprintf(L"  --color          Colored terminal output\n");
            wprintf(L"  --beep           Beep on save (short) and on overwrite prompt (long)\n");
            wprintf(L"  --keys MOD-MOD-KEY  Custom hotkey (default: %s)\n", hotkey.display);
            wprintf(L"                     Example: CTRL-LEFTALT-F12, Ctrl-Shift-A\n\n");
            wprintf(L"TOGGLE\n");
            wprintf(L"  %s   Enable/disable clipboard capture on the fly\n\n", hotkey.display);
            wprintf(L"EXAMPLES\n");
            wprintf(L"  clipsave.exe\n");
            wprintf(L"  clipsave.exe -d C:\\Screenshots -f jpg --bpp 24\n");
            wprintf(L"  clipsave.exe -f bmp --bpp 8 --name scan[N]\n");
            wprintf(L"  clipsave.exe --name photo_[DT]_[NN]\n");
            wprintf(L"  clipsave.exe -f jpg -c 85\n");
            wprintf(L"  clipsave.exe -f webp --webpq 75\n");
            wprintf(L"  clipsave.exe -f webp --webplossless\n");
            wprintf(L"  clipsave.exe --keys CTRL-LEFTALT-F12\n\n");
            wprintf(L"STOPPING  Ctrl+C\n");
            wprintf(L"REQUIREMENTS  Windows 10/11\n");
            exit(0);
        }

        if (wcscmp(arg, L"-h") == 0) {
            wprintf(L"ClipSave - Windows Clipboard Monitor\n\n");
			HelpHeader();
            wprintf(L"Usage: clipsave.exe [-d DIR] [-f FMT] [--bpp N] [-c N] [--webpq N] [--webplossless] [--name MODE] [--overwrite] [--color] [--beep] [--keys MOD-MOD-KEY]\n");
            wprintf(L"  -h             This help\n");
            wprintf(L"  --help         Full documentation\n");
            wprintf(L"  -d DIR         Target directory (default: .)\n");
            wprintf(L"  -f FORMAT      png | jpg | bmp | webp (default: png)\n");
            wprintf(L"  --bpp N        8 | P | 16 | 24 (default: 24)\n");
            wprintf(L"  -c N           JPG: quality 0-100 / PNG: compression 1-10\n");
            wprintf(L"  --webpq N      WebP: quality 1-100 (default: 80), 1=smallest..100=best\n");
            wprintf(L"  --webplossless WebP: lossless (perfect pixels, larger file)\n");
            wprintf(L"  --name MODE    DATETIME | pattern with [N] [D] [T]\n");
            wprintf(L"  --overwrite    Overwrite without asking\n");
            wprintf(L"  --color        Colored terminal output\n");
            wprintf(L"  --beep         Beep on save / overwrite prompt\n");
            wprintf(L"  --keys KEYS    Custom hotkey (default: %s)\n", hotkey.display);
            wprintf(L"  %s Toggle capture on/off\n", hotkey.display);
            exit(0);
        }

        if (wcscmp(arg, L"-d") == 0 || wcscmp(arg, L"--dir") == 0) {
            if (++i >= argc) die(L"-d requires argument DIRECTORY");
            wcscpy_s(cfg.directory, MAX_PATH, argv[i]);
        }
        else if (wcscmp(arg, L"-f") == 0) {
            if (++i >= argc) die(L"-f requires argument FORMAT (png|jpg|bmp|webp)");
            WCHAR* val = argv[i];
            _wcslwr_s(val, wcslen(val) + 1);
            if (wcscmp(val, L"png") && wcscmp(val, L"jpg") &&
                wcscmp(val, L"bmp") && wcscmp(val, L"webp"))
                die(L"Unknown format. Allowed: png, jpg, bmp, webp");
            wcscpy_s(cfg.fmt, 8, val);
            _wcslwr_s(cfg.fmt, 8);
        }
        else if (wcscmp(arg, L"--webpq") == 0) {
            if (++i >= argc) die(L"--webpq requires an argument N (1-100)");
            int v = _wtoi(argv[i]);
            if (v < WEBP_MIN_QUALITY || v > WEBP_MAX_QUALITY)
                die(L"--webpq: allowed values: 1-100");
            cfg.webp_quality = v;
        }
        else if (wcscmp(arg, L"--webplossless") == 0) {
            cfg.webp_lossless = 1;
        }
        else if (wcscmp(arg, L"--bpp") == 0) {
            if (++i >= argc) die(L"--bpp requires an argument (8|P|16|24)");
            if (_wcsicmp(argv[i], L"P") == 0) {
                cfg.bpp = 'P';
            } else {
                int v = _wtoi(argv[i]);
                if (v != 8 && v != 16 && v != 24)
                    die(L"--bpp: allowed values: 8, P, 16, 24");
                cfg.bpp = v;
            }
        }
        else if (wcscmp(arg, L"--name") == 0) {
            if (++i >= argc) die(L"--name requires an argument (DATETIME or pattern)");
            wcscpy_s(cfg.name_mode, 128, argv[i]);
        }
        else if (wcscmp(arg, L"-c") == 0 || wcscmp(arg, L"--compression") == 0) {
            if (++i >= argc) die(L"-c requires an argument N");
            int v = _wtoi(argv[i]);
            if (v < 0 || v > 100) die(L"-c: allowed values: 0-100");
            cfg.compression = v;
        }
        else if (wcscmp(arg, L"--overwrite") == 0) {
            cfg.overwrite = 1;
        }
        else if (wcscmp(arg, L"--color") == 0) {
            g_color = 1;
        }
        else if (wcscmp(arg, L"--beep") == 0) {
            g_beep = 1;
        }
        else if (wcscmp(arg, L"--keys") == 0) {
            if (++i >= argc) die(L"--keys requires an argument (e.g. CTRL-LEFTALT-F12)");
            parse_keys(argv[i]);
        }
        else {
            WCHAR err[256];
            swprintf(err, 256, L"Unknown argument: %s\nUse -h to see help.", arg);
            die(err);
        }
    }

    if (cfg.compression < 0) {
        cfg.compression = (wcscmp(cfg.fmt, L"jpg") == 0) ? 95 : 6;
    } else if (wcscmp(cfg.fmt, L"png") == 0) {
        cfg.compression = max(0, min(9, cfg.compression - 1));
    }
}

/* ── Entry point ─────────────────────────────────────────────────── */

int wmain(int argc, WCHAR* argv[]) {
    parse_args(argc, argv);

    /* Detect another ClipSave instance (C or Python) already running */
    {
        HWND h = FindWindowW(HIDDEN_CLASS, NULL);
        if (h) {
            wprintf(L"ERROR: Another ClipSave instance is already running.\n"
                    L"       Please close the existing instance first.\n"
                    L"       (Only one ClipSave instance may run at a time.)\n");
            return 1;
        }
    }

    gdiplus_init();
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    CreateDirectoryW(cfg.directory, NULL);
    clear_screen();
    print_banner();
    wprintf(L"\n");  /* blank line between header and file list */

    if (!create_listener_window()) {
        gdiplus_shutdown();
        return 1;
    }

    if (!RegisterHotKey(g_hwnd, HOTKEY_ID, hotkey.mod, hotkey.vk))
        wprintf(L"WARNING: RegisterHotKey failed (%lu)\n", GetLastError());

    message_loop();

    if (g_hwnd) DestroyWindow(g_hwnd);
    wprintf(L"\n  Finished.  Captured images: %d\n", counter);

    gdiplus_shutdown();
    return 0;
}
