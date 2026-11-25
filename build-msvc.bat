@echo off
rem build with MSVC (run from a VS developer prompt). Output: tor_rotate.exe
where cl >nul 2>nul
if errorlevel 1 (
  echo cl not found. Open a "x64 Native Tools Command Prompt for VS" first.
  exit /b 1
)
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS src\*.c /Fe:tor_rotate.exe ws2_32.lib
if errorlevel 1 exit /b 1
echo built tor_rotate.exe
