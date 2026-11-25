@echo off
rem Starts sing-box in TUN mode: all system traffic -> tor_rotate (127.0.0.1:1080).
rem Must run from an Administrator terminal. Turn Happ's TUN mode OFF first.
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
  echo This needs an Administrator terminal. Right-click and "Run as administrator".
  pause
  exit /b 1
)

if not exist proxy.txt (
  copy proxy.txt.example proxy.txt >nul
  echo Created proxy.txt. Paste your share link into it, save, then run this again.
  notepad proxy.txt
  pause
  exit /b 0
)

python make_config.py
if errorlevel 1 (pause & exit /b 1)

bin\sing-box.exe check -c config.json
if errorlevel 1 (
  echo config.json is invalid - see the error above.
  pause
  exit /b 1
)

start "sing-box TUN" bin\sing-box.exe run -c config.json
echo sing-box is running in its own window.
echo Now start tor_rotate with run.bat ^(it uses the upstream on 127.0.0.1:10810^).
pause
