@echo off
REM ============================================================
REM  build.bat  –  Incremental Ninja build (no clean / no delete)
REM
REM  This script only recompiles changed sources and re-links
REM  the exe.  Nothing in the build folder is deleted.
REM
REM  Usage:
REM    build.bat          – normal incremental build (all cores)
REM    build.bat -j4      – limit to 4 parallel jobs
REM    build.bat -v       – verbose (show full compile commands)
REM ============================================================

setlocal

REM -- Add MSYS2 mingw64 to PATH so gcc / ninja / cmake are found
set "PATH=C:\msys64\mingw64\bin;C:\msys64\clang64\bin;%PATH%"

REM -- Project directories (relative to this script)
set "SRC_DIR=%~dp0"
set "BUILD_DIR=%~dp0build"

REM -- If the build directory has never been configured, run cmake once.
REM    This does NOT delete anything – it only writes build.ninja if missing.
if not exist "%BUILD_DIR%\build.ninja" (
    echo [build.bat] No build.ninja found – running CMake configure...
    cmake -S "%SRC_DIR%" -B "%BUILD_DIR%" -G "Ninja" -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 (
        echo [build.bat] CMake configure FAILED.
        exit /b 1
    )
)

REM -- Run Ninja (incremental – only rebuilds what changed, deletes nothing)
echo [build.bat] Building with Ninja (incremental)...
ninja -C "%BUILD_DIR%" %*
if errorlevel 1 (
    echo [build.bat] Build FAILED.
    exit /b 1
)

echo.
echo [build.bat] Build succeeded: %BUILD_DIR%\Rabbids Go Home.exe
