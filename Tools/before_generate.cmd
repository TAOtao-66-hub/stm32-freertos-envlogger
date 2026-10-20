@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-Sync.ps1" -Action before > "%~dp0before_generate.log" 2>&1
exit /b %errorlevel%
