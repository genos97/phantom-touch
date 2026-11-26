@echo off
setlocal EnableDelayedExpansion
echo Building Phantom Touch Core Unit Tests...

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
) else (
    set "CXX=x86_64-w64-mingw32-g++"
)

"%CXX%" -std=c++11 -O2 -Wall -Wextra ^
    -D_WIN32_WINNT=0x0601 ^
    -Isrc ^
    tests\test_core.cpp ^
    src\bridge\SpsParser.cpp ^
    src\bridge\TouchFingerPool.cpp ^
    src\bridge\QrCodeGen.cpp ^
    src\bridge\ConnectionState.cpp ^
    src\bridge\ScrcpyBridge.cpp ^
    src\video\H264Decoder.cpp ^
    src\video\D3D11Renderer.cpp ^
    src\video\LatencyTelemetry.cpp ^
    src\config\KeyCodes.cpp ^
    src\config\Profile.cpp ^
    src\config\ProfileStore.cpp ^
    src\config\AppSettings.cpp ^
    src\engine\ThreadScheduling.cpp ^
    src\engine\FpsViewController.cpp ^
    src\engine\JoystickController.cpp ^
    src\engine\GamepadController.cpp ^
    src\engine\ActionDispatcher.cpp ^
    src\engine\InputHookThread.cpp ^
    src\engine\InputEngine.cpp ^
    src\ui\GdiPlusDraw.cpp ^
    src\ui\Animator.cpp ^
    src\ui\OverlayPopups.cpp ^
    src\ui\EditorState.cpp ^
    src\ui\HudRenderer.cpp ^
    src\ui\TopBar.cpp ^
    src\ui\SideRail.cpp ^
    src\ui\ViewportHost.cpp ^
    src\ui\OnboardingView.cpp ^
    src\ui\DrawerPanel.cpp ^
    -o tests\test_core.exe ^
    -static ^
    -ld3d11 -ldxgi -lmfplat -lmfuuid -lgdiplus -lole32 -loleaut32 -luuid -lshell32 -lws2_32 -lwinmm -lgdi32 -luser32

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [FAILED] Unit test build failed with error code %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)

echo.
echo [RUNNING] tests\test_core.exe
tests\test_core.exe
exit /b %ERRORLEVEL%
