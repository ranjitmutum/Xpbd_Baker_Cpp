@echo off
setlocal EnableExtensions

if "%VCPKG_ROOT%"=="" (
  echo ERROR: VCPKG_ROOT is not set.
  echo Install vcpkg and set VCPKG_ROOT to its root directory.
  exit /b 1
)

set "SCRIPT_DIR=%~dp0"
set "VCPKG_INSTALLED=%SCRIPT_DIR%vcpkg_installed"
set "BUILD_DIR=%SCRIPT_DIR%build"

echo Using VCPKG_ROOT=%VCPKG_ROOT%
echo Using shared VCPKG_INSTALLED_DIR=%VCPKG_INSTALLED%
echo.

if exist "%BUILD_DIR%\CMakeCache.txt" (
  findstr /C:"CMAKE_GENERATOR_PLATFORM:INTERNAL=x64" "%BUILD_DIR%\CMakeCache.txt" >nul
  if errorlevel 1 (
    echo Clearing stale build cache...
    rmdir /s /q "%BUILD_DIR%"
  )
)

cmake -S "%SCRIPT_DIR%." -B "%BUILD_DIR%" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_INSTALLED_DIR="%VCPKG_INSTALLED%" -DXPBD_BUILD_TESTS=ON -DXPBD_BUILD_CLI=ON -DXPBD_BUILD_APP=OFF
if errorlevel 1 (
  echo ERROR: cmake configure failed.
  exit /b 1
)

cmake --build "%BUILD_DIR%" --config Release --parallel 8 --target xpbd_core --target xpbd_cli --target xpbd_tests
if errorlevel 1 (
  echo ERROR: cmake build failed.
  exit /b 1
)

ctest --test-dir "%BUILD_DIR%" -C Release --output-on-failure
if errorlevel 1 (
  echo ERROR: tests failed.
  exit /b 1
)

echo.
echo Build and tests succeeded.
echo Packages live in: %VCPKG_INSTALLED%
endlocal
exit /b 0
