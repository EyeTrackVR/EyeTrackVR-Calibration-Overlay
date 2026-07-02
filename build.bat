@echo off
setlocal
rem Build EyeTrackVR-Overlay (Release). Output: build\Release\EyeTrackVR-Overlay.exe
cd /d "%~dp0"

rem -- Locate vcpkg: VCPKG_ROOT env var, else the usual local install ----------
if defined VCPKG_ROOT (
    set "VCPKG=%VCPKG_ROOT%"
) else if exist "%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake" (
    set "VCPKG=%USERPROFILE%\vcpkg"
) else (
    echo [ERROR] vcpkg not found. Set VCPKG_ROOT or install to %%USERPROFILE%%\vcpkg
    exit /b 1
)

rem -- OpenVR SDK submodule (needed for the SteamVR overlay backend) -----------
if not exist "external\OpenVR\headers\openvr.h" (
    echo [INFO] Fetching OpenVR submodule...
    git submodule update --init
    if errorlevel 1 exit /b 1
)

rem -- Configure once. Sources are globbed: after adding/removing source files,
rem    delete build\CMakeCache.txt (or the build folder) to force a reconfigure.
if not exist "build\CMakeCache.txt" (
    cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="%VCPKG%\scripts\buildsystems\vcpkg.cmake"
    if errorlevel 1 exit /b 1
)

cmake --build build --config Release
if errorlevel 1 exit /b 1

echo.
echo [OK] build\Release\EyeTrackVR-Overlay.exe
endlocal
