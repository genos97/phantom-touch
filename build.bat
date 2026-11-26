@echo off
setlocal EnableDelayedExpansion
echo Building Phantom Touch v2.1 (64-bit Production Real-Phone Emulator, D3D11 / Media Foundation / XInput)...

set "MINGW64_BIN="
for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*") do (
    if exist "%%D\mingw64\bin\x86_64-w64-mingw32-g++.exe" (
        set "MINGW64_BIN=%%D\mingw64\bin"
    )
)
if not defined MINGW64_BIN (
    if exist "C:\mingw64\bin\x86_64-w64-mingw32-g++.exe" set "MINGW64_BIN=C:\mingw64\bin"
)
if not defined MINGW64_BIN (
    if exist "C:\msys64\ucrt64\bin\x86_64-w64-mingw32-g++.exe" set "MINGW64_BIN=C:\msys64\ucrt64\bin"
)

if defined MINGW64_BIN (
    set "PATH=!MINGW64_BIN!;!PATH!"
    set "CXX=!MINGW64_BIN!\x86_64-w64-mingw32-g++.exe"
    set "WINDRES=!MINGW64_BIN!\windres.exe"
) else (
    set "CXX=x86_64-w64-mingw32-g++"
    set "WINDRES=windres"
)

"%WINDRES%" resources\phantom_touch.rc -O coff -o resources\phantom_touch.res
if %ERRORLEVEL% NEQ 0 (
    echo [FAILED] windres failed with error code %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)

"%CXX%" -std=c++11 -O2 -Wall -Wextra -mwindows ^
    -D_WIN32_WINNT=0x0601 ^
    -Isrc ^
    src\app\main.cpp ^
    src\app\Application.cpp ^
    src\bridge\SpsParser.cpp ^
    src\bridge\TouchFingerPool.cpp ^
    src\bridge\QrCodeGen.cpp ^
    src\bridge\ScrcpyBridge.cpp ^
    src\video\H264Decoder.cpp ^
    src\video\D3D11Renderer.cpp ^
    src\config\KeyCodes.cpp ^
    src\config\Profile.cpp ^
    src\config\ProfileStore.cpp ^
    src\config\AppSettings.cpp ^
    src\engine\FpsViewController.cpp ^
    src\engine\JoystickController.cpp ^
    src\engine\GamepadController.cpp ^
    src\engine\ActionDispatcher.cpp ^
    src\engine\InputEngine.cpp ^
    src\ui\GdiPlusDraw.cpp ^
    src\ui\HudRenderer.cpp ^
    src\ui\TopBar.cpp ^
    src\ui\SideRail.cpp ^
    src\ui\ViewportHost.cpp ^
    src\ui\DrawerPanel.cpp ^
    src\ui\HudOverlay.cpp ^
    src\ui\ShellWindow.cpp ^
    resources\phantom_touch.res ^
    -o PhantomTouch.exe ^
    -static ^
    -ld3d11 -ldxgi -lmfplat -lmfuuid -lgdiplus -lole32 -loleaut32 -luuid -lshell32 -lws2_32 -lwinmm -lgdi32 -luser32

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [FAILED] Build failed with error code %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)

echo.
echo [SUCCESS] Built 64-bit PhantomTouch.exe (Native D3D11 + Media Foundation + Zero Console Window)
echo Run: PhantomTouch.exe          (Emulator Shell + Auto USB / Wireless ADB Hub)
echo Run: PhantomTouch.exe --demo   (Offline Keymap Studio)
echo Run: PhantomTouch.exe --console (Attach debug console)
