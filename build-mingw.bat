@echo off
rem build with MinGW-w64 (gcc). Output: tor_rotate.exe
where gcc >nul 2>nul
if errorlevel 1 (
  echo gcc not found in PATH. Install MinGW-w64 or use build-msvc.bat
  exit /b 1
)
gcc -O2 -Wall -std=c99 -o tor_rotate.exe src\*.c -lws2_32
if errorlevel 1 exit /b 1
echo built tor_rotate.exe
