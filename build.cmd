@echo off
setlocal
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=debug"
rem Plan 9: a -x86 preset builds the 32-bit half with the x86 tools; every other preset the 64-bit half.
set "VCVARS=vcvars64.bat"
if /i "%PRESET:~-4%"=="-x86" set "VCVARS=vcvars32.bat"
rem The newest Visual Studio with the C++ tools, any edition (vswhere ships with the Visual Studio Installer).
set "VSDIR="
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSDIR=%%i"
if not defined VSDIR set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\18\Community"
if not exist "%VSDIR%\VC\Auxiliary\Build\%VCVARS%" (
  echo build.cmd: Visual Studio with the "Desktop development with C++" workload was not found
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
call tools\fetch_deps.cmd || exit /b 1
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
