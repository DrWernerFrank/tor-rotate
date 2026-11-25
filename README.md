# TorRotate

Routes your whole Windows system through Tor and rotates the exit node. A single
window starts sing-box (system-wide TUN) and Tor, shows the current exit and
rotates it on demand or on a timer.

```
apps -> sing-box TUN -> tor (socks 127.0.0.1:9050) -> sing-box socks (127.0.0.1:10810) -> your proxy -> internet
                          ^ NEWNYM rotation via tor control port 9051
```

The repo also contains `tor_rotate`, a standalone C command-line rotator with a
local SOCKS5 front end and web UI (`src/`, see "C command-line tool" below).

## Download

Grab `TorRotate.exe` from the latest GitHub release. It bundles tor, sing-box and
wintun, and requests administrator rights on start (TUN mode needs them).

## Build the exe yourself

1. Stage the third-party binaries (downloads tor, sing-box and wintun):
   ```
   powershell -ExecutionPolicy Bypass -File app\fetch-binaries.ps1
   ```
2. Build (needs Python 3.12+ with Tk and PyInstaller; the script installs PyInstaller if missing):
   ```
   app\build.bat
   ```
   Output: `app\dist\TorRotate.exe`.

To run from source without building, run `python app\torrotate_gui.py` after step 1.

## Using the app

1. **Connection** tab: paste one share link (`ss://`, `vless://`, `vmess://`, `trojan://`)
   and press *Check link*. Add bridge lines only if Tor relays are blocked on your network.
2. **sing-box** tab: adjust the TUN, DNS and logging options (see below). Press *Check config*
   to validate them with sing-box without connecting.
3. **Dashboard** tab: press *Connect*. Rotate with *Rotate now*, or choose *Timer* mode and an interval.
4. *Disconnect* stops Tor and sing-box and restores the normal network.

Settings are saved to `%APPDATA%\TorRotate\settings.json`. Logs are in the same folder
(`tor.log`, `sing-box.log`, `config.json` is the last generated sing-box config).

### sing-box options

| option | default | what it does |
|---|---|---|
| Network stack | `mixed` | TUN stack: `mixed`, `system` or `gvisor`. Switch if an app misbehaves. |
| MTU | `9000` | TUN MTU, 576-9000. Lower it (e.g. 1400-1500) if large downloads stall. |
| Strict route | on | Blocks traffic from leaving the TUN outside the tunnel. |
| Bypass LAN | on | Sends private/LAN addresses directly instead of through Tor. |
| Block UDP | on | Rejects UDP, which Tor cannot carry. QUIC and games fall back to TCP or fail. |
| DoH server | `1.1.1.1` | DNS-over-HTTPS resolver, reached through Tor. Lookups don't leak. |
| Strategy | `prefer_ipv4` | DNS strategy: `prefer_ipv4`, `ipv4_only` or `prefer_ipv6`. |
| Log level | `info` | `debug`, `info`, `warn` or `error`. |
| Direct rules | empty | Domains or IPs/CIDRs that bypass Tor. **They are not anonymised.** |

### Keys and notes

- DNS goes through Tor, so lookups don't leak.
- `NEWNYM` changes the exit but does not clear browser cookies or DNS caches. Use separate
  browser profiles or Tor Browser for identity separation.
- Guard relays are sticky by design; exits rotate.
- Only one TUN client can own the default route. Turn off other TUN VPNs first.
- Ports 9050 (tor socks), 9051 (tor control) and 10810 (sing-box upstream) must be free.

## Bridges

If Tor relays are blocked (stuck at "Connecting to Tor… 0%" or "Handshaking with a relay"), add obfs4
bridges on the Connection tab. The Tor Expert Bundle ships `lyrebird.exe`, which the app wires up
automatically. Fresh lines: https://bridges.torproject.org (choose obfs4).

## Command-line TUN setup (no GUI)

`tun\` contains the same sing-box flow for scripts:

1. Copy `tun\proxy.txt.example` to `tun\proxy.txt` and paste your link.
2. From an **Administrator** terminal, run `tun\start-tun.bat`. It writes `tun\config.json` and starts sing-box.
3. Stop with `tun\stop-tun.bat`.

`tun\make_config.py` builds the config; the GUI calls the same function with the options above.
`tun\proxy.txt` and `tun\config.json` contain credentials and are gitignored.

## C command-line tool (`tor_rotate`)

A standalone rotator in C (`src/`). Build with `build-mingw.bat` or `build-msvc.bat`
(Linux/macOS: `gcc -O2 -std=c99 -o tor_rotate src/*.c -lpthread`). Put `tor.exe` in
`.\tor\tor\` or pass `--tor PATH`.

```
tor_rotate.exe --mode timer --timer 120
tor_rotate.exe --mode per --batch 5
tor_rotate.exe --upstream 127.0.0.1:10810          ; tor through a local socks (sing-box)
tor_rotate.exe --exclude {us},{gb} --include {de}  ; exit country rules
tor_rotate.exe --bridges-file bridges.txt
tor_rotate.exe --web 8080 | --no-web               ; web dashboard on 127.0.0.1
```

Console keys: `r` rotate, `m` cycle mode, `t` timer interval, `n` batch size, `e` list circuits,
`p` pause, `h` help, `q` quit. The web UI JSON API is `GET /api/status|circuits` and
`POST /api/rotate|pause|mode|timer|batch` (POST needs header `X-Tor-Rotate: 1`).

## Caveats

- Traffic on bypass rules leaves your machine without Tor.
- Exit selection with `--include`/`--exclude` takes country codes or fingerprints; see Tor's manual.
