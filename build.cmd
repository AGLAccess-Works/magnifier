@echo off
rem Builds Magnifier with the Visual Studio 2022 C++ tools.
rem   build.cmd            Release build into .\build
rem   build.cmd Debug      Debug build into .\build-debug
rem   build.cmd UIAccess   Release build with uiAccess="true" into .\build-uiaccess
rem                        (it only starts once signed and installed: tools\install_uiaccess.ps1)
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set OUT=build
set UIACCESS=OFF
if /I "%CONFIG%"=="Debug" set OUT=build-debug
if /I "%CONFIG%"=="UIAccess" (
    set CONFIG=Release
    set OUT=build-uiaccess
    set UIACCESS=ON
)

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
    echo Visual Studio C++ build tools not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cmake -S "%~dp0." -B "%~dp0%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DMAGNIFIER_UIACCESS=%UIACCESS% || exit /b 1
cmake --build "%~dp0%OUT%" || exit /b 1
ctest --test-dir "%~dp0%OUT%" --output-on-failure || exit /b 1
echo.
echo Built %~dp0%OUT%\Magnifier.exe
