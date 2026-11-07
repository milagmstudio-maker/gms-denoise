@echo off
rem Removes the GmS De-Noise.vst3 that install.bat copied. (Setup.exe installs
rem are removed from Settings > Apps instead.)
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "DST=%CommonProgramW6432%\VST3"
if "%CommonProgramW6432%"=="" set "DST=%CommonProgramFiles%\VST3"

if exist "%DST%\GmS De-Noise.vst3" (
    rmdir /s /q "%DST%\GmS De-Noise.vst3"
    echo GmS De-Noise removed.
) else (
    echo GmS De-Noise was not installed.
)
pause
