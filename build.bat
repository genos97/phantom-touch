@echo off
setlocal
echo Building Scrcpy Keymapper Pro (C++11 / Win32)...

g++ -std=c++11 -O2 -Wall -Wextra ^
    -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN ^
    src\main.cpp ^
    src\sps_parser.cpp ^
    src\profile.cpp ^
    src\protocol.cpp ^
    src\aim_engine.cpp ^
    src\config_ui.cpp ^
    src\overlay.cpp ^
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
