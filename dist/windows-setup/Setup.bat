REM SPDX-License-Identifier: GPL-3.0-only
REM Copyright (C) 2026 Dan B. Cooper
@echo off
rem Kyujo - Windows setup. Double-click this file, or drag your ROM file onto it.
rem Everything is done by setup\Setup.ps1 (PowerShell, which every Windows 10/11 has); this file only starts it.
cd /d "%~dp0"
if "%~1"=="" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "setup\Setup.ps1"
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "setup\Setup.ps1" -Rom "%~1"
)
echo.
pause
