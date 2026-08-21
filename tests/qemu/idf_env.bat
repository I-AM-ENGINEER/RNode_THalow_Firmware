@echo off
REM Generic ESP-IDF environment wrapper (same env hygiene as _build.bat),
REM then executes the remaining arguments, e.g.:
REM   idf_env.bat idf.py -B build qemu monitor
set MSYSTEM=
set MSYS=
set MINGW_PREFIX=
set MINGW_CHOST=
set MINGW_PACKAGE_PREFIX=

call C:\esp-idf\export.bat >nul 2>&1
if errorlevel 1 (
    echo export.bat failed
    exit /b 1
)

%*
