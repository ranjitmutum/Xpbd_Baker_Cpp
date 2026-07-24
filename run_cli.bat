@echo off
setlocal EnableExtensions
set "SCRIPT_DIR=%~dp0"
set "CLI=%SCRIPT_DIR%build\Release\xpbd_cli.exe"
if not exist "%CLI%" set "CLI=%SCRIPT_DIR%build\xpbd_cli.exe"
if not exist "%CLI%" (
  echo ERROR: build xpbd_cli first:  cpp\build.bat
  exit /b 1
)
"%CLI%" %*
endlocal
exit /b %ERRORLEVEL%