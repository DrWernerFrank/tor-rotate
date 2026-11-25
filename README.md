# tor_rotate

Tor exit-node IP rotator in C. Spawns its own Tor instance, exposes a local
SOCKS5 listener, and rotates exit IPs via the Tor control port (`NEWNYM`).
Runs layered on top of xray/VPN: as long as your VPN or xray TUN is the system
route, Tor's traffic rides through it, giving you `VPN -> Tor -> exit` — your
real IP is hidden behind both the VPN and the rotating exit.

```
your app -> socks5://127.0.0.1:1080 -> tor (socks 9050) -> [xray/VPN] -> exit node
                                          ^ NEWNYM rotation via control port 9051
```

## Build (Windows)

- MinGW: `build-mingw.bat`
- MSVC (VS dev prompt): `build-msvc.bat`

Linux/macOS: `gcc -O2 -std=c99 -o tor_rotate src/*.c -lpthread`

## Get tor.exe

Download the **Tor Expert Bundle** (tor.win32) from torproject.org, put
`tor.exe` in `.\tor\` (or pass `--tor PATH`).

## Run

```
tor_rotate.exe
tor_rotate.exe --mode timer --timer 120
tor_rotate.exe --mode per --batch 5
tor_rotate.exe --upstream 127.0.0.1:10810            ; force tor through a local socks (sing-box, see below)
tor_rotate.exe --exclude {us},{gb}                    ; never these exit countries
tor_rotate.exe --include {de},{nl},{se}               ; pin to these exit countries
tor_rotate.exe --bridges-file bridges.txt             ; use obfs4/meek bridges (recommended if
                                                      ;   relay TLS is blocked on your network)
tor_rotate.exe --bridge "obfs4 IP:PORT FP cert=... iat-mode=0"   ; inline bridge lines
tor_rotate.exe --geoip .\tor\geoip                    ; enable exit country display
```

Point any SOCKS5 client (browser, curl, etc.) at `127.0.0.1:1080`.

## Web UI

A dashboard runs alongside the console at **http://127.0.0.1:8080** (localhost
only). It shows the current exit, stats, rotation mode/timer/batch, the built
circuits and a live log, and has buttons for rotate, pause and the mode
settings. Use `--web PORT` to change the port or `--no-web` to disable it.

It is also available as a JSON API: `GET /api/status`, `GET /api/circuits`,
and `POST /api/rotate|pause`, `POST /api/mode?set=manual|per|timer|off`,
`POST /api/timer?sec=N`, `POST /api/batch?n=N`. POST requests must send the
header `X-Tor-Rotate: 1` and the Host must be `127.0.0.1` or `localhost`.

## Whole-network routing (sing-box TUN)

`tun/` routes all system traffic through `tor_rotate` using sing-box's TUN mode:

```
apps -> sing-box TUN -> socks 127.0.0.1:1080 (tor_rotate) -> tor -> socks 127.0.0.1:10810 -> your proxy -> internet
```

1. Turn off any other TUN client (for example Happ's TUN mode). Only one TUN can own the default route.
2. Copy `tun\proxy.txt.example` to `tun\proxy.txt` and paste your share link
   (`ss://`, `vless://`, `vmess://` or `trojan://`). Change it any time; it's
   read on each start.
3. From an **Administrator** terminal, run `tun\start-tun.bat`. It generates
   `tun\config.json` from your link, validates it and starts sing-box.
4. Start `tor_rotate` with `run.bat`. Keep it running: while the tunnel is up,
   all traffic goes through it, so closing it cuts off the network.
5. Stop with `tun\stop-tun.bat`. Removing the adapter and routes happens when sing-box exits.

Notes:
- DNS goes through Tor (DNS-over-HTTPS via the Tor socks), so lookups don't leak.
- UDP is rejected on purpose. Tor can't carry UDP, so QUIC and game traffic fall back or fail rather than leaking outside Tor.
- Files in `tun\`: `make_config.py` (link -> config), `start-tun.bat`, `stop-tun.bat`,
  `bin\` (sing-box.exe and wintun.dll, downloaded separately; see below).
- Binaries: sing-box from https://github.com/SagerNet/sing-box/releases (windows-amd64),
  and `wintun.dll` from https://www.wintun.net, both into `tun\bin\`.
- `tun\proxy.txt` and `tun\config.json` contain your credentials and are gitignored.

## Bridges

If Tor relays are TLS-fingerprinted and blocked on your network (stuck at
"Handshaking with a relay"), use bridges. The Tor Expert Bundle ships
`lyrebird.exe` (obfs4 + meek) — the app wires it up automatically when bridge
lines are provided. Fresh obfs4 lines: https://bridges.torproject.org
(choose obfs4, paste into `bridges.txt`, one per line). When chaining through
xray AND bridges, Tor connects to bridges via your `--upstream` proxy.

## Keys (console)

| key | action |
|-----|--------|
| `r` | rotate now (NEWNYM + force circuit build) |
| `m` | cycle mode: manual -> per-request -> timer -> off |
| `t` | cycle timer interval: 60/180/300/600/1800/3600 s |
| `n` | cycle per-request batch: 1/5/10/25/50 |
| `e` | list current built circuits (exits) |
| `p` | pause/resume rotation |
| `h` | help, `q` quit |

## VPN chaining notes

- **System TUN VPN (wireguard/openvpn/xray tun):** nothing needed — Tor dials
  out through the TUN automatically. Chain = VPN -> Tor.
- **xray as local proxy only (not system-wide):** pass `--upstream 127.0.0.1:PORT`
  (xray's inbound socks). Tor then dials through xray explicitly via
  `Socks5Proxy`, so tor-in-vpn even without a TUN.
- **Whole-network (`tun\`):** `run.bat` points Tor at sing-box's local socks on
  `127.0.0.1:10810`. See "Whole-network routing" above.
- Both layers applied = your VPN IP connects to Tor guards, exits rotate.
- A guard relay is sticky for months by design; that is the *first* hop after
  your VPN and is normal. Exits rotate; guards do not (unless you wipe `data\`).

## Caveats

- `NEWNYM` doesn't clear cookies/DNS caches of browsers — for identity
  separation use separate browser profiles or Tor Browser itself.
- Exit selection via `--include`/`--exclude` uses country codes or
  fingerprints/families; see Tor's manual for the syntax.
- Ports 9050/9051/1080/8080 (web UI) must be free or remap with flags
  (`--socks`, `--control`, `--listen`, `--web`). The sing-box setup also uses
  10810 for Tor's upstream; leave it free for `tun\start-tun.bat`.
