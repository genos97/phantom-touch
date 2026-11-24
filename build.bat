@echo off
setlocal
echo Building Phantom Touch (C++11 / Win32)...

g++ -std=c++11 -O2 -Wall -Wextra ^
    -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN ^
    -Isrc ^
    src\app\main.cpp ^
    src\app\Application.cpp ^
    src\bridge\SpsParser.cpp ^
    src\bridge\ScrcpyBridge.cpp ^
    src\config\KeyCodes.cpp ^
    src\config\Profile.cpp ^
    src\config\ProfileStore.cpp ^
    src\engine\FpsViewController.cpp ^
    src\engine\JoystickController.cpp ^
    src\engine\ActionDispatcher.cpp ^
    src\engine\InputEngine.cpp ^
    src\ui\GdiDraw.cpp ^
    src\ui\HudRenderer.cpp ^
    src\ui\StudioPanel.cpp ^
    src\ui\OverlayWindow.cpp ^
    -o scrcpy_keymapper.exe ^
    -lws2_32 -lwinmm -lgdi32 -luser32 -static-libgcc -static-libstdc++

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [FAILED] Build failed with error code %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)

echo.
echo [SUCCESS] Built scrcpy_keymapper.exe
echo Run: scrcpy_keymapper.exe --demo   (test offline HUD editor)
echo Run: scrcpy_keymapper.exe          (live phone via scrcpy)
