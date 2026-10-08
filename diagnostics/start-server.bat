@echo off
rem Runs the relay server in this window. Closing the window (or Ctrl+C) stops it.
rem Optional argument: UDP port (default 27020).
title ASMP relay server
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-server.ps1" %*
echo.
echo Server stopped. Press any key to close.
pause >nul
