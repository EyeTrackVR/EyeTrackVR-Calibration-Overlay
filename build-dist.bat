@echo off
setlocal EnableExtensions
REM ==========================================================================
REM  Build the prebuilt overlay runtimes EyeTrackVR ships, into dist\.
REM
REM    build-dist.bat [windows|linux|arm64|all]   (default: all)
REM
REM    windows  build.bat (vcpkg)                -> dist\windows\
REM    linux    WSL Ubuntu-22.04                 -> dist\linux\
REM    arm64    Docker ubuntu:24.04 arm64 (QEMU) -> dist\linux-arm64\
REM
REM  Commit your source changes first: Linux builds the committed HEAD. Then
REM  run this and commit dist\. Each folder gets SOURCE_TREE, a hash of the
REM  inputs listed in dist\SOURCE_INPUTS at HEAD; EyeTrackVR's tests check it
REM  so stale binaries can't ship.
REM ==========================================================================

cd /d "%~dp0"
set "ROOT_WIN=%CD%"
set "DISTRO=Ubuntu-22.04"
set "WHICH=%~1"
if "%WHICH%"=="" set "WHICH=all"

if not exist "external\OpenVR\headers\openvr.h" (
    echo [dist] fetching the OpenVR SDK submodule...
    git submodule update --init --depth 1 external/OpenVR || exit /b 1
)
set "INPUTS="
for /f "usebackq delims=" %%i in ("dist\SOURCE_INPUTS") do set "INPUTS=%%i"
git status --porcelain -- %INPUTS% | findstr . >nul && echo [dist] WARNING: uncommitted source changes; Linux builds use HEAD and SOURCE_TREE describes HEAD
if not exist "build-dist" mkdir "build-dist"
for /f %%h in ('git ls-tree -r HEAD -- %INPUTS% ^| git hash-object --stdin') do > "build-dist\SOURCE_TREE" echo %%h

if /i "%WHICH%"=="windows" goto :windows
if /i "%WHICH%"=="linux" goto :prepare
if /i "%WHICH%"=="arm64" goto :prepare
if /i "%WHICH%"=="all" goto :windows
echo Usage: build-dist.bat [windows^|linux^|arm64^|all]
exit /b 1

:windows
echo.
echo [dist] building for Windows...
call "%ROOT_WIN%\build.bat" || exit /b 1
cd /d "%ROOT_WIN%"
set "OUT=build\Release"
if not exist "dist\windows\assets" mkdir "dist\windows\assets"
for %%f in (EyeTrackVR-Overlay.exe jsoncpp.dll openvr_api.dll openxr_loader.dll vulkan-1.dll) do (
    copy /y "%OUT%\%%f" "dist\windows\" >nul || exit /b 1
)
copy /y "%OUT%\assets\OpenSans-Regular.ttf" "dist\windows\assets\" >nul || exit /b 1
copy /y "build-dist\SOURCE_TREE" "dist\windows\" >nul || exit /b 1
echo [dist] done: dist\windows
if /i "%WHICH%"=="windows" exit /b 0

:prepare
REM Linux builds run from this export: a submodule checkout's .git lives
REM outside this folder, out of reach of WSL paths and Docker mounts.
git -c core.autocrlf=false -c core.eol=lf archive -o build-dist\src.tar HEAD || exit /b 1
git -C external\OpenVR -c core.autocrlf=false -c core.eol=lf archive -o "%ROOT_WIN%\build-dist\openvr.tar" HEAD headers lib/linux64 lib/linuxarm64 LICENSE || exit /b 1
if /i "%WHICH%"=="arm64" goto :arm64

echo.
echo [dist] building for Linux x86_64 in WSL %DISTRO%...
for /f "delims=" %%p in ('wsl.exe -d %DISTRO% -- wslpath -a "%ROOT_WIN%"') do set "ROOT_WSL=%%p"
if not defined ROOT_WSL (
    echo ERROR: WSL %DISTRO% is not available
    exit /b 1
)
wsl.exe -d %DISTRO% -u root -- bash -c "tr -d '\r' < '%ROOT_WSL%/build-dist.sh' | ETVR_DIST_PREPARED=1 bash -s -- '%ROOT_WSL%'" || exit /b 1
if /i "%WHICH%"=="linux" exit /b 0
REM Stopping the distro drops Docker Desktop's arm64 QEMU handler, which is
REM re-registered below.
wsl.exe --terminate %DISTRO% >nul 2>&1

:arm64
echo.
echo [dist] building for Linux arm64 in Docker (QEMU, slow)...
docker info >nul 2>&1
if errorlevel 1 (
    echo ERROR: Docker is not running - start Docker Desktop and retry
    exit /b 1
)
docker run --privileged --rm tonistiigi/binfmt@sha256:400a4873b838d1b89194d982c45e5fb3cda4593fbfd7e08a02e76b03b21166f0 --install arm64 >nul || exit /b 1
docker run --rm --platform linux/arm64 -v "%ROOT_WIN%:/src" ubuntu:24.04 bash -c "export DEBIAN_FRONTEND=noninteractive && apt-get update -qq && apt-get install -y -qq --no-install-recommends build-essential cmake ninja-build libvulkan-dev libopenxr-dev libstb-dev patchelf curl ca-certificates >/dev/null && tr -d '\r' < /src/build-dist.sh | ETVR_DIST_PREPARED=1 bash -s -- /src" || exit /b 1
exit /b 0
