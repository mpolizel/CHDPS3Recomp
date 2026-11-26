@echo off
rem Run CHDPS3Recomp against the extracted game.
rem game\vfs\PS3_GAME is a junction to game\base (the extracted base PKG):
rem   mklink /J game\vfs\PS3_GAME game\base
rem The installer builds into build-mingw; build.bat (Visual Studio) into build.
cd /d "%~dp0"
set PS3_VFS_ROOT=game/vfs
set EXE=build-mingw\CHDPS3Recomp.exe
if not exist %EXE% set EXE=build\CHDPS3Recomp.exe
if not exist out mkdir out
%EXE% game\decrypted\USRDIR\EBOOT.elf > out\run.log 2>&1
