@echo off
rem Builds and runs the submit snapshot host test with clang-cl (no CMake, no Catch2), at C++23 as the product
rem builds it (CMakeLists.txt: CMAKE_CXX_STANDARD 23).
rem   %1 = source root holding include\, src\ and tests\host\ (default: this checkout)
rem   %2 = output folder (default: %TEMP%\rex_submit_snapshot_test)
rem Exit code: the test's (1 when any check failed), or 1 with BUILD FAILED on a compile error.
setlocal
set "ROOT=%~1"
if "%ROOT%"=="" set "ROOT=%~dp0..\.."
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%TEMP%\rex_submit_snapshot_test"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\submit_snapshot_host_test.exe" del "%OUT%\submit_snapshot_host_test.exe"
clang-cl /nologo /clang:-std=c++23 /EHsc /O2 -I "%ROOT%\include" "%ROOT%\tests\host\submit_snapshot_host_test.cpp" "%ROOT%\src\graphics\submit_snapshot.cpp" /Fo"%OUT%\\" /Fe"%OUT%\submit_snapshot_host_test.exe"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\submit_snapshot_host_test.exe"
exit /b %ERRORLEVEL%
