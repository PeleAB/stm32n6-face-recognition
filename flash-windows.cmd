@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\flash-windows.ps1" %*
exit /b %errorlevel%
