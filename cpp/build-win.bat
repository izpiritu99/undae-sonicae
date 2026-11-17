@echo off
rem Windows build of the native edition: MSVC + the CMake and Ninja that ship with Visual Studio.
rem Usage (from any prompt):  cpp\build-win.bat [Release|Debug]
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with the C++ tools was not found. & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem Put Visual Studio's CMake and Ninja first, ahead of any MinGW copies on PATH.
set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0.."
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl || exit /b 1
cmake --build cpp/build || exit /b 1
