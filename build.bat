@echo off
setlocal
echo Building Phantom Touch v2.0 (Production Emulator Shell, C++11 / Win32 / GDI+)...

windres resources\phantom_touch.rc -O coff -o resources\phantom_touch.res
if %ERRORLEVEL% NEQ 0 (
    echo [FAILED] windres failed with error code %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)

g++ -std=c++11 -O2 -Wall -Wextra -mwindows ^
    -D_WIN32_WINNT=0x0601 ^
    -Isrc ^
    src\app\main.cpp ^
    src\app\Application.cpp ^
    src\bridge\SpsParser.cpp ^
    src\bridge\TouchFingerPool.cpp ^
    src\bridge\ScrcpyBridge.cpp ^
    src\config\KeyCodes.cpp ^
    src\config\Profile.cpp ^
    src\config\ProfileStore.cpp ^
    src\config\AppSettings.cpp ^
    src\engine\FpsViewController.cpp ^
    src\engine\JoystickController.cpp ^
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
    -lgdiplus -lole32 -lshell32 -lws2_32 -lwinmm -lgdi32 -luser32 -static-libgcc -static-libstdc++

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [FAILED] Build failed with error code %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)

echo.
echo [SUCCESS] Built PhantomTouch.exe (Production GUI Subsystem - Zero Console Window)
echo Run: PhantomTouch.exe          (Emulator Shell + Auto USB / Wireless ADB Hub)
echo Run: PhantomTouch.exe --demo   (Offline Keymap Studio)
echo Run: PhantomTouch.exe --console (Attach debug console)
