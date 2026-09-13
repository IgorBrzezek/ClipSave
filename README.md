# ClipSave

**ClipSave** is a lightweight Windows clipboard monitor that automatically captures images copied to the clipboard and saves them as files.

Triggered by: Snipping Tool, Win+Shift+S, Print Screen, Ctrl+C on an image, or any other action that places an image on the clipboard.

Uses the native `AddClipboardFormatListener` API — **zero polling, ~0% CPU** while waiting.

![Example screen of clipsave.exe](images/clipsave.webp)

---

# Author

- SCRIPT_AUTH = "Igor Brzeżek"
- SCRIPT_VERSION = 0.9
- SCRIPT_DATE = 13.09.2026
- SCRIPT_GITHUB = "https://github.com/IgorBrzezek/ClipSave"

---

# Python version

## Requirements

- Python version:
- **Python** 3.7 or newer
- **Pillow** (`pip install Pillow`)
- **Windows** 10 or 11

- ANSI C version:
- **Windows** 10 or 11
- To **build** (two routes):
  1. **`compile.cmd` (recommended, clean Windows)** — needs only a plain **MinGW-w64 gcc** on PATH (`gcc.exe` + `ar.exe`). The script downloads the libwebp source, builds the static library itself, and compiles `clipsave.exe` — **no cmake, no make, no MSYS2 needed**. See *Build — Windows, clean system* below.
  2. **MSYS2 UCRT64** with `mingw-w64-ucrt-x86_64-gcc`, `cmake`, `make` (libwebp is built from source and statically linked into the exe — see *Build on Windows* below)
- To **run**: a single standalone `clipsave.exe` — no DLLs required (either route produces the same exe)

---

## Installation

```bash
pip install Pillow
python clipsave.py
```

---

## Usage

```bash
python clipsave.py [options]
```

### Options

| Option | Description | Default |
|--------|-------------|---------|
| `-h` | Short help | — |
| `--help` | Full documentation with examples | — |
| `-d DIRECTORY` | Target save directory (created if missing) | `.` |
| `-f FORMAT` | Image format: `png`, `jpg`, `bmp`, `webp` | `png` |
| `--bpp N` | Color depth: `8` (grayscale), `P` (palette 256), `16` (RGB565), `24` (full RGB) | `24` |
| `-c N` / `--compression N` | Compression level — see below | format default |
| `--webpq N` | WebP: quality `1`–`100` (lossy mode only) | `80` |
| `--webplossless` | WebP: lossless encoding (VP8L, perfect pixels) | off |
| `--name MODE` | Naming scheme — see below | `DATETIME` |
| `--overwrite` | Overwrite existing files without asking | ask first |
| `--color` | Colored terminal output (ANSI) | off |
| `--beep` | Beep on save (short beep), on overwrite prompt (long beep), and on duplicate clipboard data (medium beep) | off |
| `--keys KEYS` | Hotkey combination (`MOD-MOD-KEY`, e.g. `CTRL-Shift-F11`, `LeftCTRL-LeftALT-F5`) | `CTRL-Shift-F11` |

### Toggle capture

Press the configured hotkey (default **Ctrl+Shift+F11**) to enable/disable clipboard capture on the fly. The current status (ON/OFF) updates in-place on the banner's second line. With `--color`, ON is green, OFF is red.

Customize with `--keys`:
```bash
python clipsave.py --keys LeftCTRL-LeftALT-F12
python clipsave.py --keys CTRL-ALT-A
python clipsave.py --keys ALT-Shift-5
```

Format: `MODIFIER-MODIFIER-KEY` (three hyphen-separated parts).
- **Modifiers:** `CTRL`, `LeftCTRL`, `RightCTRL`, `ALT`, `LeftALT`, `RightALT`, `SHIFT`, `LeftSHIFT`, `RightSHIFT`
- **Key:** `F1`–`F12`, `A`–`Z`, `0`–`9`

### Naming scheme (`--name`)

**`DATETIME`** — default mode. Produces filenames like `clip_20260610_112105_063.png`.

**Custom pattern** — any string with the following tokens:

| Token | Expands to |
|-------|-----------|
| `[N]` | Sequential number: `1`, `2`, `3`... |
| `[NN]` | Sequential number with leading zeros: `01`, `02`... |
| `[NNN]` | `001`, `002`... (any number of `N` chars) |
| `[D]` | Current date: `YYYYMMDD` |
| `[T]` | Current time: `HHMMSS` |
| `[DT]` | Date and time: `YYYYMMDD_HHMMSS` |
| `[TD]` | Time and date: `HHMMSS_YYYYMMDD` |

### Color depth (`--bpp`)

| Value | Description |
|-------|-------------|
| `8` | Grayscale (Pillow mode `L`) |
| `P` | 8-bit palette, 256 colors (Pillow mode `P`). Adaptive median-cut quantization with Floyd-Steinberg dithering. |
| `16` | RGB565 quantization (5 bits red, 6 bits green, 5 bits blue) — 65,536 colors. BMP files are saved as true 16-bit BITFIELDS; PNG files include an sBIT chunk to signal 5-6-5 significant bits. |
| `24` | Full RGB color (8 bits per channel) |

### Compression (`-c` / `--compression`)

Controls the file size / quality trade-off.

| Format | Range | Default | Notes |
|--------|-------|---------|-------|
| `jpg` | `0`–`100` | `95` | Quality percentage (higher = better quality, larger file). `0` = worst quality / smallest, `100` = best quality / largest. |
| `png` | `1`–`10` | `6` | Compression level (`1` = fastest / largest, `10` = slowest / smallest). Uses zlib `compress_level` mapped as `N-1` (Pillow range 0–9). |

WebP quality is controlled with `--webpq` (not `-c`), see the options table above.

### Overwrite behavior

By default ClipSave asks before overwriting an existing file:

```
  [?] File exists: clip_20260610_112105_063.png. Overwrite? [y/N]
```

Pass `--overwrite` to skip the prompt and always overwrite.

---

## Examples

**Default run** — PNG, full 24-bit RGB, current directory:
```bash
python clipsave.py
```

**Full-color JPEGs to a custom directory:**
```bash
python clipsave.py -d C:\Screenshots -f jpg --bpp 24
```

**Grayscale BMPs with sequential naming:**
```bash
python clipsave.py -f bmp --bpp 8 --name scan[N]
```
Produces `scan1.bmp`, `scan2.bmp`, ...

**Custom pattern with date and counter:**
```bash
python clipsave.py --name photo_[DT]_[NN]
```
Produces `photo_20260610_112105_01.png`, `photo_20260610_112106_02.png`, ...

**JPEG with custom quality:**
```bash
python clipsave.py -f jpg -c 85
```

**Maximum PNG compression (smallest files, slower):**
```bash
python clipsave.py -f png -c 10
```

**Lossy WebP at 75% quality** (small files, good quality):
```bash
python clipsave.py -f webp --webpq 75
```

**Lossless WebP (perfect pixels, larger file):**
```bash
python clipsave.py -f webp --webplossless
```

**Overwrite existing files without asking:**
```bash
python clipsave.py --overwrite
```

**Audible beep on capture:**
```bash
python clipsave.py --beep
python clipsave.py --beep --color
```

Three distinct beep sounds: short (1500 Hz) on save, medium (600 Hz) on duplicate clipboard data, long (300 Hz) on overwrite prompt.

**Custom hotkey:**
```bash
python clipsave.py --keys LeftCTRL-LeftALT-F12
python clipsave.py --keys CTRL-ALT-A --color
```

---

## Stopping

Press **Ctrl+C** in the terminal.

---

## How it works

ClipSave registers a hidden window with Windows via `AddClipboardFormatListener`. When the clipboard content changes, the system sends a `WM_CLIPBOARDUPDATE` message. ClipSave retrieves the image using Pillow's `ImageGrab.grabclipboard()`, applies the configured color depth conversion, and saves it to disk.

Because it uses the native listener API, there is **no polling loop** — CPU usage sits at ~0% while waiting for images.

---

## File format details

| Format | 8 bpp | P bpp | 16 bpp | 24 bpp |
|--------|-------|-------|--------|--------|
| PNG | Grayscale, 8-bit | Palette 256, 8-bit | RGB with sBIT (5,6,5) | RGB, 24-bit |
| JPEG | Grayscale | Palette → RGB | RGB, quantized colors | RGB, full color |
| BMP | Grayscale, 8-bit | Palette 256, 8-bit | **True 16-bit BITFIELDS** | RGB, 24-bit |
| WebP | Grayscale → RGB (VP8) | Palette → RGB (VP8) | RGB565 → RGB (VP8) | RGB, 24-bit (VP8) |

Both versions produce WebP. `--bpp` is applied first (so `--bpp 8` / `P` / `16` restrict colors before encoding) and the converted frame is written as 24-bit RGB, encoded lossy (VP8, quality `--webpq 1-100`) or lossless (`--webplossless`, VP8L — perfect pixels). Transparent images are flattened onto a white background.

---

## Project

- **Author:** Igor Brzeżek
- **Version:** 0.9 (Python) / 0.9 (ANSI C)
- **GitHub:** [https://github.com/IgorBrzezek/ClipSave](https://github.com/IgorBrzezek/ClipSave)

---

## ANSI C version

A standalone C port (`clipsave.c` + `webp_save.c`) with identical functionality — no Python or Pillow required. Saves images as **PNG / JPEG / BMP / WebP**.

WebP is a still-image format: unlike the old WebM output (VP9 in a video container, which many media players refused to show), WebP files open in any image viewer or web browser as a normal picture, with real image compression (lossy VP8 or lossless VP8L).

**Building** — three routes, each producing the same single `clipsave.exe` with libwebp linked statically (no DLLs at runtime):

| Route | What you need | Command |
|-------|---------------|---------|
| Windows, clean system | plain **MinGW-w64 gcc** (`gcc` + `ar`) on PATH | `compile.cmd` |
| Windows, MSYS2 UCRT64 | `mingw-w64-ucrt-x86_64-gcc`, `cmake`, `make` | `bash dl_webp.sh && bash build_webp_lib.sh && bash build_final.sh` |
| Linux → Windows exe | `sudo apt` + MinGW-w64 | `bash build_linux.sh` |

Full step-by-step instructions for each route are in the sections below.

### Requirements — Windows

Build inside **MSYS2 UCRT64** (install from <https://www.msys2.org>, then launch *MSYS2 UCRT64* from the Start menu). Install the toolchain and build tools:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc cmake make
```

libwebp is **not** taken from the MSYS2 package — the prebuilt `mingw-w64-ucrt-x86_64-libwebp` requires newer `gcc-libs` than the installed gcc, so libwebp is built **statically from source** (see below). Besides gcc you need `cmake` + `make` for that build and `curl` or `wget` to fetch the source.

| Library | Purpose |
|---------|---------|
| `gdiplus` | GDI+ — image encoding (PNG, JPEG, BMP) |
| `gdi32` | GDI — `CreateDIBSection`, `DeleteObject` |
| `ole32` | COM — GDI+ startup |
| `uuid` | UUID — encoder CLSID lookup |
| `webp` + `sharpyuv` | WebP encoder (static, built from source) |

GDI/GDI+/OLE32/UUID are built into Windows. Only **libwebp** is third-party, and it is linked statically — the resulting exe needs **no codec DLLs at runtime**.

### Build — Windows (step by step)

1. In the MSYS2 UCRT64 terminal, go to the project folder:
   ```bash
   cd /path/to/clipsave
   ```
2. Fetch and build libwebp statically from source (once):
   ```bash
   bash dl_webp.sh          # downloads libwebp-1.4.0.tar.gz (needs curl/wget)
   bash build_webp_lib.sh   # unpacks + cmake build
   ```
   This unpacks the tarball into `libwebp-1.4.0/`, runs CMake with all encoder tools disabled, and produces `build/libwebp.a` + `libsharpyuv.a`.
3. Compile:
   ```bash
   gcc -O2 -municode clipsave.c webp_save.c -Ilibwebp-1.4.0/src -Llibwebp-1.4.0/build \
       -lgdiplus -lgdi32 -lole32 -luuid -lwebp -lsharpyuv -lpthread -lm -o clipsave.exe
   ```
   Or use the ready script (also packs a zip):
   ```bash
   bash build_final.sh
   ```
4. Run: `./clipsave.exe -h`

Flag details:
- `-O2` — optimization level
- `-municode` — Unicode (`wmain`) entry point
- `-Ilibwebp-1.4.0/src -Llibwebp-1.4.0/build` — point the compiler/linker at the locally built static libwebp
- `webp_save.c` — WebP encoder wrapper around libwebp (required; omitting it gives `undefined reference to 'save_webp'`)
- `-l...` — system libraries; `-lwebp -lsharpyuv -lpthread -lm` are **required** for WebP
- `-f webp` output cannot work without webp headers (`webp/encode.h`) — build libwebp first

**No runtime DLLs:** libwebp is linked statically into `clipsave.exe`, so a single copy of the exe is all you need. No `libwebp-*.dll`, no `libvpx-1.dll`, no `libwinpthread-1.dll`.

### Build — Windows, clean system (`compile.cmd`)

If you don't want MSYS2, a plain **MinGW-w64 gcc** is enough. `compile.cmd` does the whole job on a clean Windows 10/11:

1. Downloads the **libwebp 1.4.0** source (`curl.exe` is built into Windows; falls back to PowerShell).
2. Extracts it (built-in `tar`, or PowerShell `Expand-Archive`).
3. Builds **`libwebp.a` + `libsharpyuv.a` statically** directly with `gcc` + `ar` — no CMake, no make, no configure.
4. Compiles **`clipsave.exe`** (`clipsave.c` + `webp_save.c`, WebP statically linked).

Steps:

1. Install a MinGW-w64 toolchain and put its `bin` folder on `PATH`:
   - **WinLibs** (easiest on a clean machine): https://winlibs.com — download a UCRT zip, unzip, add `mingw64\bin` to `PATH`.
   - **MSYS2** (without using its terminal): https://www.msys2.org — `pacman -S mingw-w64-ucrt-x86_64-gcc`, then `C:\msys64\ucrt64\bin` on `PATH`.
2. From the project folder run:
   ```bat
   compile.cmd
   ```
   First run downloads and builds libwebp (takes a minute or two). Re-runs skip that if `libwebp-1.4.0\build\libwebp.a` already exists.
3. Result: `clipsave.exe` in the same folder — single file, no DLLs. Test: `clipsave.exe -h` and `clipsave.exe -f webp --webpq 75`.

Error messages point to the log `libwebp-1.4.0\build\gcc.log` if the library build fails. If `curl`/`tar` are unavailable (very old Windows), download the zip manually into the project folder as `libwebp-1.4.0.zip` and re-run.

### Cross-compile — Linux → Windows exe

You can produce a Windows `clipsave.exe` from Linux. On Debian/Ubuntu the MinGW-w64 toolchain is available, but the distro does **not** ship a *libwebp* for MinGW, so libwebp must be built from source for the `win64` target first.

Step by step:

1. Install the toolchain and build tools:
   ```bash
   sudo apt update
   sudo apt install -y gcc-mingw-w64-x86-64 cmake make pkg-config git
   ```
2. Get and build libwebp for Windows:
   ```bash
   git clone --depth 1 -b v1.4.0 https://github.com/webmproject/libwebp
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
   ```
3. Compile ClipSave:
   ```bash
   x86_64-w64-mingw32-gcc -O2 -municode clipsave.c webp_save.c \
     -Ilibwebp/src -Llibwebp/build -lgdiplus -lgdi32 -lole32 -luuid \
     -lwebp -lsharpyuv -lpthread -lm -o clipsave.exe
   ```
4. The resulting `clipsave.exe` runs on Windows as a standalone binary — libwebp is linked statically, so **no DLLs** are needed.

The whole flow is available as a script:

```bash
bash build_linux.sh
```

Works as-is on Debian/Ubuntu: the source uses lowercase MinGW header names (`wincon.h`), which resolve correctly on Linux's case-sensitive file system as well as on Windows' case-insensitive one. The script builds `libwebp.a` from source, compiles a **PE32+ x86-64** `clipsave.exe`, and needs no manual fix-ups.

### Distribute (release)

`build_final.sh` packs a ready-to-run `release/clipsave-win64.zip` holding just `clipsave.exe`. Attach that zip to a **GitHub Release** — don't commit build artifacts into the repo (they are ignored via `.gitignore`).

### Usage — C version

Same command-line interface as the Python version:

```bash
clipsave.exe -d C:\Screenshots -f jpg --bpp 24 -c 90
clipsave.exe -f bmp --bpp 8 --name scan[N]
clipsave.exe --name photo_[DT]_[NN] --overwrite
clipsave.exe --color
clipsave.exe --beep
clipsave.exe --keys CTRL-LEFTALT-F12
clipsave.exe --keys LeftCTRL-RightALT-F5
clipsave.exe -f webp --webpq 75
clipsave.exe -f webp --webplossless
```

WebP extra options (identical in the Python version):

| Option | Description | Default |
|--------|-------------|---------|
| `-f webp` | Save as WebP (single-frame still image) | — |
| `--webpq N` | Quality `1`–`100` (`1` = smallest/worst, `100` = best/largest, JPEG-like scale) | `80` |
| `--webplossless` | Lossless VP8L (pixel-perfect, larger file) | off |

### Differences from the Python version

| Aspect | Python | C |
|--------|--------|---|
| Runtime | Python 3.7+ + Pillow | Standalone `.exe` (libwebp statically linked, no DLLs) |
| PNG sBIT chunk | Included for 16 bpp | Not included (GDI+ limitation) |
| True 16-bit BMP | Manual BITFIELDS packing | GDI+ saves as 24-bit container |
| `--beep` | Supported (`kernel32.Beep`); tones: save `1500 Hz`, duplicate `600 Hz`, overwrite `300 Hz` | Supported (`Beep()` API); tones: save `1200 Hz`, duplicate `600 Hz`, overwrite `400 Hz` |
| DATETIME timestamps | Real milliseconds, e.g. `clip_20260610_112105_063.png` | Fixed `_000` milliseconds (no real ms) |
| Instance detection | Mutex-based (`CreateMutexW`) | Window-based (`FindWindowW`) |
| Hotkey | `--keys` supported | `--keys` supported identically |
| Size | ~900 lines | ~1400 lines + `webp_save.c` (~70 lines) |

The core clipboard-listener mechanism and all options are identical across both versions. Instance detection uses a different technique (mutex vs. window lookup) but achieves the same goal.
