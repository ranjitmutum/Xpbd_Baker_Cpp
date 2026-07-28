@echo off
setlocal EnableExtensions

if "%VCPKG_ROOT%"=="" (
  echo ERROR: VCPKG_ROOT is not set.
  exit /b 1
)

set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%build"

echo === XPBD Baker C++ verify ===
call "%SCRIPT_DIR%build.bat"
if not "%ERRORLEVEL%"=="0" exit /b 1

set "CLI=%BUILD_DIR%\Release\xpbd_cli.exe"
if not exist "%CLI%" set "CLI=%BUILD_DIR%\xpbd_cli.exe"
if not exist "%CLI%" (
  echo ERROR: xpbd_cli not found
  exit /b 1
)

echo.
echo === CLI startup smoke ===
"%CLI%" --help
if not "%ERRORLEVEL%"=="0" exit /b 1

echo.
echo Verify OK.
endlocal
exit /b 0
