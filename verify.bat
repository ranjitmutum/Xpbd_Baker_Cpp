@echo off
setlocal EnableExtensions

if "%VCPKG_ROOT%"=="" (
  echo ERROR: VCPKG_ROOT is not set.
  exit /b 1
)

set "SCRIPT_DIR=%~dp0"
set "FIXTURES=%SCRIPT_DIR%tests\fixtures"
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

set "OUT_DIR=%TEMP%\xpbd_verify_%RANDOM%"
mkdir "%OUT_DIR%" 2>nul

echo.
echo === CLI bake smoke ===
"%CLI%" bake --model "%FIXTURES%\chain.geo.json" --anim "%FIXTURES%\chain.animation.json" --out "%OUT_DIR%\chain.baked.json" --bones root,mid,tip --loop once --velocity "%OUT_DIR%\chain.velocity.json"
if not "%ERRORLEVEL%"=="0" exit /b 1

if not exist "%OUT_DIR%\chain.baked.json" (
  echo ERROR: bake output missing
  exit /b 1
)

echo.
echo Verify OK.
echo   CLI bake output: %OUT_DIR%\chain.baked.json
endlocal
exit /b 0