# Downloads the third-party binaries TorRotate bundles and lays them out where the build expects them:
#   tor\tor\tor.exe, tor\tor\pluggable_transports\lyrebird.exe, tor\geoip, tor\geoip6
#   tun\bin\sing-box.exe, tun\bin\wintun.dll
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File app\fetch-binaries.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tmp = Join-Path $env:TEMP "torrotate-fetch"
New-Item -ItemType Directory -Force $tmp | Out-Null

$singbox = "1.14.4"
$wintun = "0.14.1"
$tor = "16.0.3"

Invoke-WebRequest "https://github.com/SagerNet/sing-box/releases/download/v$singbox/sing-box-$singbox-windows-amd64.zip" -OutFile "$tmp\sb.zip"
Expand-Archive "$tmp\sb.zip" "$tmp\sb" -Force
New-Item -ItemType Directory -Force "$root\tun\bin" | Out-Null
Copy-Item "$tmp\sb\sing-box-$singbox-windows-amd64\sing-box.exe" "$root\tun\bin\" -Force

Invoke-WebRequest "https://www.wintun.net/builds/wintun-$wintun.zip" -OutFile "$tmp\wintun.zip"
Expand-Archive "$tmp\wintun.zip" "$tmp\wintun" -Force
Copy-Item "$tmp\wintun\wintun\bin\amd64\wintun.dll" "$root\tun\bin\" -Force

Invoke-WebRequest "https://dist.torproject.org/torbrowser/$tor/tor-expert-bundle-windows-x86_64-$tor.tar.gz" -OutFile "$tmp\tor.tar.gz"
New-Item -ItemType Directory -Force "$tmp\tor" | Out-Null
tar -xzf "$tmp\tor.tar.gz" -C "$tmp\tor"
New-Item -ItemType Directory -Force "$root\tor\tor" | Out-Null
Copy-Item "$tmp\tor\tor\*" "$root\tor\tor" -Recurse -Force
Copy-Item "$tmp\tor\data\geoip*" "$root\tor\" -Force

Write-Host "done: binaries staged under tor\ and tun\bin\"
