@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-Sync.ps1" -Action check
exit /b %errorlevel%
