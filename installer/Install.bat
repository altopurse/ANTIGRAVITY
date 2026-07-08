@echo off
rem Double-click friendly installer wrapper.
rem Bypasses the PowerShell execution policy that silently blocks .ps1 files.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1"
echo.
pause
