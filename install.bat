@echo off
rem Install Castlevania: Harmony of Despair from your own PKG.
rem   install.bat <game.pkg> [licence.rap]
rem   install.bat --pkg=<game.pkg> [--rap=<licence.rap>]   (as RecompLauncher calls it)
rem Everything else (firmware decoder, decryption, translation, build) is automatic.
rem CHDLauncher.exe does the same with a window and no console.
setlocal
cd /d "%~dp0"

rem A release package carries its own Python (python\) and a patched ps3recomp.
set PY=python\python.exe
if exist %PY% goto run

if not exist ext\ps3recomp\tools\ppu_lifter.py (
    git submodule update --init || exit /b 1
)
set PY=.venv\Scripts\python.exe
if not exist %PY% (
    echo Setting up Python...
    py -3 -m venv .venv || python -m venv .venv || exit /b 1
    .venv\Scripts\python -m pip install -q -r ext\ps3recomp\requirements.txt || exit /b 1
)

:run
set "FIRST=%~1"
if "%FIRST:~0,2%"=="--" (
    %PY% -u tools\install.py %*
    exit /b %ERRORLEVEL%
)
set ARGS=
if not "%~1"=="" set ARGS=--pkg "%~1"
if not "%~2"=="" set ARGS=%ARGS% --rap "%~2"
%PY% -u tools\install.py %ARGS%
exit /b %ERRORLEVEL%
