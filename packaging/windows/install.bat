@echo off
rem GmS De-Noise manual install: copies GmS De-Noise.vst3 into the standard VST3 folder.
rem Use this when the Setup.exe cannot be used. Needs administrator rights
rem (it asks for them by itself).
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "SRC=%~dp0GmS De-Noise.vst3"
set "DST=%CommonProgramW6432%\VST3"
if "%CommonProgramW6432%"=="" set "DST=%CommonProgramFiles%\VST3"

if not exist "%SRC%" (
    echo GmS De-Noise.vst3 was not found. Extract the whole zip first, then run this file.
    pause
    exit /b 1
)

rem Files from a downloaded zip carry the "from the internet" mark; remove it.
powershell -NoProfile -Command "Get-ChildItem -LiteralPath '%SRC%' -Recurse | Unblock-File" >nul 2>&1

if not exist "%DST%" mkdir "%DST%"
if exist "%DST%\GmS De-Noise.vst3" rmdir /s /q "%DST%\GmS De-Noise.vst3"
xcopy "%SRC%" "%DST%\GmS De-Noise.vst3\" /e /i /q /y >nul
if %errorlevel% neq 0 (
    echo Copy failed. Close your DAW and run this file again.
    pause
    exit /b 1
)

echo.
echo GmS De-Noise installed: %DST%\GmS De-Noise.vst3
echo Restart your DAW and rescan plug-ins.
pause
