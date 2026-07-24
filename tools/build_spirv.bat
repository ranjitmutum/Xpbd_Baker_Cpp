@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_spirv.ps1" %*
exit /b %ERRORLEVEL%

