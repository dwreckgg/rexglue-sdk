@echo off
rem Builds and runs the D3D12 blend factor choice host test with clang-cl (no CMake, no Catch2).
rem The header is header-only, so no .cpp is compiled beside the test. registers.h reaches simde
rem through rex/platform/fpscr.h, hence the thirdparty\simde include root.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "OUT=%TEMP%\rex_blend_factor_select_test"
if not exist "%OUT%" mkdir "%OUT%"
clang-cl /nologo /std:c++latest /EHsc /O2 -march=x86-64-v3 -I "%~dp0..\..\include" -I "%~dp0..\..\thirdparty\simde" "%~dp0blend_factor_select_host_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\blend_factor_select_host_test.exe"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\blend_factor_select_host_test.exe"
