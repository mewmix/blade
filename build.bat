@echo off
setlocal

rem Get Git Commit SHA
for /f "delims=" %%i in ('git rev-parse HEAD') do set COMMIT_SHA=%%i

rem Define Version
set APP_VERSION=1.0.3

rem Create version.h
echo #define VERSION "%APP_VERSION%" > version.h
echo #define COMMIT_SHA "%COMMIT_SHA%" >> version.h

rem Compile blade_tui.c
gcc -O3 -mavx2 blade_tui.c -o blade.exe
echo TUI Build complete.
gcc -O3 -mavx2 -mwindows blade_gui.c -o BladeExplorer.exe -lgdi32 -luser32 -lshell32 -lole32 -lcomctl32
echo GUI Build complete.
endlocal