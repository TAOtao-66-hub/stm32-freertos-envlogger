@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-Sync.ps1" -Action after > "%~dp0after_generate.log" 2>&1
exit /b %errorlevel%
