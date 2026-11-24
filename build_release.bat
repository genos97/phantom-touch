@echo off
setlocal
echo Packaging Phantom Touch v2.0 Production Release...

call build.bat
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

set DIST=dist\PhantomTouch
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%"
mkdir "%DIST%\profiles"

copy /y PhantomTouch.exe "%DIST%\" >nul
copy /y run.bat "%DIST%\" >nul
copy /y setup_scrcpy.ps1 "%DIST%\" >nul
copy /y README.md "%DIST%\" >nul
copy /y profiles\freefire.cfg "%DIST%\profiles\" >nul

if exist scrcpy\scrcpy.exe (
    mkdir "%DIST%\scrcpy"
    xcopy /e /i /q /y scrcpy "%DIST%\scrcpy\" >nul
)

echo.
echo [RELEASE READY] Staged clean portable package in: %DIST%\
dir /b "%DIST%"
