@echo off
rem Run the finetune calibration mode. Extra args pass through,
rem e.g.: run-finetune.bat --coverage=0.8 --max-deg=25
cd /d "%~dp0build\Release"
if not exist EyeTrackVR-Overlay.exe (
    echo Release build not found. Build first: cmake --build build --config Release
    pause
    exit /b 1
)
EyeTrackVR-Overlay.exe finetune %*
echo.
echo ---- eyetrackvr_overlay.log ----
type eyetrackvr_overlay.log
pause
