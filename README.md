# ClipSave

**ClipSave** is a lightweight Windows clipboard monitor that automatically captures images copied to the clipboard and saves them as files.

Triggered by: Snipping Tool, Win+Shift+S, Print Screen, Ctrl+C on an image, or any other action that places an image on the clipboard.

Uses the native `AddClipboardFormatListener` API — **zero polling, ~0% CPU** while waiting.

---

# Author

SCRIPT_AUTH = "Igor Brzeżek"
SCRIPT_VERSION = 0.1
SCRIP_GITHUB = "https://github.com/IgorBrzezek/ClipSave"

## Requirements

- **Python** 3.7 or newer
- **Pillow** (`pip install Pillow`)
- **Windows** 10 or 11

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
| `-f FORMAT` | Image format: `png`, `jpg`, `bmp` | `png` |
| `--bpp N` | Color depth: `8` (grayscale), `P` (palette 256), `16` (RGB565), `24` (full RGB) | `16` |
| `-c N` / `--compression N` | Compression level — see below | format default |
| `--name MODE` | Naming scheme — see below | `DATETIME` |
| `--overwrite` | Overwrite existing files without asking | ask first |

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

### Overwrite behavior

By default ClipSave asks before overwriting an existing file:

```
  [?] File exists: clip_20260610_112105_063.png. Overwrite? [y/N]
```

Pass `--overwrite` to skip the prompt and always overwrite.

---

## Examples

**Default run** — PNG, 16-bit RGB565, current directory:
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

**Overwrite existing files without asking:**
```bash
python clipsave.py --overwrite
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

---

## Project

- **Author:** Igor Brzeżek
- **Version:** 0.1
- **GitHub:** [https://github.com/IgorBrzezek/ClipSave](https://github.com/IgorBrzezek/ClipSave)

---

## ANSI C version

A standalone C port (`clipsave.c`) with identical functionality — no Python or Pillow required.

### Requirements

- **GCC** (MinGW-w64) — any recent version with `gcc` and `ld`
- **Windows 10 or 11**
- No third-party libraries — uses only built-in Windows APIs (GDI+, Win32)

### Dependencies

The only link-time dependencies are system libraries included with every Windows installation:

| Library | Purpose |
|---------|---------|
| `gdiplus` | GDI+ — image encoding (PNG, JPEG, BMP) |
| `gdi32` | GDI — `CreateDIBSection`, `DeleteObject` |
| `ole32` | COM — GDI+ startup |
| `uuid` | UUID — encoder CLSID lookup |

All are available in any MinGW-w64 distribution (MSYS2, Cygwin, etc.).

### Build

```bash
gcc -O2 -municode clipsave.c -lgdiplus -lgdi32 -lole32 -luuid -o clipsave.exe
```

Flags explained:
- `-O2` — optimization level
- `-municode` — use Unicode (`wmain`) entry point
- `-l...` — link against system libraries

The resulting binary is a standalone `.exe` (~170 KB) with no runtime dependencies beyond what Windows 10/11 provides.

### Usage

Same command-line interface as the Python version:

```bash
clipsave.exe -d C:\Screenshots -f jpg --bpp 24 -c 90
clipsave.exe -f bmp --bpp 8 --name scan[N]
clipsave.exe --name photo_[DT]_[NN] --overwrite
```

### Differences from the Python version

| Aspect | Python | C |
|--------|--------|---|
| Runtime | Python 3.7+ + Pillow | Standalone `.exe` |
| PNG sBIT chunk | Included for 16 bpp | Not included (GDI+ limitation) |
| True 16-bit BMP | Manual BITFIELDS packing | GDI+ saves as 24-bit container |
| Size | ~500 lines | ~750 lines |

The core behavior, all options, and the clipboard-listener mechanism are identical.
