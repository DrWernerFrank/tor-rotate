@echo off
rem tor_rotate launcher - opens the console UI in this window
cd /d "%~dp0"
tor_rotate.exe ^
  --tor .\tor\tor\tor.exe ^
  --datadir .\data ^
  --mode timer --timer 120 ^
  --upstream 127.0.0.1:10808 ^
  --bridges-file .\bridges.txt ^
  --geoip .\tor\geoip
pause
