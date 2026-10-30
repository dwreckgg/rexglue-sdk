@echo off
rem Builds and runs the speakers host test with clang-cl (no CMake, no Catch2).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "OUT=%TEMP%\rex_speakers_test"
if not exist "%OUT%" mkdir "%OUT%"
clang-cl /nologo /std:c++latest /EHsc /O2 -march=x86-64-v3 -I "%~dp0..\..\include" "%~dp0speakers_host_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\speakers_host_test.exe"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\speakers_host_test.exe"
