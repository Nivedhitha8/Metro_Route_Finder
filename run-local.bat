@echo off
REM Builds and runs Metro Route Finder on http://localhost:8080
REM Needs gcc (MinGW-w64 / MSYS2 / WinLibs) on PATH.
cd /d "%~dp0"
where gcc >nul 2>nul || (echo gcc not found. Install MinGW-w64 and add it to PATH. & pause & exit /b 1)
gcc -O2 -o metro.exe metro.c -lws2_32 || (pause & exit /b 1)
metro.exe
