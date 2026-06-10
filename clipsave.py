#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ClipSave - Windows clipboard monitor
Captures images copied to the clipboard (Snipping Tool / Win+Shift+S /
Print Screen / Ctrl+C on image) and saves them as image files.

Mechanism: AddClipboardFormatListener (zero-polling, ~0% CPU).
Requirements: Python 3.7+, Pillow, Windows 10/11.
"""

SCRIPT_AUTH = "Igor Brzeżek"
SCRIPT_VERSION = 0.1
SCRIP_GITHUB = "https://github.com/IgorBrzezek/ClipSave"

import sys
import os
import re
import time
import hashlib
import ctypes
import ctypes.wintypes as wt
from datetime import datetime
from pathlib import Path

# Force UTF-8 on stdout/stderr (Windows cp1250 fix)
if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# Platform check
if sys.platform != "win32":
    print("ClipSave works only on Windows.")
    sys.exit(1)

try:
    from PIL import Image, ImageGrab
    from PIL.PngImagePlugin import PngInfo
except ImportError:
    print("Pillow library not found.  Install:  pip install Pillow")
    sys.exit(1)


# ── Help texts ──────────────────────────────────────────────

SHORT_HELP = """\
ClipSave - capture images from Windows clipboard

Usage:  python clipsave.py [-d DIRECTORY] [-f FORMAT] [--bpp N] [--name MODE]

  -h            This help (short)
  --help        Full documentation with examples
  -d DIRECTORY    Target save directory (default: .)
  -f FORMAT     png | jpg | bmp  (default: png)
  --bpp N       Color depth: 8 | 16 | 24 | P  (default: 16)
  -c N          JPG: quality 0-100 / PNG: compression 1-10  (see --help)
  --name MODE   DATETIME | pattern with [N] [D] [T] (default: DATETIME)
  --overwrite   Overwrite existing files without asking
"""

LONG_HELP = """\
===================================================================
          ClipSave - Windows Clipboard Monitor  v1.0
===================================================================

DESCRIPTION
  ClipSave registers as a Windows clipboard listener via the native
  AddClipboardFormatListener API.  When an image appears in the
  clipboard (Snipping Tool, Win+Shift+S, PrintScreen, Ctrl+C on image),
  the program automatically saves it as an image file.

  Zero polling -- CPU usage while waiting is ~0%%.

USAGE
  python clipsave.py [options]

OPTIONS
  -h              Short help
  --help          This extended documentation

  -d DIRECTORY    Target directory.  Default: current directory (.).
                  Created automatically if it does not exist.

  -f FORMAT       Save format:
                    png - lossless, default
                    jpg - lossy JPEG compression
                    bmp - Windows Bitmap, uncompressed

  -c N            Compression level:
                    JPG: quality 0-100 (default: 95)
                    PNG: level 1-10, where 1=fast/large..10=slow/small (default: 6)

  --bpp N         Color depth (bits per pixel):
                     8  - grayscale (mode L)
                     16 - RGB565 reduction (5-6-5 bits per channel)
                     24 - full RGB color (3x8 bits)
                     P  - 8-bit palette, 256 colors (mode P)
                   Default: 16.

  --name MODE     Naming scheme:
                    DATETIME   - clip_YYYYMMDD_HHMMSS_mmm.fmt
                    pattern    - custom with tokens: [N] [NN] [D] [T] [DT] [TD]
                  Default: DATETIME.

                  Tokens in custom pattern:
                    [N]   - number 1,2,3...
                    [NN]  - number with leading zeros 01,02,03...
                    [NNN] - 001,002,003... (any N count)
                    [D]   - date YYYYMMDD
                    [T]   - time HHMMSS
                    [DT]  - date_time YYYYMMDD_HHMMSS
                    [TD]  - time_date HHMMSS_YYYYMMDD

  --overwrite     Overwrite existing files without asking.
                  Default: ask before overwriting.

EXAMPLES
  python clipsave.py
      PNG, 16 bpp, current directory.

  python clipsave.py -d C:\\Screenshots -f jpg --bpp 24
      Full-color JPEG to C:\\Screenshots.

  python clipsave.py -f bmp --bpp 8 --name scan[N]
      BMP grayscale, files: scan1.bmp, scan2.bmp...

STOPPING
  Ctrl+C

REQUIREMENTS
  Python 3.7+, Pillow (pip install Pillow), Windows 10/11.
"""


# ── Windows API ─────────────────────────────────────────────────

WM_CLIPBOARDUPDATE = 0x031D
WM_DESTROY = 0x0002

# LRESULT = c_ssize_t (8 bytes on x64, 4 on x86) — correct size
LRESULT = ctypes.c_ssize_t

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

# Set argtypes/restype for DefWindowProcW — critical on x64
user32.DefWindowProcW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = LRESULT

WNDPROCTYPE = ctypes.WINFUNCTYPE(
    LRESULT, wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM
)


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [
        ("cbSize",        ctypes.c_uint),
        ("style",         ctypes.c_uint),
        ("lpfnWndProc",   WNDPROCTYPE),
        ("cbClsExtra",    ctypes.c_int),
        ("cbWndExtra",    ctypes.c_int),
        ("hInstance",     wt.HINSTANCE),
        ("hIcon",         wt.HICON),
        ("hCursor",       wt.HANDLE),
        ("hbrBackground", wt.HBRUSH),
        ("lpszMenuName",  wt.LPCWSTR),
        ("lpszClassName", wt.LPCWSTR),
        ("hIconSm",       wt.HICON),
    ]


# ── Main class ───────────────────────────────────────────────

class ClipSave:
    """Clipboard monitor - captures and saves images."""

    # LUT tables for RGB565 quantization (16 bpp)
    _LUT_R5 = [(v >> 3) << 3 for v in range(256)]
    _LUT_G6 = [(v >> 2) << 2 for v in range(256)]
    _LUT_B5 = [(v >> 3) << 3 for v in range(256)]

    def __init__(self, directory, fmt, bpp, name_mode, overwrite, compression):
        self.directory = Path(directory).resolve()
        self.fmt = fmt.lower()
        self.bpp = bpp
        self.name_mode = name_mode
        self.overwrite = overwrite
        if compression == -1:
            self.compression = 95 if self.fmt == "jpg" else 6
        elif self.fmt == "png":
            self.compression = max(0, min(9, compression - 1))
        else:
            self.compression = compression
        self.counter = 0
        self._last_hash = None
        self._hwnd = None
        self._wndproc_ref = None  # prevent GC of the callback

        self.directory.mkdir(parents=True, exist_ok=True)

    # ── naming ────────────────────────────────────────────

    def _make_filename(self):
        now = datetime.now()
        if self.name_mode.upper() == "DATETIME":
            ms = f"{now.microsecond // 1000:03d}"
            stamp = now.strftime("%Y%m%d_%H%M%S_") + ms
            base = f"clip_{stamp}"
        else:
            mode = self.name_mode
            date_str = now.strftime("%Y%m%d")
            time_str = now.strftime("%H%M%S")

            mode = mode.replace("[DT]", f"{date_str}_{time_str}")
            mode = mode.replace("[TD]", f"{time_str}_{date_str}")
            mode = mode.replace("[D]", date_str)
            mode = mode.replace("[T]", time_str)

            def _num_repl(m):
                w = len(m.group(0)) - 2
                return f"{self.counter + 1:0{w}d}"

            mode = re.sub(r'\[N+\]', _num_repl, mode)
            base = mode
        return self.directory / f"{base}.{self.fmt}"

    # ── color depth conversion ────────────────────────────────

    def _apply_bpp(self, img):
        """Convert image according to color depth setting."""
        # Normalize to RGB (RGBA -> white background)
        if img.mode == "RGBA":
            bg = Image.new("RGB", img.size, (255, 255, 255))
            bg.paste(img, mask=img.split()[3])
            img = bg
        elif img.mode != "RGB":
            img = img.convert("RGB")

        if self.bpp == 8:
            return img.convert("L")
        if self.bpp == "P":
            return img.quantize(colors=256)
        if self.bpp == 16:
            r, g, b = img.split()
            r = r.point(self._LUT_R5)
            g = g.point(self._LUT_G6)
            b = b.point(self._LUT_B5)
            if self.fmt == "bmp":
                rb = r.tobytes()
                gb = g.tobytes()
                bb = b.tobytes()
                pixels = bytearray()
                for i in range(len(rb)):
                    b5 = bb[i] >> 3
                    g6 = gb[i] >> 2
                    r5 = rb[i] >> 3
                    val = b5 | (g6 << 5) | (r5 << 11)
                    pixels.append(val & 0xFF)
                    pixels.append((val >> 8) & 0xFF)
                return Image.frombytes("BGR;16", img.size, bytes(pixels))
            return Image.merge("RGB", (r, g, b))
        # 24 bpp - no change
        return img

    # ── deduplication ───────────────────────────────────────────

    @staticmethod
    def _image_hash(img):
        """Calculate image hash for deduplication."""
        h = hashlib.md5(usedforsecurity=False)
        h.update(f"{img.size[0]}x{img.size[1]}:{img.mode}".encode())
        raw = img.tobytes()
        h.update(raw[:8192])
        if len(raw) > 16384:
            mid = len(raw) // 2
            h.update(raw[mid:mid + 4096])
            h.update(raw[-4096:])
        return h.hexdigest()

    # ── clipboard event handler ──────────────────────────────

    def _on_clipboard(self):
        """Get image from clipboard and save."""
        try:
            img = ImageGrab.grabclipboard()
        except Exception:
            return

        if not isinstance(img, Image.Image):
            return

        ihash = self._image_hash(img)
        if ihash == self._last_hash:
            return
        self._last_hash = ihash

        img = self._apply_bpp(img)
        fpath = self._make_filename()

        if fpath.exists() and not self.overwrite:
            resp = input(f"  [?] File exists: {fpath.name}. Overwrite? [y/N] ")
            if resp.lower() != "y":
                print("  [-] Skipped.")
                return

        save_kw = {}
        if self.fmt == "jpg":
            save_kw = {"format": "JPEG", "quality": self.compression, "subsampling": 0}
        elif self.fmt == "png":
            save_kw = {"format": "PNG", "compress_level": self.compression}
            if self.bpp == 16:
                info = PngInfo()
                info.add(b"sBIT", bytes([5, 6, 5]))
                save_kw["pnginfo"] = info
        elif self.fmt == "bmp":
            save_kw = {"format": "BMP"}

        try:
            img.save(str(fpath), **save_kw)
        except Exception as e:
            print(f"  [!] Save error: {e}")
            return

        self.counter += 1
        w, h = img.size
        sz = fpath.stat().st_size
        if sz < 1_048_576:
            size_str = f"{sz / 1024:.1f} KB"
        else:
            size_str = f"{sz / 1_048_576:.1f} MB"
        print(f"  [+] {fpath.name}  [{w}x{h} px, {size_str}]")
        sys.stdout.flush()

    # ── Windows message loop ───────────────────────────────────

    def _wnd_proc(self, hwnd, msg, wparam, lparam):
        """Windows message callback."""
        if msg == WM_CLIPBOARDUPDATE:
            self._on_clipboard()
            return 0
        if msg == WM_DESTROY:
            user32.RemoveClipboardFormatListener(hwnd)
            user32.PostQuitMessage(0)
            return 0
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def _create_listener_window(self):
        """Create hidden window for clipboard event listening."""
        hinstance = kernel32.GetModuleHandleW(None)
        cls_name = "ClipSaveHiddenWnd"

        self._wndproc_ref = WNDPROCTYPE(self._wnd_proc)

        wc = WNDCLASSEXW()
        wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
        wc.lpfnWndProc = self._wndproc_ref
        wc.hInstance = hinstance
        wc.lpszClassName = cls_name

        if not user32.RegisterClassExW(ctypes.byref(wc)):
            raise OSError(f"RegisterClassExW error {ctypes.GetLastError()}")

        self._hwnd = user32.CreateWindowExW(
            0, cls_name, "ClipSave", 0,
            0, 0, 0, 0, None, None, hinstance, None,
        )
        if not self._hwnd:
            raise OSError(f"CreateWindowExW error {ctypes.GetLastError()}")

        if not user32.AddClipboardFormatListener(self._hwnd):
            raise OSError(
                f"AddClipboardFormatListener error {ctypes.GetLastError()}"
            )

    WM_QUIT = 0x0012

    def _message_loop(self):
        """Windows message loop."""
        msg = wt.MSG()
        while True:
            while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):  # PM_REMOVE
                if msg.message == self.WM_QUIT:
                    return
                user32.TranslateMessage(ctypes.byref(msg))
                user32.DispatchMessageW(ctypes.byref(msg))
            time.sleep(0.05)

    # ── startup ───────────────────────────────────────────

    def run(self):
        """Run clipboard monitor."""
        bpp_desc = {8: "grayscale", "P": "palette 256", 16: "RGB565", 24: "full RGB"}
        print()
        print("  ===============================================")
        print("    ClipSave - clipboard monitor active")
        print("  ===============================================")
        print(f"  Directory : {self.directory}")
        print(f"  Format  : {self.fmt.upper()}   BPP: {self.bpp} ({bpp_desc[self.bpp]})")
        print(f"  Names   : {self.name_mode}")
        print()
        print("  Waiting for images in clipboard...  (Ctrl+C = exit)")
        print("  " + "-" * 47)
        sys.stdout.flush()

        self._create_listener_window()

        try:
            self._message_loop()
        except KeyboardInterrupt:
            pass
        finally:
            if self._hwnd:
                user32.DestroyWindow(self._hwnd)
            print(f"\n  Finished.  Captured images: {self.counter}")


# ── Argument parsing ──────────────────────────────────────

def _die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(2)


def parse_args(argv):
    """Parse command line arguments."""
    if "--help" in argv:
        print(LONG_HELP)
        sys.exit(0)
    if "-h" in argv:
        print(SHORT_HELP)
        sys.exit(0)

    VALID_FMT = {"png", "jpg", "bmp"}
    VALID_BPP = {8, 16, 24}

    cfg = {
        "directory": ".",
        "fmt": "png",
        "bpp": 16,
        "name": "DATETIME",
        "overwrite": False,
        "compression": -1,
    }

    i = 0
    while i < len(argv):
        arg = argv[i]

        if arg in ("-d", "--dir"):
            i += 1
            if i >= len(argv):
                _die("-d requires argument DIRECTORY")
            cfg["directory"] = argv[i]

        elif arg == "-f":
            i += 1
            if i >= len(argv):
                _die("-f requires argument FORMAT (png|jpg|bmp)")
            val = argv[i].lower()
            if val not in VALID_FMT:
                _die(f"Unknown format '{val}'. Allowed: png, jpg, bmp")
            cfg["fmt"] = val

        elif arg == "--bpp":
            i += 1
            if i >= len(argv):
                _die("--bpp requires an argument (8|16|24|P)")
            val = argv[i]
            if val.upper() == "P":
                cfg["bpp"] = "P"
            else:
                try:
                    val = int(val)
                except ValueError:
                    _die(f"--bpp: '{argv[i]}' is not valid (allowed: 8, 16, 24, P)")
                if val not in VALID_BPP:
                    _die(f"--bpp: {val} -- allowed values: 8, 16, 24, P")
                cfg["bpp"] = val

        elif arg == "--name":
            i += 1
            if i >= len(argv):
                _die("--name requires an argument (DATETIME or pattern with [N][D][T])")
            cfg["name"] = argv[i]

        elif arg in ("-c", "--compression"):
            i += 1
            if i >= len(argv):
                _die("-c requires an argument N")
            try:
                val = int(argv[i])
            except ValueError:
                _die(f"-c: '{argv[i]}' is not a number")
            cfg["compression"] = val

        elif arg == "--overwrite":
            cfg["overwrite"] = True

        else:
            _die(f"Unknown argument: {arg}\nUse -h to see help.")

        i += 1

    return cfg


# ── Entry point ────────────────────────────────────────────────

def main():
    cfg = parse_args(sys.argv[1:])
    monitor = ClipSave(
        directory=cfg["directory"],
        fmt=cfg["fmt"],
        bpp=cfg["bpp"],
        name_mode=cfg["name"],
        overwrite=cfg["overwrite"],
        compression=cfg["compression"],
    )
    monitor.run()


if __name__ == "__main__":
    main()
