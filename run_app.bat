@echo off
setlocal EnableExtensions
if "%VCPKG_ROOT%"=="" (
  echo ERROR: VCPKG_ROOT is not set.
  exit /b 1
)
set "SCRIPT_DIR=%~dp0"
set "VCPKG_INSTALLED=%SCRIPT_DIR%vcpkg_installed"
set "BUILD_DIR=%SCRIPT_DIR%build-app"
if not "%VULKAN_SDK%"=="" echo Using VULKAN_SDK=%VULKAN_SDK%
echo Backend flags: -gl  -vk  -d3d/-dx11  -ml   (or env XPBD_GFX=opengl^|vulkan^|dx11^|auto)
if exist "%BUILD_DIR%\CMakeCache.txt" (
  findstr /C:"CMAKE_GENERATOR_PLATFORM:INTERNAL=x64" "%BUILD_DIR%\CMakeCache.txt" >nul
  if errorlevel 1 rmdir /s /q "%BUILD_DIR%"
)
if exist "%BUILD_DIR%\CMakeCache.txt" (
  findstr /C:"eui_neo" "%BUILD_DIR%\CMakeCache.txt" >nul
  if not errorlevel 1 (
    echo Clearing build-app cache (was EUI-based)...
    rmdir /s /q "%BUILD_DIR%"
  )
)
cmake -S "%SCRIPT_DIR%." -B "%BUILD_DIR%" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_INSTALLED_DIR="%VCPKG_INSTALLED%" -DXPBD_BUILD_TESTS=OFF -DXPBD_BUILD_CLI=ON -DXPBD_BUILD_APP=ON
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%" --config Release --parallel 8 --target xpbd_baker_app
if errorlevel 1 exit /b 1
if exist "%BUILD_DIR%\Release\xpbd_baker_app.exe" (
  start "" "%BUILD_DIR%\Release\xpbd_baker_app.exe" %*
) else if exist "%BUILD_DIR%\xpbd_baker_app.exe" (
  start "" "%BUILD_DIR%\xpbd_baker_app.exe" %*
) else (
  echo Executable not found.
  exit /b 1
)
endlocal
exit /b 0
