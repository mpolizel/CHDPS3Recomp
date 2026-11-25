@echo off
rem Configure and build CHDPS3Recomp with clang-cl from Visual Studio.
rem Usage: build.bat [Release|RelWithDebInfo|Debug]
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (
    echo Visual Studio with "C++ Clang tools for Windows" not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1

cd /d "%~dp0"

rem Apply our ps3recomp patches (patches\ps3recomp\*.patch) to a clean submodule.
rem A submodule with local changes is assumed to be the patched working copy.
git -C ext\ps3recomp diff --quiet && (
    for %%p in (patches\ps3recomp\*.patch) do (
        echo Applying %%p
        git -C ext\ps3recomp apply "..\..\%%p" || exit /b 1
    )
)

if not exist build\build.ninja (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% ^
        -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
)
cmake --build build || exit /b 1
