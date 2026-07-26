@echo off
rem Configure and build the thorstream host with MSVC + Ninja.
setlocal

set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;C:\Program Files\CMake\bin;%PATH%"

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo Failed to initialise the MSVC environment.
  exit /b 1
)

rem %~dp0 ends with a backslash, which would escape the closing quote below.
set "SRC=%~dp0"
set "SRC=%SRC:~0,-1%"
set "BUILD=%SRC%\build"

cmake -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
cmake --build "%BUILD%" || exit /b 1

echo.
echo Built: %BUILD%\thorstream-host.exe
