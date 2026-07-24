@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_tools.ps1" %*
exit /b %ERRORLEVEL%
