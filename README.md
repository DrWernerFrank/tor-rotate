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
tor_rotate.exe --upstream 127.0.0.1:10808            ; force tor through xray's local socks
tor_rotate.exe --exclude {us},{gb}                    ; never these exit countries
tor_rotate.exe --include {de},{nl},{se}               ; pin to these exit countries
tor_rotate.exe --bridges-file bridges.txt             ; use obfs4/meek bridges (recommended if
                                                      ;   relay TLS is blocked on your network)
tor_rotate.exe --bridge "obfs4 IP:PORT FP cert=... iat-mode=0"   ; inline bridge lines
tor_rotate.exe --geoip .\tor\geoip                    ; enable exit country display
```

Point any SOCKS5 client (browser, curl, etc.) at `127.0.0.1:1080`.

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
- Both layers applied = your VPN IP connects to Tor guards, exits rotate.
- A guard relay is sticky for months by design; that is the *first* hop after
  your VPN and is normal. Exits rotate; guards do not (unless you wipe `data\`).

## Caveats

- `NEWNYM` doesn't clear cookies/DNS caches of browsers — for identity
  separation use separate browser profiles or Tor Browser itself.
- Exit selection via `--include`/`--exclude` uses country codes or
  fingerprints/families; see Tor's manual for the syntax.
- Ports 9050/9051/1080 must be free or remap with flags.
