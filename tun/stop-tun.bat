@echo off
rem Stops sing-box. Its TUN adapter and routes are removed when it exits.
taskkill /IM sing-box.exe /F
pause
