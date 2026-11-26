@echo off
rem Install Castlevania: Harmony of Despair from your own PKG.
rem Usage: install.bat <game.pkg> [licence.rap]
rem Everything else (firmware decoder, decryption, translation, build) is automatic.
setlocal
cd /d "%~dp0"

if not exist ext\ps3recomp\tools\ppu_lifter.py (
    git submodule update --init || exit /b 1
)
if not exist .venv\Scripts\python.exe (
    echo Setting up Python...
    py -3 -m venv .venv || python -m venv .venv || exit /b 1
    .venv\Scripts\python -m pip install -q -r ext\ps3recomp\requirements.txt || exit /b 1
)

set ARGS=
if not "%~1"=="" set ARGS=--pkg "%~1"
if not "%~2"=="" set ARGS=%ARGS% --rap "%~2"
.venv\Scripts\python -u tools\install.py %ARGS%
