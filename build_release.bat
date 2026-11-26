@echo off
setlocal
echo Packaging Phantom Touch v2.1 Production Real-Phone Emulator Release...

if not exist scrcpy\adb.exe (
    echo [FAILED] Missing scrcpy\adb.exe. Run powershell -ExecutionPolicy Bypass -File setup_scrcpy.ps1 first.
    exit /b 1
)
if not exist scrcpy\scrcpy-server (
    echo [FAILED] Missing scrcpy\scrcpy-server. Run setup_scrcpy.ps1 first.
    exit /b 1
)
if not exist scrcpy\scrcpy.exe (
    echo [FAILED] Missing scrcpy\scrcpy.exe. Run setup_scrcpy.ps1 first.
    exit /b 1
)

call build.bat
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

set DIST=dist\PhantomTouch
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%"
mkdir "%DIST%\profiles"
mkdir "%DIST%\scrcpy"

copy /y PhantomTouch.exe "%DIST%\" >nul
copy /y run.bat "%DIST%\" >nul
copy /y setup_scrcpy.ps1 "%DIST%\" >nul
copy /y README.md "%DIST%\" >nul
copy /y THIRD_PARTY_LICENSES.txt "%DIST%\" >nul
if exist profiles\freefire.cfg copy /y profiles\freefire.cfg "%DIST%\profiles\" >nul

for %%F in (adb.exe AdbWinApi.dll AdbWinUsbApi.dll scrcpy-server scrcpy.exe SDL3.dll avcodec-63.dll avformat-63.dll avutil-61.dll swresample-7.dll libusb-1.0.dll LICENSE.txt) do (
    if exist "scrcpy\%%F" (
        copy /y "scrcpy\%%F" "%DIST%\scrcpy\" >nul
    )
)

echo.
echo [RELEASE READY] Staged clean portable package in: %DIST%\
dir /b "%DIST%"
