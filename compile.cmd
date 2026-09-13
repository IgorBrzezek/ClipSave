@echo off
rem ============================================================
rem   ClipSave - Windows build, plain MinGW gcc (no cmake/make)
rem
rem   This script does everything on a clean Windows:
rem     1) downloads the libwebp 1.4.0 source (curl / PowerShell,
rem        both built into Windows 10/11),
rem     2) extracts it,
rem     3) builds static libwebp.a + libsharpyuv.a directly with
rem        gcc + ar (no configure / cmake / make needed),
rem     4) compiles clipsave.exe (clipsave.c + webp_save.c).
rem
rem   Requirements:
rem     - MinGW-w64 with gcc and ar on PATH
rem         * MSYS2 UCRT64:  pacman -S mingw-w64-ucrt-x86_64-gcc
rem         * WinLibs:       https://winlibs.com  (unzip, add "bin" to PATH)
rem     - Windows 10/11 (curl.exe + tar/PowerShell are built in)
rem
rem   Re-running skips the download and libwebp build if the library
rem   already exists (delete libwebp-1.4.0\build to force a rebuild).
rem ============================================================

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "LW=libwebp-1.4.0"
set "ZIP=%LW%.zip"
set "URL=https://github.com/webmproject/libwebp/archive/refs/tags/v1.4.0.zip"
set "SRC_ENC=%LW%\src\enc"
set "SRC_DSP=%LW%\src\dsp"
set "SRC_DEC=%LW%\src\dec"
set "SRC_UTL=%LW%\src\utils"
set "SRC_SHP=%LW%\sharpyuv"
set "OBJ_DIR=%LW%\build\obj"

echo.
echo   ==========================================================
echo     ClipSave Windows build  (clipsave.c + webp_save.c)
echo   ==========================================================

rem ---- 0) run from the project folder ----
if not exist clipsave.c (
    echo.
    echo   [ERROR] clipsave.c not found in the current folder.
    echo           Run compile.cmd from the ClipSave project folder.
    exit /b 1
)

rem ---- 1) toolchain check ----
gcc --version >nul 2>&1
if errorlevel 1 goto :nocc
ar --version >nul 2>&1
if errorlevel 1 goto :nocc
for /f "delims=" %%v in ('gcc --version 2^>nul ^| findstr /i "gcc"') do (
    echo   Toolchain: %%v
    goto :verdone
)
:verdone

rem ---- 2) fetch libwebp source if not present ----
if exist "%LW%\src\webp\encode.h" goto :have_src
echo   Downloading libwebp 1.4.0 source...
where curl >nul 2>&1
if not errorlevel 1 (
    curl -L -o "%ZIP%" "%URL%"
) else (
    powershell -NoProfile -Command "Invoke-WebRequest -Uri '%URL%' -OutFile '%ZIP%'"
)
if errorlevel 1 goto :dlerr
if not exist "%ZIP%" goto :dlerr

if exist "%LW%" rd /s /q "%LW%"
where tar >nul 2>&1
if not errorlevel 1 (
    rem bsdtar handles .zip too
    tar -xf "%ZIP%"
) else (
    powershell -NoProfile -Command "Expand-Archive -Path '%ZIP%' -DestinationPath '.' -Force"
)
if errorlevel 1 goto :dlerr
if not exist "%LW%\src\webp\encode.h" goto :dlerr
echo   Source ready: %LW%\src\webp\encode.h
:have_src

rem ---- 3) build static libwebp (plain gcc; skip if already built) ----
if exist "%LW%\build\libwebp.a" goto :have_lib
echo   Building static libwebp.a + libsharpyuv.a ...
if exist "%OBJ_DIR%" rd /s /q "%OBJ_DIR%"
mkdir "%OBJ_DIR%\webp"   >nul 2>&1
mkdir "%OBJ_DIR%\sharpyuv" >nul 2>&1
if exist "%LW%\build\gcc.log" del /q "%LW%\build\gcc.log"

for /r "%SRC_ENC%" %%f in (*.c) do (
    gcc -O2 -I "%LW%\src" -I "%LW%" -c "%%f" -o "%OBJ_DIR%\webp\%%~nf.o" >>"%LW%\build\gcc.log" 2>&1 || goto :builderr
)
for /r "%SRC_DSP%" %%f in (*.c) do (
    gcc -O2 -I "%LW%\src" -I "%LW%" -c "%%f" -o "%OBJ_DIR%\webp\%%~nf.o" >>"%LW%\build\gcc.log" 2>&1 || goto :builderr
)
for /r "%SRC_DEC%" %%f in (*.c) do (
    gcc -O2 -I "%LW%\src" -I "%LW%" -c "%%f" -o "%OBJ_DIR%\webp\%%~nf.o" >>"%LW%\build\gcc.log" 2>&1 || goto :builderr
)
for /r "%SRC_UTL%" %%f in (*.c) do (
    gcc -O2 -I "%LW%\src" -I "%LW%" -c "%%f" -o "%OBJ_DIR%\webp\%%~nf.o" >>"%LW%\build\gcc.log" 2>&1 || goto :builderr
)
for /r "%SRC_SHP%" %%f in (*.c) do (
    gcc -O2 -I "%LW%\src" -I "%LW%" -c "%%f" -o "%OBJ_DIR%\sharpyuv\%%~nf.o" >>"%LW%\build\gcc.log" 2>&1 || goto :builderr
)

for /r "%OBJ_DIR%\webp" %%f in (*.o) do (
    ar rcs "%LW%\build\libwebp.a" "%%f" >>"%LW%\build\gcc.log" 2>&1 || goto :builderr
)
for /r "%OBJ_DIR%\sharpyuv" %%f in (*.o) do (
    ar rcs "%LW%\build\libsharpyuv.a" "%%f" >>"%LW%\build\gcc.log" 2>&1 || goto :builderr
)
if not exist "%LW%\build\libwebp.a" goto :builderr
if not exist "%LW%\build\libsharpyuv.a" goto :builderr
echo   Static libraries: %LW%\build\libwebp.a  %LW%\build\libsharpyuv.a
:have_lib

rem ---- 4) compile clipsave.exe ----
echo   Compiling clipsave.exe ...
gcc -O2 -municode clipsave.c webp_save.c -o clipsave.exe ^
    -I "%LW%\src" -L "%LW%\build" ^
    -lgdiplus -lgdi32 -lole32 -luuid ^
    -lwebp -lsharpyuv -lpthread -lm
if errorlevel 1 goto :compileerr

echo.
echo   OK:  clipsave.exe  (no codec DLLs - libwebp linked statically)
for %%f in (clipsave.exe) do echo         size: %%~zf bytes
echo.
echo   Try:  clipsave.exe -h
echo         clipsave.exe -f webp --webpq 75
echo         clipsave.exe -f webp --webplossless
echo.
exit /b 0

rem ---- error handlers ----
:nocc
echo.
echo   [ERROR] MinGW gcc or ar not found on PATH.
echo           Install MinGW-w64 and add its "bin" folder to PATH:
echo             - MSYS2:  https://www.msys2.org  (pacman -S mingw-w64-ucrt-x86_64-gcc)
echo             - WinLibs: https://winlibs.com
echo           Then run compile.cmd again.
exit /b 1

:dlerr
echo.
echo   [ERROR] could not download/extract libwebp.
echo           Download it manually to "%ZIP%" and re-run:
echo           %URL%
exit /b 1

:builderr
echo.
echo   [ERROR] libwebp build failed.
echo           Detail log: %LW%\build\gcc.log
exit /b 1

:compileerr
echo.
echo   [ERROR] compilation of clipsave.exe failed.
echo           Fix the reported errors and re-run compile.cmd.
exit /b 1