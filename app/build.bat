@echo off
rem Builds app\dist\TorRotate.exe: one file with the GUI, tor, sing-box and wintun inside.
rem Run app\fetch-binaries.ps1 first to stage the bundled binaries.
cd /d "%~dp0"

for %%F in ("..\tor\tor\tor.exe" "..\tor\tor\pluggable_transports\lyrebird.exe" "..\tor\geoip" "..\tun\bin\sing-box.exe" "..\tun\bin\wintun.dll") do (
  if not exist %%F (echo missing %%F - run app\fetch-binaries.ps1 first & pause & exit /b 1)
)

python -m PyInstaller --version >nul 2>&1
if errorlevel 1 python -m pip install pyinstaller
if errorlevel 1 (echo could not install pyinstaller & pause & exit /b 1)

python -m PyInstaller --noconfirm --clean --onefile --windowed --uac-admin ^
  --name TorRotate ^
  --paths ..\tun ^
  --add-binary "..\tun\bin\sing-box.exe;tun\bin" ^
  --add-binary "..\tun\bin\wintun.dll;tun\bin" ^
  --add-binary "..\tor\tor\tor.exe;tor\tor" ^
  --add-binary "..\tor\tor\pluggable_transports\lyrebird.exe;tor\tor\pluggable_transports" ^
  --add-data "..\tor\geoip;tor" ^
  --add-data "..\tor\geoip6;tor" ^
  torrotate_gui.py
if errorlevel 1 (echo build failed & pause & exit /b 1)

echo.
echo built: %~dp0dist\TorRotate.exe
