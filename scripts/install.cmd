@echo off
rem Double-click friendly wrapper for Windows. Passes arguments through to install.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
if errorlevel 1 pause
