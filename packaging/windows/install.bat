@echo off
rem Shizumaru (formerly GmS De-Noise) manual install: copies Shizumaru.vst3 into the standard VST3 folder.
rem Use this when the Setup.exe cannot be used. Needs administrator rights
rem (it asks for them by itself).
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "SRC=%~dp0Shizumaru.vst3"
set "DST=%CommonProgramW6432%\VST3"
if "%CommonProgramW6432%"=="" set "DST=%CommonProgramFiles%\VST3"

if not exist "%SRC%" (
    echo Shizumaru.vst3 was not found. Extract the whole zip first, then run this file.
    pause
    exit /b 1
)

rem Files from a downloaded zip carry the "from the internet" mark; remove it.
powershell -NoProfile -Command "Get-ChildItem -LiteralPath '%SRC%' -Recurse | Unblock-File" >nul 2>&1

if not exist "%DST%" mkdir "%DST%"
rem The old name (GmS De-Noise) is the same plug-in: remove it so hosts do not see it twice.
if exist "%DST%\GmS De-Noise.vst3" rmdir /s /q "%DST%\GmS De-Noise.vst3"
if exist "%DST%\Shizumaru.vst3" rmdir /s /q "%DST%\Shizumaru.vst3"
xcopy "%SRC%" "%DST%\Shizumaru.vst3\" /e /i /q /y >nul
if %errorlevel% neq 0 (
    echo Copy failed. Close your DAW and run this file again.
    pause
    exit /b 1
)

echo.
echo Shizumaru installed: %DST%\Shizumaru.vst3
echo Restart your DAW and rescan plug-ins.
pause
