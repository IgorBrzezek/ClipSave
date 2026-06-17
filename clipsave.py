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
SCRIPT_VERSION = 0.4
SCRIPT_GITHUB = "https://github.com/IgorBrzezek/ClipSave"

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
  --color       Colored terminal output (ANSI)
  --beep        Beep on save and on overwrite prompt
  --keys KEYS   Hotkey combination (default: CTRL-Shift-F11)
                Format: MOD-MOD-KEY, e.g. CTRL-LeftALT-F12
                Modifiers: CTRL, LeftCTRL, RightCTRL, ALT, LeftALT,
                           RightALT, SHIFT, LeftSHIFT, RightSHIFT
                Key: F1-F12, A-Z, 0-9

Toggle: configurable hotkey (default: Ctrl+Shift+F11, use --keys)
"""

LONG_HELP = """\
===================================================================
          ClipSave - Windows Clipboard Monitor  v0.4
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

  --color         Colored terminal output (ANSI).
                  Banner, filenames (jpg=magenta, png=cyan, bmp=yellow)
                  and errors are colorized.

  --beep          Play a short beep on successful save and a long
                  beep when asking about overwriting an existing file.

  --keys KEYS     Hotkey combination for toggling capture on/off.
                  Format: MODIFIER-MODIFIER-KEY (3 hyphen-separated parts).
                  Modifiers: CTRL | LeftCTRL | RightCTRL
                             ALT  | LeftALT  | RightALT
                             SHIFT| LeftSHIFT| RightSHIFT
                  Key: F1-F12 | A-Z | 0-9
                  Default: CTRL-Shift-F11
                  Examples:
                    --keys CTRL-Shift-F12
                    --keys LeftCTRL-LeftALT-F5
                    --keys ALT-Shift-A

TOGGLE
  Ctrl+Shift+F11  Enable/disable clipboard capture on the fly
                  (customizable with --keys).
                  Status updates in-place on the banner's second line.

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
WM_HOTKEY = 0x0312

MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_SHIFT = 0x0004
MOD_LEFT = 0x8000
MOD_RIGHT = 0x4000
VK_F11 = 0x7A
HOTKEY_ID = 1

ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
STD_OUTPUT_HANDLE = -11
ERROR_ALREADY_EXISTS = 183

# Modifier name → (base_flag, side_flag)
_MOD_MAP = {
    "CTRL":      (MOD_CONTROL, 0),
    "LEFTCTRL":  (MOD_CONTROL, MOD_LEFT),
    "RIGHTCTRL": (MOD_CONTROL, MOD_RIGHT),
    "ALT":       (MOD_ALT, 0),
    "LEFTALT":   (MOD_ALT, MOD_LEFT),
    "RIGHTALT":  (MOD_ALT, MOD_RIGHT),
    "SHIFT":     (MOD_SHIFT, 0),
    "LEFTSHIFT": (MOD_SHIFT, MOD_LEFT),
    "RIGHTSHIFT":(MOD_SHIFT, MOD_RIGHT),
}

_MOD_DISPLAY = {
    "CTRL":"Ctrl", "LEFTCTRL":"LeftCtrl", "RIGHTCTRL":"RightCtrl",
    "ALT":"Alt", "LEFTALT":"LeftAlt", "RIGHTALT":"RightAlt",
    "SHIFT":"Shift", "LEFTSHIFT":"LeftShift", "RIGHTSHIFT":"RightShift",
}


def parse_hotkey(s):
    """Parse --keys string.  Returns (mod_flags, vk, display)."""
    parts = s.split("-")
    if len(parts) != 3:
        raise ValueError("--keys needs exactly 3 hyphen-separated parts, "
                         "e.g. CTRL-Shift-F11")

    m1, m2, key = parts
    m1u, m2u, ku = m1.upper(), m2.upper(), key.upper()

    for label, raw in [("first", m1), ("second", m2)]:
        if raw.upper() not in _MOD_MAP:
            raise ValueError(f"Unknown {label} modifier '{raw}'. "
                             "Valid: CTRL, LeftCTRL, RightCTRL, ALT, "
                             "LeftALT, RightALT, SHIFT, LeftSHIFT, RightSHIFT")

    base1, side1 = _MOD_MAP[m1u]
    base2, side2 = _MOD_MAP[m2u]
    if base1 == base2:
        raise ValueError(f"Cannot use same modifier type twice "
                         f"(got '{m1}' and '{m2}')")
    mod_flags = base1 | base2 | side1 | side2

    # Key
    if ku.startswith("F") and 2 <= len(ku) <= 3:
        try:
            n = int(ku[1:])
        except ValueError:
            raise ValueError(f"Invalid F-key '{key}'")
        if not 1 <= n <= 12:
            raise ValueError(f"F-key number must be 1-12, got F{n}")
        vk = 0x70 + n - 1
        key_disp = ku
    elif len(ku) == 1 and "A" <= ku <= "Z":
        vk = ord(ku)
        key_disp = ku
    elif len(ku) == 1 and "0" <= ku <= "9":
        vk = ord(ku)
        key_disp = ku
    else:
        raise ValueError(f"Invalid key '{key}'. "
                         "Must be F1-F12, a letter A-Z, or a digit 0-9")

    d1 = _MOD_DISPLAY.get(m1u, m1u)
    d2 = _MOD_DISPLAY.get(m2u, m2u)
    return mod_flags, vk, f"{d1}-{d2}-{key_disp}"

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


class COORD(ctypes.Structure):
    _fields_ = [("X", wt.SHORT), ("Y", wt.SHORT)]


class SMALL_RECT(ctypes.Structure):
    _fields_ = [
        ("Left", wt.SHORT), ("Top", wt.SHORT),
        ("Right", wt.SHORT), ("Bottom", wt.SHORT),
    ]


class CONSOLE_SCREEN_BUFFER_INFO(ctypes.Structure):
    _fields_ = [
        ("dwSize",              COORD),
        ("dwCursorPosition",    COORD),
        ("wAttributes",         wt.WORD),
        ("srWindow",            SMALL_RECT),
        ("dwMaximumWindowSize", COORD),
    ]


# Set argtypes for console/hotkey API
kernel32.GetStdHandle.argtypes = [wt.DWORD]
kernel32.GetStdHandle.restype = wt.HANDLE

kernel32.GetConsoleMode.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
kernel32.GetConsoleMode.restype = wt.BOOL

kernel32.SetConsoleMode.argtypes = [wt.HANDLE, wt.DWORD]
kernel32.SetConsoleMode.restype = wt.BOOL

kernel32.GetConsoleScreenBufferInfo.argtypes = [
    wt.HANDLE, ctypes.POINTER(CONSOLE_SCREEN_BUFFER_INFO)
]
kernel32.GetConsoleScreenBufferInfo.restype = wt.BOOL

kernel32.SetConsoleCursorPosition.argtypes = [wt.HANDLE, COORD]
kernel32.SetConsoleCursorPosition.restype = wt.BOOL

kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
kernel32.GetModuleHandleW.restype = wt.HMODULE

kernel32.CreateMutexW.argtypes = [wt.LPVOID, wt.BOOL, wt.LPCWSTR]
kernel32.CreateMutexW.restype = wt.HANDLE

kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.CloseHandle.restype = wt.BOOL

kernel32.Beep.argtypes = [wt.DWORD, wt.DWORD]
kernel32.Beep.restype = wt.BOOL

user32.RegisterClassExW.argtypes = [ctypes.POINTER(WNDCLASSEXW)]
user32.RegisterClassExW.restype = wt.ATOM

user32.CreateWindowExW.argtypes = [
    wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID,
]
user32.CreateWindowExW.restype = wt.HWND

user32.AddClipboardFormatListener.argtypes = [wt.HWND]
user32.AddClipboardFormatListener.restype = wt.BOOL

user32.RemoveClipboardFormatListener.argtypes = [wt.HWND]
user32.RemoveClipboardFormatListener.restype = wt.BOOL

user32.RegisterHotKey.argtypes = [wt.HWND, ctypes.c_int, ctypes.c_uint, ctypes.c_uint]
user32.RegisterHotKey.restype = wt.BOOL

user32.PeekMessageW.argtypes = [
    ctypes.POINTER(wt.MSG), wt.HWND, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint,
]
user32.PeekMessageW.restype = wt.BOOL

user32.TranslateMessage.argtypes = [ctypes.POINTER(wt.MSG)]
user32.TranslateMessage.restype = wt.BOOL

user32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
user32.DispatchMessageW.restype = LRESULT

user32.PostQuitMessage.argtypes = [ctypes.c_int]
user32.PostQuitMessage.restype = None

user32.DestroyWindow.argtypes = [wt.HWND]
user32.DestroyWindow.restype = wt.BOOL


# ── Main class ───────────────────────────────────────────────

class ClipSave:
    """Clipboard monitor - captures and saves images."""

    # LUT tables for RGB565 quantization (16 bpp)
    _LUT_R5 = [(v >> 3) << 3 for v in range(256)]
    _LUT_G6 = [(v >> 2) << 2 for v in range(256)]
    _LUT_B5 = [(v >> 3) << 3 for v in range(256)]

    def __init__(self, directory, fmt, bpp, name_mode, overwrite, compression,
                 color=False, beep=False, keys_modifiers=None, keys_vk=None, keys_display=None):
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
        self.color = color
        self.beep = beep
        self.active = True
        self._status_row = -1
        self._hotkey_modifiers = keys_modifiers if keys_modifiers is not None else (MOD_CONTROL | MOD_SHIFT)
        self._hotkey_vk = keys_vk if keys_vk is not None else VK_F11
        self._hotkey_display = keys_display if keys_display is not None else "Ctrl-Shift-F11"

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
        if not self.active:
            return

        try:
            img = ImageGrab.grabclipboard()
        except Exception:
            return

        if not isinstance(img, Image.Image):
            return

        try:
            img.load()
        except Exception:
            return
        ihash = self._image_hash(img)
        if ihash == self._last_hash:
            return
        self._last_hash = ihash

        img = self._apply_bpp(img)
        fpath = self._make_filename()

        if fpath.exists() and not self.overwrite:
            self._beep(300, 300)
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
        except Exception:
            if self.color:
                print(f"  \x1b[31m[!] Save error: {fpath.name} - WRITE ERROR!\x1b[0m")
            else:
                print(f"  [!] Save error: {fpath.name}")
            return

        self.counter += 1
        w, h = img.size
        sz = fpath.stat().st_size
        if sz < 1_048_576:
            size_str = f"{sz / 1024:.1f} KB"
        else:
            size_str = f"{sz / 1_048_576:.1f} MB"
        if self.color:
            fmt_color = {"jpg": "\x1b[35m", "png": "\x1b[36m", "bmp": "\x1b[33m"}
            c = fmt_color.get(self.fmt, "")
            print(f"  [+] [{self.counter}] {c}{fpath.name}\x1b[0m  [{w}x{h} px, {size_str}]")
        else:
            print(f"  [+] [{self.counter}] {fpath.name}  [{w}x{h} px, {size_str}]")
        sys.stdout.flush()
        self._beep(1500, 100)

    # ── console helpers ─────────────────────────────────────

    def _enable_ansi(self):
        """Enable ANSI virtual terminal processing for colored output."""
        hcon = kernel32.GetStdHandle(STD_OUTPUT_HANDLE)
        mode = wt.DWORD()
        if kernel32.GetConsoleMode(hcon, ctypes.byref(mode)):
            kernel32.SetConsoleMode(hcon, mode.value | ENABLE_VIRTUAL_TERMINAL_PROCESSING)

    def _update_status_line(self):
        """Update the ON/OFF status on the banner's second line in-place."""
        if self._status_row < 0:
            return
        hcon = kernel32.GetStdHandle(STD_OUTPUT_HANDLE)
        csbi = CONSOLE_SCREEN_BUFFER_INFO()
        if kernel32.GetConsoleScreenBufferInfo(hcon, ctypes.byref(csbi)):
            saved = COORD(csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y)
            target = COORD(0, self._status_row)
            kernel32.SetConsoleCursorPosition(hcon, target)
            if self.color:
                onoff = "\x1b[32mON" if self.active else "\x1b[31mOFF"
                print(f"    \x1b[1;37mClipSave - clipboard monitor active: {onoff}\x1b[0m  ", end="", flush=True)
            else:
                print(f"    ClipSave - clipboard monitor active: {'ON' if self.active else 'OFF'}  ", end="", flush=True)
            kernel32.SetConsoleCursorPosition(hcon, saved)

    def _beep(self, freq, dur):
        if self.beep:
            kernel32.Beep(freq, dur)

    # ── Windows message loop ───────────────────────────────────

    def _wnd_proc(self, hwnd, msg, wparam, lparam):
        """Windows message callback."""
        if msg == WM_CLIPBOARDUPDATE:
            self._on_clipboard()
            return 0
        if msg == WM_HOTKEY and wparam == HOTKEY_ID:
            self.active = not self.active
            self._update_status_line()
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

        if self.color:
            self._enable_ansi()
            S = "\x1b[90m"; R = "\x1b[0m"
            B = "\x1b[1;37m"; G = "\x1b[32m"
            Y = "\x1b[33m"; C = "\x1b[36m"
        else:
            S = R = B = G = Y = C = ""

        print()
        print(f"{S}  =========================================================={R}")
        onoff = f"{G}ON{R}" if self.color else "ON"
        print(f"    {B}ClipSave - clipboard monitor active: {onoff}")
        print(f"{S}    v0.4 - Igor Brzeżek - github.com/IgorBrzezek/ClipSave{R}")
        print(f"{S}  =========================================================={R}")
        print(f"  {Y}Directory{R} : {C}{self.directory}{R}")
        bpp_str = bpp_desc[self.bpp]
        print(f"  {Y}Format{R}    : {C}{self.fmt.upper()}{R}   {Y}BPP{R}: {self.bpp} ({bpp_str})")
        print(f"  {Y}Names{R}     : {C}{self.name_mode}{R}")
        print()
        print(f"{S}  Waiting for images in clipboard...  (Ctrl+C = exit){R}")
        print(f"  Use {self._hotkey_display} to switch ON|OFF capturing")
        print(f"{S}  ----------------------------------------------------------{R}")
        sys.stdout.flush()

        # Store cursor position for banner updates (title line is 10 rows up)
        hcon = kernel32.GetStdHandle(STD_OUTPUT_HANDLE)
        csbi = CONSOLE_SCREEN_BUFFER_INFO()
        if kernel32.GetConsoleScreenBufferInfo(hcon, ctypes.byref(csbi)):
            self._status_row = csbi.dwCursorPosition.Y - 10

        self._create_listener_window()
        if not user32.RegisterHotKey(self._hwnd, HOTKEY_ID,
                                     self._hotkey_modifiers, self._hotkey_vk):
            base = self._hotkey_modifiers & ~(MOD_LEFT | MOD_RIGHT)
            if base != self._hotkey_modifiers:
                if not user32.RegisterHotKey(self._hwnd, HOTKEY_ID,
                                             base, self._hotkey_vk):
                    pass  # still fails — hotkey taken
            else:
                pass  # hotkey taken, no side flags to strip

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
        "color": False,
        "beep": False,
        "keys_modifiers": MOD_CONTROL | MOD_SHIFT,
        "keys_vk": VK_F11,
        "keys_display": "Ctrl-Shift-F11",
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

        elif arg == "--color":
            cfg["color"] = True

        elif arg == "--beep":
            cfg["beep"] = True

        elif arg == "--keys":
            i += 1
            if i >= len(argv):
                _die("--keys requires an argument, e.g. CTRL-Shift-F11")
            try:
                mod, vk, disp = parse_hotkey(argv[i])
            except ValueError as e:
                _die(str(e))
            cfg["keys_modifiers"] = mod
            cfg["keys_vk"] = vk
            cfg["keys_display"] = disp

        else:
            _die(f"Unknown argument: {arg}\nUse -h to see help.")

        i += 1

    return cfg


# ── Single-instance check ──────────────────────────────────────

_mutex_handle = None

MUTEX_NAME = "Local\\ClipSave_SingleInstanceMutex"


def _check_single_instance():
    global _mutex_handle
    _mutex_handle = kernel32.CreateMutexW(None, False, MUTEX_NAME)
    if not _mutex_handle:
        return
    if ctypes.GetLastError() == ERROR_ALREADY_EXISTS:
        kernel32.CloseHandle(_mutex_handle)
        _mutex_handle = None
        print("ClipSave", file=sys.stderr)
        print("       Please close the existing instance first.       (Only one ClipSave instance may run at a time.)", file=sys.stderr)
        sys.exit(1)


# ── Entry point ────────────────────────────────────────────────

def main():
    _check_single_instance()
    cfg = parse_args(sys.argv[1:])
    monitor = ClipSave(
        directory=cfg["directory"],
        fmt=cfg["fmt"],
        bpp=cfg["bpp"],
        name_mode=cfg["name"],
        overwrite=cfg["overwrite"],
        compression=cfg["compression"],
        color=cfg["color"],
        beep=cfg["beep"],
        keys_modifiers=cfg["keys_modifiers"],
        keys_vk=cfg["keys_vk"],
        keys_display=cfg["keys_display"],
    )
    monitor.run()


if __name__ == "__main__":
    main()
