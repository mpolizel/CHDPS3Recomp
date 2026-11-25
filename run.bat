@echo off
rem Run CHDPS3Recomp against the extracted game.
rem game\vfs\PS3_GAME is a junction to game\base (the extracted base PKG):
rem   mklink /J game\vfs\PS3_GAME game\base
cd /d "%~dp0"
set PS3_VFS_ROOT=game/vfs
build\CHDPS3Recomp.exe game\decrypted\USRDIR\EBOOT.elf > out\run.log 2>&1
