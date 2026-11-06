@echo off
setlocal EnableDelayedExpansion
title OpenRaven - Halo: Fireteam Raven PC Launcher (Windows / WSL2)

echo ================================================================
echo   OpenRaven - Halo: Fireteam Raven (v3.11) PC Loader
echo ================================================================
echo.

:: Run the PowerShell setup & launcher script with ExecutionPolicy Bypass
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0openraven\windows_setup.ps1" %*
if errorlevel 1 (
    echo.
    echo [OpenRaven] Stopped with an error. Press any key to close...
    pause >nul
)
