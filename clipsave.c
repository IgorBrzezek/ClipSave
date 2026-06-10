/*
 * ClipSave - Windows clipboard monitor (ANSI C)
 * Captures images copied to the clipboard and saves them as files.
 * Mechanism: AddClipboardFormatListener (zero-polling, ~0% CPU).
 * Requires: Windows 10/11, GDI+ (built-in).
 *
 * Author:   Igor Brzeżek
 * Version:  0.1
 * GitHub:   https://github.com/IgorBrzezek/ClipSave
 *
 * Build:  gcc -O2 -municode clipsave.c -lgdiplus -lgdi32 -lole32 -luuid -o clipsave.exe
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

#pragma comment(lib, "gdiplus")
#pragma comment(lib, "ole32")
#pragma comment(lib, "uuid")

/* ── Constants ─────────────────────────────────────────────── */

#define APP_NAME L"ClipSave"
#define HIDDEN_CLASS L"ClipSaveHiddenWnd"

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
} Config;

static Config cfg = {
    L".",
    L"png",
    16,
    L"DATETIME",
    0,
    -1,
};

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

static int save_image(GpBitmap* img, const WCHAR* path) {
    CLSID clsid;
    const WCHAR* mime;

    if (wcscmp(cfg.fmt, L"jpg") == 0)
        mime = L"image/jpeg";
    else if (wcscmp(cfg.fmt, L"bmp") == 0)
        mime = L"image/bmp";
    else
        mime = L"image/png";

    if (!get_encoder_clsid(mime, &clsid))
        return 0;

    EncoderParameters enc_params;
    ULONG quality_val = (ULONG)cfg.compression;
    enc_params.Count = 0;

    if (wcscmp(cfg.fmt, L"jpg") == 0) {
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

/* ── Filename generation ──────────────────────────────────── */

static int counter = 0;

static void make_filename(WCHAR* out, size_t out_sz) {
    time_t t;
    struct tm tm;
    WCHAR stamp[64], date_str[16], time_str[16];

    time(&t);
    localtime_s(&tm, &t);

    swprintf(date_str, 16, L"%04d%02d%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    swprintf(time_str, 16, L"%02d%02d%02d",
        tm.tm_hour, tm.tm_min, tm.tm_sec);

    if (_wcsicmp(cfg.name_mode, L"DATETIME") == 0) {
        swprintf(stamp, 64, L"%s_%s_000", date_str, time_str);
        swprintf(out, out_sz, L"%s\\clip_%s.%s", cfg.directory, stamp, cfg.fmt);
        return;
    }

    WCHAR buf[256];
    wcscpy_s(buf, 256, cfg.name_mode);
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

    swprintf(out, out_sz, L"%s\\%s.%s", cfg.directory, buf, cfg.fmt);
}

/* ── Clipboard event handler ───────────────────────────────── */

static UINT64 last_hash = 0;

static void on_clipboard(void) {
    if (!OpenClipboard(NULL)) return;

    HANDLE h = GetClipboardData(CF_DIB);
    if (!h) { CloseClipboard(); return; }

    BITMAPINFO* bmi = (BITMAPINFO*)GlobalLock(h);
    if (!bmi) { CloseClipboard(); return; }

    HBITMAP hbm = NULL;
    GpBitmap* src_bmp = NULL;
    UINT w = bmi->bmiHeader.biWidth;
    UINT h_ = abs(bmi->bmiHeader.biHeight);

    if (bmi->bmiHeader.biSize >= sizeof(BITMAPINFOHEADER)) {
        HDC hdc = GetDC(NULL);
        void* bits = NULL;
        hbm = CreateDIBSection(hdc, bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (hbm && bits) {
            BYTE* src_pixels = (BYTE*)bmi + bmi->bmiHeader.biSize;
            if (bmi->bmiHeader.biBitCount <= 8) {
                src_pixels += (1 << bmi->bmiHeader.biBitCount) * sizeof(RGBQUAD);
            }
            size_t pitch = ((w * bmi->bmiHeader.biBitCount + 31) / 32) * 4;
            memcpy(bits, src_pixels, pitch * h_);
            GdipCreateBitmapFromHBITMAP(hbm, NULL, &src_bmp);
        }
        ReleaseDC(NULL, hdc);
    }

    GlobalUnlock(h);
    CloseClipboard();

    if (!src_bmp) {
        if (hbm) DeleteObject(hbm);
        return;
    }

    /* Deduplication */
    UINT64 hval = image_hash(src_bmp);
    if (hval == last_hash && last_hash != 0) {
        GdipDisposeImage((GpImage*)src_bmp);
        if (hbm) DeleteObject(hbm);
        return;
    }
    last_hash = hval;

    /* Apply color depth */
    GpBitmap* final_img = apply_bpp(src_bmp);
    GdipDisposeImage((GpImage*)src_bmp);

    if (!final_img) {
        if (hbm) DeleteObject(hbm);
        return;
    }

    /* Build path */
    WCHAR path[MAX_PATH];
    make_filename(path, MAX_PATH);

    /* Overwrite check */
    if (!cfg.overwrite) {
        FILE* f = _wfopen(path, L"rb");
        if (f) {
            fclose(f);
            const WCHAR* base = wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            wprintf(L"  [?] File exists: %s. Overwrite? [y/N] ", base);
            fflush(stdout);
            WCHAR ans[16];
            if (fgetws(ans, 16, stdin)) {
                if (ans[0] != L'y' && ans[0] != L'Y') {
                    wprintf(L"  [-] Skipped.\n");
                    GdipDisposeImage((GpImage*)final_img);
                    if (hbm) DeleteObject(hbm);
                    return;
                }
            }
        }
    }

    /* Save */
    if (!save_image(final_img, path)) {
        wprintf(L"  [!] Save error: %s\n", path);
        GdipDisposeImage((GpImage*)final_img);
        if (hbm) DeleteObject(hbm);
        return;
    }

    counter++;
    UINT img_w, img_h;
    GdipGetImageWidth((GpImage*)final_img, &img_w);
    GdipGetImageHeight((GpImage*)final_img, &img_h);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    UINT64 sz = 0;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        sz = ((UINT64)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    const WCHAR* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;

    if (sz < 1048576)
        wprintf(L"  [+] %s  [%ux%u px, %.1f KB]\n", base, img_w, img_h, sz / 1024.0);
    else
        wprintf(L"  [+] %s  [%ux%u px, %.1f MB]\n", base, img_w, img_h, sz / 1048576.0);
    fflush(stdout);

    GdipDisposeImage((GpImage*)final_img);
    if (hbm) DeleteObject(hbm);
}

/* ── Window procedure ─────────────────────────────────────── */

static HWND g_hwnd = NULL;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLIPBOARDUPDATE) {
        on_clipboard();
        return 0;
    }
    if (msg == WM_DESTROY) {
        RemoveClipboardFormatListener(hwnd);
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

/* ── Startup display ─────────────────────────────────────────────── */

static void print_banner(void) {
    const WCHAR* bpp_str;
    if (cfg.bpp == 8)       bpp_str = L"grayscale";
    else if (cfg.bpp == 16) bpp_str = L"RGB565";
    else if (cfg.bpp == 24) bpp_str = L"full RGB";
    else if (cfg.bpp == 'P') bpp_str = L"palette 256";
    else                    bpp_str = L"?";

    wprintf(L"\n");
    wprintf(L"  ===========================================\n");
    wprintf(L"    ClipSave - clipboard monitor active\n");
    wprintf(L"  ===========================================\n");
    wprintf(L"  Directory : %s\n", cfg.directory);
    wprintf(L"  Format    : %ls   BPP: ", cfg.fmt);
    if (cfg.bpp == 'P') wprintf(L"P"); else wprintf(L"%d", cfg.bpp);
    wprintf(L" (%s)\n", bpp_str);
    wprintf(L"  Names     : %s\n", cfg.name_mode);
    wprintf(L"\n");
    wprintf(L"  Waiting for images in clipboard...  (Ctrl+C = exit)\n");
    wprintf(L"  -----------------------------------------------\n");
    fflush(stdout);
}

/* ── Argument parsing ──────────────────────────────────────────────── */

static void die(const WCHAR* msg) {
    wprintf(L"ERROR: %s\n", msg);
    exit(2);
}

static void parse_args(int argc, WCHAR* argv[]) {
    for (int i = 1; i < argc; i++) {
        const WCHAR* arg = argv[i];

        if (wcscmp(arg, L"--help") == 0) {
            wprintf(L"\n");
            wprintf(L"ClipSave - Windows Clipboard Monitor  v0.1\n\n");
            wprintf(L"DESCRIPTION\n");
            wprintf(L"  Listens for clipboard image changes via AddClipboardFormatListener.\n");
            wprintf(L"  Saves images as PNG/JPEG/BMP with optional color depth conversion.\n\n");
            wprintf(L"USAGE\n");
            wprintf(L"  clipsave.exe [options]\n\n");
            wprintf(L"OPTIONS\n");
            wprintf(L"  -h               Short help\n");
            wprintf(L"  --help           This extended documentation\n");
            wprintf(L"  -d DIRECTORY     Target directory (default: .)\n");
            wprintf(L"  -f FORMAT        png | jpg | bmp (default: png)\n");
            wprintf(L"  -c N             JPG: quality 0-100 (default: 95)\n");
            wprintf(L"                   PNG: compression 1-10, 1=fast..10=slow (default: 6)\n");
            wprintf(L"  --bpp N          Color depth: 8 | P | 16 | 24 (default: 16)\n");
            wprintf(L"                     8  - grayscale (mode L)\n");
            wprintf(L"                     P  - 8-bit palette, 256 colors (mode P)\n");
            wprintf(L"                     16 - RGB565 (5-6-5 bits per channel)\n");
            wprintf(L"                     24 - full RGB (8 bits per channel)\n");
            wprintf(L"  --name MODE      Naming scheme:\n");
            wprintf(L"                     DATETIME  - clip_YYYYMMDD_HHMMSS.fmt\n");
            wprintf(L"                     pattern   - custom with [N][NN][D][T][DT][TD]\n");
            wprintf(L"  --overwrite      Overwrite existing files without asking\n\n");
            wprintf(L"EXAMPLES\n");
            wprintf(L"  clipsave.exe\n");
            wprintf(L"  clipsave.exe -d C:\\Screenshots -f jpg --bpp 24\n");
            wprintf(L"  clipsave.exe -f bmp --bpp 8 --name scan[N]\n");
            wprintf(L"  clipsave.exe --name photo_[DT]_[NN]\n");
            wprintf(L"  clipsave.exe -f jpg -c 85\n\n");
            wprintf(L"STOPPING  Ctrl+C\n");
            wprintf(L"REQUIREMENTS  Windows 10/11\n");
            exit(0);
        }

        if (wcscmp(arg, L"-h") == 0) {
            wprintf(L"ClipSave - capture images from Windows clipboard\n");
            wprintf(L"Usage: clipsave.exe [-d DIR] [-f FMT] [--bpp N] [-c N] [--name MODE] [--overwrite]\n");
            wprintf(L"  -h             This help\n");
            wprintf(L"  --help         Full documentation\n");
            wprintf(L"  -d DIR         Target directory (default: .)\n");
            wprintf(L"  -f FORMAT      png | jpg | bmp (default: png)\n");
            wprintf(L"  --bpp N        8 | P | 16 | 24 (default: 16)\n");
            wprintf(L"  -c N           JPG: quality 0-100 / PNG: compression 1-10\n");
            wprintf(L"  --name MODE    DATETIME | pattern with [N] [D] [T]\n");
            wprintf(L"  --overwrite    Overwrite without asking\n");
            exit(0);
        }

        if (wcscmp(arg, L"-d") == 0 || wcscmp(arg, L"--dir") == 0) {
            if (++i >= argc) die(L"-d requires argument DIRECTORY");
            wcscpy_s(cfg.directory, MAX_PATH, argv[i]);
        }
        else if (wcscmp(arg, L"-f") == 0) {
            if (++i >= argc) die(L"-f requires argument FORMAT (png|jpg|bmp)");
            WCHAR* val = argv[i];
            _wcslwr_s(val, wcslen(val) + 1);
            if (wcscmp(val, L"png") && wcscmp(val, L"jpg") && wcscmp(val, L"bmp"))
                die(L"Unknown format. Allowed: png, jpg, bmp");
            wcscpy_s(cfg.fmt, 8, val);
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
    gdiplus_init();
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    CreateDirectoryW(cfg.directory, NULL);
    print_banner();

    if (!create_listener_window()) {
        gdiplus_shutdown();
        return 1;
    }

    message_loop();

    if (g_hwnd) DestroyWindow(g_hwnd);
    wprintf(L"\n  Finished.  Captured images: %d\n", counter);

    gdiplus_shutdown();
    return 0;
}
