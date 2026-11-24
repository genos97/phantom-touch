@echo off
cd /d "%~dp0"
scrcpy_keymapper.exe
if %ERRORLEVEL% NEQ 0 pause
