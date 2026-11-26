@echo off
rem Build CHDLauncher.exe with the portable toolchain the installer downloads
rem (cache\toolchain) and copy it to the project root, where the release has it.
setlocal
cd /d "%~dp0"
set T=%~dp0cache\toolchain
if not exist "%T%\llvm-mingw-20261117-ucrt-x86_64\bin\clang++.exe" (
    echo The portable toolchain is not in cache\toolchain yet: run the installer once first.
    exit /b 1
)
set PATH=%T%\llvm-mingw-20261117-ucrt-x86_64\bin;%T%\cmake-4.5.1-windows-x86_64\bin;%T%\ninja;%SystemRoot%\System32;%SystemRoot%
if not exist build-launcher\build.ninja (
    cmake -S launcher -B build-launcher -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ || exit /b 1
)
cmake --build build-launcher || exit /b 1
copy /y build-launcher\CHDLauncher.exe CHDLauncher.exe >nul
echo CHDLauncher.exe built.
