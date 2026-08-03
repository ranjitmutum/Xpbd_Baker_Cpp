@echo off
setlocal EnableExtensions

if "%VCPKG_ROOT%"=="" (
  echo ERROR: VCPKG_ROOT is not set.
  exit /b 1
)

set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%out\build\vscode-windows-app"

echo === XPBD Baker C++ verify ===
cmake --preset vscode-windows-app
if errorlevel 1 (
  echo ERROR: cmake configure failed.
  exit /b 1
)

cmake --build --preset vscode-windows-app-release --parallel 8
if errorlevel 1 (
  echo ERROR: desktop app build failed.
  exit /b 1
)

cmake --build "%BUILD_DIR%" --config Release --target xpbd_cli --parallel 8
if errorlevel 1 (
  echo ERROR: CLI build failed.
  exit /b 1
)

set "CLI=%BUILD_DIR%\Release\xpbd_cli.exe"
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
