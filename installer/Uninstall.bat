@echo off
rem Double-click friendly uninstaller wrapper.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall.ps1"
echo.
pause
