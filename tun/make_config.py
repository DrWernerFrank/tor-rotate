"""make_config.py - build a sing-box config from a proxy share link.

Reads the first non-comment line of proxy.txt (ss:// vless:// vmess:// trojan://)
and writes config.json. start-tun.bat calls this; the GUI calls build() directly
with the options from its sing-box tab.

Traffic flow:
  apps -> sing-box TUN -> socks 127.0.0.1:9050 (tor) -> socks 127.0.0.1:10810 (sing-box) -> proxy -> internet
  Tor's upstream (10810) is a local socks inbound that sing-box sends straight to your proxy.
"""
import base64
import ipaddress
import json
import os
import sys
from urllib.parse import unquote, parse_qs, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
PROXY_FILE = os.path.join(HERE, "proxy.txt")
CONFIG_FILE = os.path.join(HERE, "config.json")

TOR_SOCKS = 9050        # tor SocksPort
UPSTREAM_PORT = 10810   # tor Socks5Proxy: sing-box's local socks to your proxy

LOG_LEVELS = ("debug", "info", "warn", "error")
STACKS = ("mixed", "system", "gvisor")
DNS_STRATEGIES = ("prefer_ipv4", "ipv4_only", "prefer_ipv6")

DEFAULTS = {
    "log_level": "info",
    "stack": "mixed",
    "mtu": 9000,
    "strict_route": True,
    "bypass_lan": True,
    "block_udp": True,
    "dns_server": "1.1.1.1",
    "dns_strategy": "prefer_ipv4",
    "direct_rules": [],
}


def b64dec(s):
    s = s.strip().replace("-", "+").replace("_", "/")
    s += "=" * (-len(s) % 4)
    return base64.b64decode(s).decode("utf-8", "replace")


def split_hostport(hp):
    if hp.startswith("["):
        host, _, port = hp[1:].partition("]:")
        return host, int(port)
    host, _, port = hp.rpartition(":")
    return host, int(port)


def tls_block(q, host, force=False):
    security = q.get("security", [""])[0]
    if not force and security not in ("tls", "reality"):
        return None
    tls = {"enabled": True, "server_name": q.get("sni", [host])[0] or host}
    fp = q.get("fp", [""])[0]
    if fp:
        tls["utls"] = {"enabled": True, "fingerprint": fp}
    if security == "reality":
        tls.setdefault("utls", {"enabled": True, "fingerprint": fp or "chrome"})
        tls["reality"] = {
            "enabled": True,
            "public_key": q.get("pbk", [""])[0],
            "short_id": q.get("sid", [""])[0],
        }
    alpn = q.get("alpn", [""])[0]
    if alpn:
        tls["alpn"] = alpn.split(",")
    if q.get("allowInsecure", ["0"])[0] in ("1", "true"):
        tls["insecure"] = True
    return tls


def transport(net, path, host, service=""):
    if net == "ws":
        t = {"type": "ws", "path": path or "/"}
        if host:
            t["headers"] = {"Host": host}
        return t
    if net == "grpc":
        return {"type": "grpc", "service_name": service or path}
    if net in ("http", "h2"):
        t = {"type": "http", "path": path or "/"}
        if host:
            t["host"] = [host]
        return t
    if net == "httpupgrade":
        t = {"type": "httpupgrade", "path": path or "/"}
        if host:
            t["host"] = host
        return t
    return None  # tcp / raw / none


def parse_ss(link):
    body = link[len("ss://"):].split("#")[0].split("?")[0]
    if "@" in body:
        userinfo, _, hostport = body.rpartition("@")
        userinfo = unquote(userinfo)
        if ":" not in userinfo:
            userinfo = b64dec(userinfo)
    else:
        decoded = b64dec(body)
        userinfo, _, hostport = decoded.rpartition("@")
    method, _, password = userinfo.partition(":")
    host, port = split_hostport(hostport)
    return {"type": "shadowsocks", "server": host, "server_port": port,
            "method": method, "password": unquote(password)}


def parse_vless(link):
    u = urlsplit(link)
    uuid, _, hostport = u.netloc.rpartition("@")
    host, port = split_hostport(hostport)
    q = parse_qs(u.query)
    out = {"type": "vless", "server": host, "server_port": port, "uuid": unquote(uuid)}
    flow = q.get("flow", [""])[0]
    if flow:
        out["flow"] = flow
    tls = tls_block(q, host)
    if tls:
        out["tls"] = tls
    tr = transport(q.get("type", ["tcp"])[0], q.get("path", [""])[0],
                   q.get("host", [""])[0], q.get("serviceName", [""])[0])
    if tr:
        out["transport"] = tr
    return out


def parse_vmess(link):
    j = json.loads(b64dec(link[len("vmess://"):]))
    host = j["add"]
    out = {"type": "vmess", "server": host, "server_port": int(j["port"]),
           "uuid": j["id"], "security": j.get("scy") or "auto"}
    q = {"security": ["tls"] if j.get("tls") == "tls" else [""],
         "sni": [j.get("sni", "")], "fp": [j.get("fp", "")], "alpn": [j.get("alpn", "")]}
    tls = tls_block(q, host)
    if tls:
        out["tls"] = tls
    tr = transport(j.get("net", "tcp"), j.get("path", ""), j.get("host", ""), j.get("path", ""))
    if tr:
        out["transport"] = tr
    return out


def parse_trojan(link):
    u = urlsplit(link)
    password, _, hostport = u.netloc.rpartition("@")
    host, port = split_hostport(hostport)
    q = parse_qs(u.query)
    out = {"type": "trojan", "server": host, "server_port": port, "password": unquote(password)}
    out["tls"] = tls_block(q, host, force=True)
    tr = transport(q.get("type", ["tcp"])[0], q.get("path", [""])[0],
                   q.get("host", [""])[0], q.get("serviceName", [""])[0])
    if tr:
        out["transport"] = tr
    return out


PARSERS = {"ss://": parse_ss, "vless://": parse_vless,
           "vmess://": parse_vmess, "trojan://": parse_trojan}


def read_link():
    if not os.path.exists(PROXY_FILE):
        sys.exit("proxy.txt not found. Copy proxy.txt.example to proxy.txt and paste your link.")
    with open(PROXY_FILE, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                return line
    sys.exit("proxy.txt has no link in it.")


def split_direct_rules(rules):
    """Split bypass entries into (domain suffixes, CIDRs). Accepts a list or a multi-line string."""
    if isinstance(rules, str):
        rules = rules.splitlines()
    domains, cidrs = [], []
    for raw in rules:
        item = raw.strip().lower()
        if not item or item.startswith("#"):
            continue
        try:
            cidrs.append(str(ipaddress.ip_network(item, strict=False)))
        except ValueError:
            domains.append(item.lstrip("."))
    return domains, cidrs


def checked_options(opts):
    o = {**DEFAULTS, **(opts or {})}
    if o["log_level"] not in LOG_LEVELS:
        raise ValueError(f"log level must be one of: {', '.join(LOG_LEVELS)}")
    if o["stack"] not in STACKS:
        raise ValueError(f"TUN stack must be one of: {', '.join(STACKS)}")
    if o["dns_strategy"] not in DNS_STRATEGIES:
        raise ValueError(f"DNS strategy must be one of: {', '.join(DNS_STRATEGIES)}")
    try:
        o["mtu"] = int(str(o["mtu"]).strip())
    except ValueError:
        raise ValueError("MTU must be a whole number") from None
    if not 576 <= o["mtu"] <= 9000:
        raise ValueError("MTU must be between 576 and 9000")
    o["dns_server"] = str(o["dns_server"]).strip()
    if not o["dns_server"]:
        raise ValueError("DNS server is empty")
    return o


def build(link, opts=None):
    o = checked_options(opts)
    link = (link or "").strip()
    for prefix, fn in PARSERS.items():
        if link.startswith(prefix):
            try:
                proxy = fn(link)
            except (ValueError, KeyError, IndexError, TypeError) as e:
                raise ValueError(f"could not read that link: {e}") from e
            break
    else:
        raise ValueError("unsupported link. Use ss://, vless://, vmess:// or trojan://")

    proxy["tag"] = "proxy"
    domains, cidrs = split_direct_rules(o["direct_rules"])

    rules = [
        {"inbound": ["socks-up"], "outbound": "proxy"},
        {"action": "sniff"},
        {"protocol": "dns", "action": "hijack-dns"},
    ]
    if o["bypass_lan"]:
        rules.append({"ip_is_private": True, "outbound": "direct"})
    if domains:
        rules.append({"domain_suffix": domains, "outbound": "direct"})
    if cidrs:
        rules.append({"ip_cidr": cidrs, "outbound": "direct"})
    if o["block_udp"]:
        rules.append({"network": "udp", "action": "reject"})

    return {
        "log": {"level": o["log_level"], "timestamp": True},
        "dns": {
            "servers": [
                {"type": "https", "tag": "dns-remote", "server": o["dns_server"], "detour": "tor"},
                {"type": "https", "tag": "dns-direct", "server": o["dns_server"]},
            ],
            "final": "dns-remote",
            "strategy": o["dns_strategy"],
        },
        "inbounds": [
            {"type": "tun", "tag": "tun-in", "address": ["172.19.0.1/30"], "mtu": o["mtu"],
             "auto_route": True, "strict_route": o["strict_route"], "stack": o["stack"]},
            {"type": "socks", "tag": "socks-up", "listen": "127.0.0.1",
             "listen_port": UPSTREAM_PORT},
        ],
        "outbounds": [
            proxy,
            {"type": "socks", "tag": "tor", "server": "127.0.0.1",
             "server_port": TOR_SOCKS, "version": "5"},
            {"type": "direct", "tag": "direct"},
        ],
        "route": {
            "auto_detect_interface": True,
            "default_domain_resolver": "dns-direct",
            "final": "tor",
            "rules": rules,
        },
    }


def main():
    try:
        config = build(read_link())
    except ValueError as e:
        sys.exit(str(e))
    with open(CONFIG_FILE, "w", encoding="utf-8", newline="\n") as f:
        json.dump(config, f, indent=2)
    p = config["outbounds"][0]
    print(f"config.json written: {p['type']} -> {p['server']}:{p['server_port']}")


if __name__ == "__main__":
    main()
