@echo off
setlocal EnableExtensions

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin;C:\Program Files\LLVM\bin;C:\Users\chron\AppData\Local\Microsoft\WinGet\Links;%PATH%"

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build-ninja"
set "PATCHES_CC=C:/Program Files/LLVM/bin/clang.exe"
set "PATCHES_LD=C:/Program Files/LLVM/bin/ld.lld.exe"

cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl ^
  -DCMAKE_CXX_COMPILER=clang-cl ^
  -DWR64_STUB_PATCHES=ON ^
  "-DPATCHES_C_COMPILER=%PATCHES_CC%" ^
  "-DPATCHES_LD=%PATCHES_LD%" ^
  "-DCMAKE_CXX_FLAGS=-Xclang -fexceptions -Xclang -fcxx-exceptions"

if errorlevel 1 exit /b 1

cmake --build "%BUILD%" --target WaveRace64Recompiled -j 8
exit /b %ERRORLEVEL%
