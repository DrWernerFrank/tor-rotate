"""TorRotate - one window that runs the whole stack.

  apps -> sing-box TUN -> tor (socks 9050) -> sing-box socks (10810) -> your proxy -> internet

Connect/Disconnect bring up sing-box (system-wide TUN) and tor, rotate exits
through tor's control port and show the current exit. Settings live in
%APPDATA%\\TorRotate\\settings.json.
"""
import copy
import json
import math
import os
import queue
import re
import signal
import socket
import subprocess
import sys
import threading
import time
import tkinter as tk
from datetime import datetime
from tkinter import messagebox, ttk

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tun"))
import make_config  # noqa: E402

FROZEN = getattr(sys, "frozen", False)
BASE = sys._MEIPASS if FROZEN else os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
TOR_EXE = os.path.join(BASE, "tor", "tor", "tor.exe")
LYREBIRD = os.path.join(BASE, "tor", "tor", "pluggable_transports", "lyrebird.exe")
GEOIP = os.path.join(BASE, "tor", "geoip")
SINGBOX = os.path.join(BASE, "tun", "bin", "sing-box.exe")

DATA = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "TorRotate")
TOR_DATA = os.path.join(DATA, "tor-data")
TORRC = os.path.join(DATA, "torrc")
SB_CONFIG = os.path.join(DATA, "config.json")
SB_LOG = os.path.join(DATA, "sing-box.log")
TOR_LOG = os.path.join(DATA, "tor.log")
SETTINGS_FILE = os.path.join(DATA, "settings.json")
LEGACY_LINK = os.path.join(DATA, "proxy.txt")
LEGACY_BRIDGES = os.path.join(DATA, "bridges.txt")

SOCKS_PORT = make_config.TOR_SOCKS
CONTROL_PORT = 9051
UPSTREAM_PORT = make_config.UPSTREAM_PORT
NEWNYM_MIN_GAP = 10
INTERVALS = {"1 min": 60, "3 min": 180, "5 min": 300, "10 min": 600, "30 min": 1800, "1 hour": 3600}
NO_WINDOW = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP

C = {
    "bg": "#0f1116", "card": "#171a22", "field": "#222631", "border": "#2c3240",
    "text": "#e7eaf3", "muted": "#8a93a8", "accent": "#6d8dff", "accent_hi": "#8ea4ff",
    "ok": "#3fb950", "warn": "#e3b341", "bad": "#f85149",
}
FONT = "Segoe UI"
MONO = "Consolas"
STATE_COLORS = {"stopped": C["muted"], "starting": C["warn"], "stopping": C["warn"],
                "connected": C["ok"], "failed": C["bad"]}


def enable_dpi_awareness():
    """Draw at real pixels. Without this Windows bitmap-stretches the window, which looks blurry."""
    if sys.platform != "win32":
        return 96
    import ctypes
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except (AttributeError, OSError):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            pass
    try:
        return ctypes.windll.user32.GetDpiForSystem()
    except (AttributeError, OSError):
        return 96


SCALE = enable_dpi_awareness() / 96


def lerp_color(a, b, t):
    ca = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    cb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#%02x%02x%02x" % tuple(round(x + (y - x) * t) for x, y in zip(ca, cb))


def lerp_num(a, b, t):
    return a + (b - a) * t


def ease(t):
    return 1 - (1 - t) ** 3


DEFAULT_SETTINGS = {
    "link": "",
    "bridges": "",
    "rotation": {"mode": "Manual", "interval": "5 min", "drop_open": True},
    "singbox": {**make_config.DEFAULTS, "direct_rules": ""},
}


def load_settings():
    s = copy.deepcopy(DEFAULT_SETTINGS)
    if os.path.exists(SETTINGS_FILE):
        try:
            with open(SETTINGS_FILE, encoding="utf-8") as f:
                saved = json.load(f)
            for key, value in saved.items():
                if isinstance(value, dict) and isinstance(s.get(key), dict):
                    s[key].update(value)
                else:
                    s[key] = value
        except (OSError, ValueError):
            pass
    else:
        s["link"] = _read_text(LEGACY_LINK)
        s["bridges"] = _read_text(LEGACY_BRIDGES)
    return s


def write_settings(settings):
    os.makedirs(DATA, exist_ok=True)
    with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
        json.dump(settings, f, indent=2)


def _read_text(path):
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8") as f:
        return f.read().strip()


def bridge_lines(text):
    return [l.strip() for l in text.splitlines() if l.strip() and not l.strip().startswith("#")]


def port_busy(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.5):
            return True
    except OSError:
        return False


def torrc_text(bridges):
    lines = [
        f"SocksPort 127.0.0.1:{SOCKS_PORT}",
        f"ControlPort 127.0.0.1:{CONTROL_PORT}",
        "CookieAuthentication 1",
        f"DataDirectory {TOR_DATA}",
        f"Log notice file {TOR_LOG}",
        "SafeLogging 1",
        f"Socks5Proxy 127.0.0.1:{UPSTREAM_PORT}",
        f"GeoIPFile {GEOIP}",
    ]
    if bridges:
        lines += [
            "UseBridges 1",
            f"ClientTransportPlugin obfs4 exec {LYREBIRD}",
            f"ClientTransportPlugin meek_lite exec {LYREBIRD}",
        ]
        lines += ["Bridge " + b for b in bridges]
    return "\n".join(lines) + "\n"


class Control:
    """Minimal tor control-port client (cookie auth)."""

    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", CONTROL_PORT), timeout=10)
        self.rf = self.sock.makefile("rb")
        with open(os.path.join(TOR_DATA, "control_auth_cookie"), "rb") as f:
            cookie = f.read()
        self.cmd("AUTHENTICATE " + cookie.hex())

    def cmd(self, line):
        self.sock.sendall((line + "\r\n").encode())
        out = []
        while True:
            raw = self.rf.readline()
            if not raw:
                raise ConnectionError("tor control connection closed")
            text = raw.decode("utf-8", "replace").rstrip("\r\n")
            code, sep, body = text[:3], text[3:4], text[4:]
            if sep == "+":
                data = []
                while True:
                    line2 = self.rf.readline().decode("utf-8", "replace").rstrip("\r\n")
                    if line2 == ".":
                        break
                    data.append(line2)
                out.append("\n".join(data))
            elif sep == "-":
                out.append(body)
            elif sep == " ":
                if code != "250":
                    raise RuntimeError(body or text)
                if body and body != "OK":
                    out.append(body)
                return "\n".join(out)
            else:
                raise ConnectionError(f"bad control reply: {text}")

    def getinfo(self, key):
        val = self.cmd("GETINFO " + key)
        return val[len(key) + 1:] if val.startswith(key + "=") else val

    def newnym(self):
        self.cmd("SIGNAL NEWNYM")

    def close_streams(self):
        """Close open streams so apps reconnect and pick up a circuit on the new exit."""
        closed = 0
        for line in self.cmd("GETINFO stream-status").splitlines():
            parts = line.split()
            if parts and parts[0].isdigit():
                try:
                    self.cmd(f"CLOSESTREAM {parts[0]} 1")
                    closed += 1
                except RuntimeError:
                    pass
        return closed

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def read_exit(ctl):
    built = [l for l in ctl.getinfo("circuit-status").splitlines()
             if " BUILT " in l and "PURPOSE=GENERAL" in l]
    if not built:
        return None
    hop = built[-1].split()[2].split(",")[-1]
    m = re.match(r"\$([0-9A-Fa-f]{40})[~=]?(\S*)", hop)
    if not m:
        return None
    fp, nick = m.group(1), m.group(2)
    ip, cc = "-", "-"
    for line in ctl.getinfo("ns/id/" + fp).splitlines():
        if line.startswith("r "):
            ip = line.split()[6]
            break
    try:
        cc = ctl.getinfo("ip-to-country/" + ip) or "-"
    except (RuntimeError, ConnectionError, OSError):
        pass
    return {"nick": nick or "(unnamed)", "ip": ip, "cc": cc}


def apply_theme(root):
    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure(".", background=C["bg"], foreground=C["text"], fieldbackground=C["field"],
                    bordercolor=C["border"], lightcolor=C["border"], darkcolor=C["border"],
                    troughcolor=C["field"], font=(FONT, 10))
    style.configure("Card.TFrame", background=C["card"])
    style.configure("Card.TLabel", background=C["card"], foreground=C["text"])
    style.configure("Heading.TLabel", background=C["card"], foreground=C["muted"], font=(FONT, 9, "bold"))
    style.configure("Muted.TLabel", background=C["card"], foreground=C["muted"], font=(FONT, 9))
    style.configure("Big.TLabel", background=C["card"], foreground=C["text"], font=(FONT, 22, "bold"))
    style.configure("Card.TCheckbutton", background=C["card"], foreground=C["text"])
    style.map("Card.TCheckbutton", background=[("active", C["card"])])
    style.configure("TButton", background=C["field"], foreground=C["text"], borderwidth=0,
                    focusthickness=0, padding=(14, 8))
    style.map("TButton", background=[("disabled", C["card"]), ("pressed", C["border"]),
                                     ("active", C["border"])],
              foreground=[("disabled", C["muted"])])
    style.configure("Accent.TButton", background=C["accent"], foreground="#ffffff")
    style.map("Accent.TButton", background=[("disabled", C["card"]), ("active", C["accent_hi"])],
              foreground=[("disabled", C["muted"])])
    for name in ("TEntry", "TSpinbox"):
        style.configure(name, fieldbackground=C["field"], foreground=C["text"],
                        insertcolor=C["text"], arrowcolor=C["text"], borderwidth=0, padding=6)
    style.configure("TCombobox", fieldbackground=C["field"], foreground=C["text"],
                    arrowcolor=C["text"], borderwidth=0, padding=5)
    style.map("TCombobox", fieldbackground=[("readonly", C["field"])],
              foreground=[("readonly", C["text"])], selectbackground=[("readonly", C["field"])],
              selectforeground=[("readonly", C["text"])])
    style.configure("TNotebook", background=C["bg"], borderwidth=0)
    style.configure("TNotebook.Tab", background=C["bg"], foreground=C["muted"],
                    padding=(18, 9), borderwidth=0, bordercolor=C["bg"],
                    lightcolor=C["bg"], darkcolor=C["bg"])
    style.map("TNotebook.Tab", background=[("selected", C["card"])],
              foreground=[("selected", C["text"])])


def card(parent, title):
    frame = ttk.Frame(parent, style="Card.TFrame", padding=(18, 14))
    frame.columnconfigure(1, weight=1)
    ttk.Label(frame, text=title.upper(), style="Heading.TLabel").grid(
        row=0, column=0, columnspan=2, sticky="w", pady=(0, 8))
    return frame


def add_row(frame, label, widget):
    row = frame.grid_size()[1]
    ttk.Label(frame, text=label, style="Card.TLabel").grid(
        row=row, column=0, sticky="w", padx=(0, 16), pady=5)
    widget.grid(row=row, column=1, sticky="ew", pady=5)


def add_check(frame, text, var):
    row = frame.grid_size()[1]
    ttk.Checkbutton(frame, text=text, variable=var, style="Card.TCheckbutton").grid(
        row=row, column=0, columnspan=2, sticky="w", pady=4)


def add_note(frame, text):
    row = frame.grid_size()[1]
    ttk.Label(frame, text=text, style="Muted.TLabel", wraplength=640, justify="left").grid(
        row=row, column=0, columnspan=2, sticky="w", pady=(6, 0))


def text_box(parent, height, mono=False):
    return tk.Text(parent, height=height, bg=C["field"], fg=C["text"], insertbackground=C["text"],
                   relief="flat", padx=8, pady=6, font=(MONO if mono else FONT, 9 if mono else 10),
                   highlightthickness=1, highlightbackground=C["border"],
                   highlightcolor=C["accent"], wrap="none")


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("TorRotate")
        self.geometry(f"{round(940 * SCALE)}x{round(700 * SCALE)}")
        self.minsize(round(880 * SCALE), round(640 * SCALE))
        self.configure(bg=C["bg"])
        self.attributes("-alpha", 0.0)
        apply_theme(self)
        self.style = ttk.Style(self)
        self.option_add("*TCombobox*Listbox.background", C["field"])
        self.option_add("*TCombobox*Listbox.foreground", C["text"])
        self.option_add("*TCombobox*Listbox.selectBackground", C["accent"])

        self.q = queue.Queue()
        self.sb = None
        self.sb_log = None
        self.tor = None
        self.ctl = None
        self.ctl_lock = threading.Lock()
        self.running = False
        self.busy = False
        self.rotate_now = False
        self.rotations = 0
        self.last_rot = 0.0
        self.last_exit_poll = 0.0
        self.rot_mode = "Manual"
        self.rot_interval = 300
        self.drop_open = True
        self.last_newnym = 0.0
        self._anims = {}
        self._btn_color = {}
        self._pill_color = C["muted"]
        self._core_color = C["muted"]
        self._glow_state = "stopped"
        self._glow_phase = 0.0

        self.status_var = tk.StringVar(value="Disconnected")
        self.exit_var = tk.StringVar(value="No exit yet")
        self.exit_meta_var = tk.StringVar(value="Connect to route traffic through Tor")
        self.link_var = tk.StringVar()
        self.mode_var = tk.StringVar()
        self.interval_var = tk.StringVar()
        self.drop_var = tk.BooleanVar()
        self.sb_vars = {
            "log_level": tk.StringVar(), "stack": tk.StringVar(), "mtu": tk.StringVar(),
            "strict_route": tk.BooleanVar(), "bypass_lan": tk.BooleanVar(),
            "block_udp": tk.BooleanVar(), "dns_server": tk.StringVar(),
            "dns_strategy": tk.StringVar(),
        }

        self._build_ui()
        self._apply(load_settings())
        self._animate_buttons(self)
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(150, self._pump)
        self.after(40, self._glow_tick)
        self.fade("alpha", 350, lambda a: self.attributes("-alpha", a), 0.0, 1.0, mix=lerp_num)
        self.log(f"settings folder: {DATA}")
        for path in (TOR_EXE, SINGBOX, LYREBIRD):
            if not os.path.exists(path):
                self.log(f"missing bundled file: {path}")

    # ---- layout --------------------------------------------------------------
    def _build_ui(self):
        header = tk.Frame(self, bg=C["bg"])
        header.pack(fill="x", padx=20, pady=(16, 8))
        self.glow = tk.Canvas(header, width=34, height=34, bg=C["bg"], highlightthickness=0)
        self.glow.pack(side="left", padx=(0, 10))
        self.halo = self.glow.create_oval(2, 2, 32, 32, outline="", fill=C["bg"])
        self.core = self.glow.create_oval(10, 10, 24, 24, outline="", fill=C["muted"])
        tk.Label(header, text="TorRotate", bg=C["bg"], fg=C["text"],
                 font=(FONT, 18, "bold")).pack(side="left")
        self.pill = tk.Label(header, textvariable=self.status_var, bg=C["muted"], fg=C["bg"],
                             font=(FONT, 10, "bold"), padx=14, pady=4)
        self.pill.pack(side="right")

        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=20, pady=(4, 20))
        dash, conn, sbtab, logtab = (ttk.Frame(nb, padding=16) for _ in range(4))
        nb.add(dash, text="Dashboard")
        nb.add(conn, text="Connection")
        nb.add(sbtab, text="sing-box")
        nb.add(logtab, text="Log")
        self._build_dashboard(dash)
        self._build_connection(conn)
        self._build_singbox(sbtab)
        self._build_log(logtab)

    def _build_dashboard(self, parent):
        exit_card = card(parent, "Current exit")
        exit_card.pack(fill="x")
        ttk.Label(exit_card, textvariable=self.exit_var, style="Big.TLabel").grid(
            row=1, column=0, columnspan=2, sticky="w")
        ttk.Label(exit_card, textvariable=self.exit_meta_var, style="Muted.TLabel").grid(
            row=2, column=0, columnspan=2, sticky="w", pady=(2, 0))

        actions = ttk.Frame(parent)
        actions.pack(fill="x", pady=14)
        self.btn_connect = ttk.Button(actions, text="Connect", style="Accent.TButton", command=self.start)
        self.btn_disconnect = ttk.Button(actions, text="Disconnect", command=self.stop)
        self.btn_rotate = ttk.Button(actions, text="Rotate now", command=self.request_rotate)
        self.btn_connect.pack(side="left", padx=(0, 8))
        self.btn_disconnect.pack(side="left", padx=8)
        self.btn_rotate.pack(side="left", padx=8)

        rot = card(parent, "Rotation")
        rot.pack(fill="x")
        mode = ttk.Combobox(rot, textvariable=self.mode_var, values=["Manual", "Timer"],
                            state="readonly", width=16)
        mode.bind("<<ComboboxSelected>>", lambda e: self.on_rotation_change())
        add_row(rot, "Mode", mode)
        iv = ttk.Combobox(rot, textvariable=self.interval_var, values=list(INTERVALS),
                          state="readonly", width=16)
        iv.bind("<<ComboboxSelected>>", lambda e: self.on_rotation_change())
        add_row(rot, "Change exit every (timer mode)", iv)
        add_check(rot, "Close open connections on rotate (apps reconnect on the new exit)", self.drop_var)
        self.drop_var.trace_add("write", lambda *a: self.on_rotation_change(log=False))

        route = card(parent, "Route")
        route.pack(fill="x", pady=(14, 0))
        ttk.Label(route, text="Apps  →  sing-box TUN  →  Tor  →  your proxy  →  internet",
                  style="Card.TLabel", font=(FONT, 11)).grid(row=1, column=0, columnspan=2, sticky="w")
        add_note(route, "Keep TorRotate running. While connected, all system traffic goes through "
                        "Tor; closing the app cuts the network.")

    def _build_connection(self, parent):
        link = card(parent, "Proxy link")
        link.pack(fill="x")
        entry = ttk.Entry(link, textvariable=self.link_var)
        add_row(link, "Link", entry)
        add_note(link, "Supported: ss://  vless://  vmess://  trojan://  (one link). Saved on Connect.")
        ttk.Button(link, text="Check link", command=self.check_link).grid(
            row=link.grid_size()[1], column=1, sticky="w", pady=(10, 0))

        br = card(parent, "Bridges (optional)")
        br.pack(fill="both", expand=True, pady=(14, 0))
        self.bridges_txt = text_box(br, 6, mono=True)
        self.bridges_txt.grid(row=br.grid_size()[1], column=0, columnspan=2, sticky="nsew", pady=4)
        br.rowconfigure(br.grid_size()[1] - 1, weight=1)
        add_note(br, "Only needed when Tor relays are blocked on your network. One bridge per line; "
                     "fresh obfs4 lines: bridges.torproject.org")

    def _build_singbox(self, parent):
        parent.columnconfigure(0, weight=1)
        parent.columnconfigure(1, weight=1)

        tun = card(parent, "TUN interface")
        tun.grid(row=0, column=0, rowspan=2, sticky="nsew", padx=(0, 8))
        stack = ttk.Combobox(tun, textvariable=self.sb_vars["stack"], values=list(make_config.STACKS),
                             state="readonly", width=14)
        add_row(tun, "Network stack", stack)
        mtu = ttk.Spinbox(tun, textvariable=self.sb_vars["mtu"], from_=576, to=9000, increment=100, width=16)
        add_row(tun, "MTU", mtu)
        add_check(tun, "Strict route (block leaks outside the TUN)", self.sb_vars["strict_route"])
        add_check(tun, "Bypass LAN and private addresses", self.sb_vars["bypass_lan"])
        add_check(tun, "Block UDP (Tor can't carry it)", self.sb_vars["block_udp"])
        add_note(tun, "Stack: mixed is the default. Use system or gvisor if some app misbehaves.")

        dns = card(parent, "DNS (over Tor)")
        dns.grid(row=0, column=1, sticky="nsew", padx=(8, 0))
        add_row(dns, "DoH server", ttk.Entry(dns, textvariable=self.sb_vars["dns_server"], width=18))
        strat = ttk.Combobox(dns, textvariable=self.sb_vars["dns_strategy"],
                             values=list(make_config.DNS_STRATEGIES), state="readonly", width=16)
        add_row(dns, "Strategy", strat)
        add_note(dns, "IP of a DNS-over-HTTPS server, e.g. 1.1.1.1, 9.9.9.9 or 8.8.8.8. "
                      "Lookups are sent through Tor.")

        log = card(parent, "sing-box logging")
        log.grid(row=1, column=1, sticky="nsew", padx=(8, 0), pady=(14, 0))
        lvl = ttk.Combobox(log, textvariable=self.sb_vars["log_level"], values=list(make_config.LOG_LEVELS),
                           state="readonly", width=16)
        add_row(log, "Level", lvl)
        add_note(log, "Written to sing-box.log in the settings folder.")

        rules = card(parent, "Direct rules (bypass Tor)")
        rules.grid(row=2, column=0, columnspan=2, sticky="nsew", pady=(14, 0))
        self.direct_txt = text_box(rules, 4, mono=True)
        self.direct_txt.grid(row=rules.grid_size()[1], column=0, columnspan=2, sticky="nsew", pady=4)
        rules.rowconfigure(rules.grid_size()[1] - 1, weight=1)
        add_note(rules, "One domain (example.com) or IP/CIDR (10.0.0.0/8) per line. Matching traffic "
                        "goes out directly, NOT through Tor, so your real IP is visible to those sites.")

        bar = ttk.Frame(parent)
        bar.grid(row=3, column=0, columnspan=2, sticky="w", pady=(14, 0))
        ttk.Button(bar, text="Save", style="Accent.TButton", command=self.save).pack(side="left", padx=(0, 8))
        ttk.Button(bar, text="Check config", command=self.check_config).pack(side="left", padx=8)
        ttk.Button(bar, text="Reset to defaults", command=self.reset_singbox).pack(side="left", padx=8)
        ttk.Label(bar, text="Changes apply on the next Connect.", style="Muted.TLabel").pack(side="left", padx=12)

    def _build_log(self, parent):
        self.log_txt = text_box(parent, 20, mono=True)
        self.log_txt.configure(state="disabled")
        self.log_txt.pack(fill="both", expand=True)
        bar = ttk.Frame(parent)
        bar.pack(fill="x", pady=(10, 0))
        ttk.Button(bar, text="Clear", command=self._clear_log).pack(side="left")
        ttk.Button(bar, text="Open settings folder", command=self._open_folder).pack(side="left", padx=8)

    # ---- settings <-> widgets ----------------------------------------------
    def _apply(self, s):
        self.link_var.set(s["link"])
        self.bridges_txt.delete("1.0", "end")
        self.bridges_txt.insert("1.0", s["bridges"])
        self.mode_var.set(s["rotation"]["mode"])
        self.interval_var.set(s["rotation"]["interval"])
        self.drop_var.set(s["rotation"]["drop_open"])
        self.on_rotation_change(log=False)
        sb = s["singbox"]
        for key, var in self.sb_vars.items():
            var.set(sb[key])
        self.direct_txt.delete("1.0", "end")
        self.direct_txt.insert("1.0", sb["direct_rules"])

    def snapshot(self):
        return {
            "link": self.link_var.get().strip(),
            "bridges": self.bridges_txt.get("1.0", "end").strip(),
            "rotation": {"mode": self.mode_var.get(), "interval": self.interval_var.get(),
                         "drop_open": self.drop_var.get()},
            "singbox": {
                **{k: v.get() for k, v in self.sb_vars.items()},
                "direct_rules": self.direct_txt.get("1.0", "end").strip(),
            },
        }

    def persist(self):
        write_settings(self.snapshot())

    def save(self):
        self.persist()
        self.log("settings saved")

    def reset_singbox(self):
        for key, var in self.sb_vars.items():
            var.set(make_config.DEFAULTS[key])
        self.direct_txt.delete("1.0", "end")

    def check_link(self):
        try:
            cfg = make_config.build(self.link_var.get(), self.snapshot()["singbox"])
        except ValueError as e:
            messagebox.showerror("Proxy link", str(e))
            return
        p = cfg["outbounds"][0]
        messagebox.showinfo("Proxy link", f"Looks good: {p['type']} → {p['server']}:{p['server_port']}")

    def check_config(self):
        snap = self.snapshot()
        try:
            cfg = make_config.build(snap["link"], snap["singbox"])
        except ValueError as e:
            messagebox.showerror("sing-box settings", str(e))
            return
        if not os.path.exists(SINGBOX):
            messagebox.showerror("sing-box", f"sing-box.exe is missing:\n{SINGBOX}")
            return
        os.makedirs(DATA, exist_ok=True)
        with open(SB_CONFIG, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
        r = subprocess.run([SINGBOX, "check", "-c", SB_CONFIG], capture_output=True, text=True,
                           creationflags=NO_WINDOW, cwd=DATA, timeout=30)
        if r.returncode == 0:
            messagebox.showinfo("sing-box", "Config is valid.")
        else:
            messagebox.showerror("sing-box", (r.stderr or r.stdout).strip()[-800:] or "Config rejected.")

    # ---- animation ---------------------------------------------------------
    def fade(self, key, ms, apply, a, b, mix=lerp_color):
        if key in self._anims:
            self.after_cancel(self._anims.pop(key))
        steps = max(1, ms // 16)

        def step(i):
            apply(mix(a, b, ease(min(i / steps, 1.0))))
            if i < steps:
                self._anims[key] = self.after(16, step, i + 1)
            else:
                self._anims.pop(key, None)
        step(1)

    def flash_exit(self):
        self.fade("exit", 600, lambda c: self.style.configure("Big.TLabel", foreground=c),
                  C["accent_hi"], C["text"])

    def _set_state_visual(self, name, text):
        self.status_var.set(text)
        self._glow_state = name
        self.fade("pill", 300, self._paint_pill, self._pill_color, STATE_COLORS[name])
        self.fade("core", 300, self._paint_core, self._core_color, STATE_COLORS[name])

    def _paint_pill(self, c):
        self._pill_color = c
        self.pill.configure(bg=c)

    def _paint_core(self, c):
        self._core_color = c
        self.glow.itemconfigure(self.core, fill=c)

    def _glow_tick(self):
        self._glow_phase += 0.04
        st = self._glow_state
        busy = st in ("starting", "stopping")
        if busy:
            amp = 0.5 + 0.5 * math.sin(self._glow_phase * 6)
        elif st == "connected":
            amp = 0.25 + 0.2 * math.sin(self._glow_phase * 1.5)
        else:
            amp = 0.0
        r = 7 + (1.5 * amp if busy else 0.0)
        self.glow.itemconfigure(self.halo, fill=lerp_color(C["bg"], self._core_color, 0.45 * amp))
        self.glow.coords(self.core, 17 - r, 17 - r, 17 + r, 17 + r)
        self.after(40, self._glow_tick)

    def _animate_buttons(self, root):
        hover = {"TButton": (C["field"], "#2f3649"), "Accent.TButton": (C["accent"], C["accent_hi"])}
        for w in root.winfo_children():
            if isinstance(w, ttk.Button):
                base_style = str(w.cget("style")) or "TButton"
                if base_style in hover:
                    self._hover_button(w, base_style, *hover[base_style])
            self._animate_buttons(w)

    def _hover_button(self, btn, base_style, base, hi):
        name = f"Anim{id(btn)}.{base_style}"
        self.style.configure(name, background=base)
        self.style.map(name, background=[("disabled", C["card"])],
                       foreground=[("disabled", C["muted"])])
        btn.configure(style=name)
        self._btn_color[name] = base

        def go(target):
            if not btn.instate(["disabled"]):
                self.fade(name, 180, lambda c: self._paint_btn(name, c), self._btn_color[name], target)
        btn.bind("<Enter>", lambda e: go(hi), add="+")
        btn.bind("<Leave>", lambda e: go(base), add="+")

    def _paint_btn(self, name, c):
        self._btn_color[name] = c
        self.style.configure(name, background=c)

    # ---- logging & thread-safe UI updates ---------------------------------
    def log(self, text):
        self.q.put(("log", f"[{datetime.now():%H:%M:%S}] {text}"))

    def _set_state(self, name, text):
        self.q.put(("state", (name, text)))

    def set_exit(self, info):
        self.q.put(("exit", info))

    def _clear_log(self):
        self.log_txt.configure(state="normal")
        self.log_txt.delete("1.0", "end")
        self.log_txt.configure(state="disabled")

    def _open_folder(self):
        os.makedirs(DATA, exist_ok=True)
        os.startfile(DATA)

    def _pump(self):
        try:
            while True:
                kind, val = self.q.get_nowait()
                if kind == "state":
                    name, text = val
                    self._set_state_visual(name, text)
                elif kind == "log":
                    self.log_txt.configure(state="normal")
                    self.log_txt.insert("end", val + "\n")
                    self.log_txt.see("end")
                    self.log_txt.configure(state="disabled")
                elif kind == "exit":
                    self._show_exit(val)
                elif kind == "error":
                    messagebox.showerror("TorRotate", val)
        except queue.Empty:
            pass
        self._refresh_buttons()
        self.after(150, self._pump)

    def _show_exit(self, info):
        if info is None:
            if self.running:
                text, meta = "Building circuit…", f"rotations {self.rotations}"
            else:
                text, meta = "No exit yet", "Connect to route traffic through Tor"
        else:
            text = f"{info['cc']}  ·  {info['nick']}"
            meta = f"IP {info['ip']}     rotations {self.rotations}"
        self.exit_meta_var.set(meta)
        if text != self.exit_var.get():
            self.exit_var.set(text)
            self.flash_exit()

    def _refresh_buttons(self):
        active = any(x is not None for x in (self.sb, self.tor))
        self.btn_connect.state(["disabled"] if (self.busy or self.running) else ["!disabled"])
        self.btn_disconnect.state(["!disabled"] if (active and not self.busy) else ["disabled"])
        self.btn_rotate.state(["!disabled"] if (self.running and not self.busy) else ["disabled"])

    # ---- rotation ----------------------------------------------------------
    def on_rotation_change(self, log=True):
        self.rot_mode = self.mode_var.get()
        self.rot_interval = INTERVALS.get(self.interval_var.get(), 300)
        self.drop_open = self.drop_var.get()
        if log:
            self.log(f"rotation: {self.rot_mode}" +
                     (f" every {self.interval_var.get()}" if self.rot_mode == "Timer" else ""))

    def request_rotate(self):
        self.rotate_now = True
        self.flash_exit()

    # ---- start / stop ------------------------------------------------------
    def _set_busy(self, flag):
        self.busy = flag

    def start(self):
        if self.busy or self.running:
            return
        missing = [p for p in (TOR_EXE, SINGBOX) if not os.path.exists(p)]
        if missing:
            messagebox.showerror("Missing files", "These bundled files are missing:\n" + "\n".join(missing))
            return
        snap = self.snapshot()
        try:
            cfg = make_config.build(snap["link"], snap["singbox"])
        except ValueError as e:
            messagebox.showerror("Settings", str(e))
            return
        for port, name in ((SOCKS_PORT, "tor socks"), (CONTROL_PORT, "tor control"),
                           (UPSTREAM_PORT, "sing-box local")):
            if port_busy(port):
                messagebox.showerror(
                    "Port in use",
                    f"Port {port} ({name}) is already in use.\n"
                    "Close any old tor.exe or sing-box.exe (Task Manager) and try again.")
                return
        self.persist()
        self._set_busy(True)
        self._set_state("starting", "Starting…")
        threading.Thread(target=self._start_worker, args=(cfg, bridge_lines(snap["bridges"])),
                         daemon=True).start()

    def _start_worker(self, cfg, bridges):
        try:
            self._set_state("starting", "Starting sing-box…")
            os.makedirs(DATA, exist_ok=True)
            with open(SB_CONFIG, "w", encoding="utf-8") as f:
                json.dump(cfg, f, indent=2)
            chk = subprocess.run([SINGBOX, "check", "-c", SB_CONFIG], capture_output=True,
                                 text=True, creationflags=NO_WINDOW, cwd=DATA)
            if chk.returncode != 0:
                raise RuntimeError("sing-box rejected the config:\n" + (chk.stderr or chk.stdout).strip()[-600:])

            self.sb_log = open(SB_LOG, "w", encoding="utf-8")
            self.sb = subprocess.Popen([SINGBOX, "run", "-c", SB_CONFIG], cwd=DATA,
                                       stdout=self.sb_log, stderr=subprocess.STDOUT,
                                       creationflags=NO_WINDOW)
            time.sleep(3)
            if self.sb.poll() is not None:
                raise RuntimeError(f"sing-box exited early, see {SB_LOG}")
            self.log("sing-box TUN is up")

            with open(TORRC, "w", encoding="utf-8") as f:
                f.write(torrc_text(bridges))
            self._set_state("starting", "Starting Tor…")
            self.tor = subprocess.Popen([TOR_EXE, "-f", TORRC], cwd=DATA,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                        creationflags=NO_WINDOW)
            ctl = self._wait_for_bootstrap()
            with self.ctl_lock:
                self.ctl = ctl
            self.rotations = 0
            self.last_rot = time.time()
            self.running = True
            self._set_state("connected", "Connected")
            self.log("Tor is up. Everything is routed through Tor.")
            threading.Thread(target=self._monitor, daemon=True).start()
        except Exception as e:
            self.log(f"start failed: {e}")
            self._set_state("failed", "Failed")
            self.q.put(("error", str(e)))
            self._teardown()
        finally:
            self._set_busy(False)

    def _wait_for_bootstrap(self):
        ctl = None
        deadline = time.time() + 240
        while time.time() < deadline:
            if self.tor.poll() is not None:
                raise RuntimeError(f"tor exited, see {TOR_LOG}")
            try:
                if ctl is None:
                    ctl = Control()
                m = re.search(r"PROGRESS=(\d+)", ctl.getinfo("status/bootstrap-phase"))
                pct = int(m.group(1)) if m else 0
                self._set_state("starting", f"Connecting to Tor… {pct}%")
                if pct >= 100:
                    return ctl
            except (OSError, RuntimeError, ConnectionError):
                if ctl:
                    ctl.close()
                ctl = None
            time.sleep(1.5)
        if ctl:
            ctl.close()
        raise RuntimeError(f"Tor did not finish bootstrapping in 4 minutes, see {TOR_LOG}")

    def _monitor(self):
        while self.running:
            now = time.time()
            try:
                with self.ctl_lock:
                    if self.ctl is None:
                        self.ctl = Control()
                    due = self.rot_mode == "Timer" and now - self.last_rot >= self.rot_interval
                    # Tor ignores NEWNYM sent less than 10 s after the previous one.
                    if (self.rotate_now or due) and now - self.last_newnym >= NEWNYM_MIN_GAP:
                        self.rotate_now = False
                        self.ctl.newnym()
                        self.last_newnym = now
                        self.rotations += 1
                        self.last_rot = now
                        closed = self.ctl.close_streams() if self.drop_open else 0
                        self.log(f"exit rotated (#{self.rotations}), closed {closed} open connections")
                    if now - self.last_exit_poll >= 3:
                        self.last_exit_poll = now
                        self.set_exit(read_exit(self.ctl))
            except (OSError, RuntimeError, ConnectionError) as e:
                if not self.running:
                    break
                self.log(f"control error, reconnecting: {e}")
                with self.ctl_lock:
                    self._drop_ctl()
                time.sleep(3)
                continue
            time.sleep(0.5)

    def stop(self):
        if self.busy or not (self.running or any(x is not None for x in (self.sb, self.tor))):
            return
        self._set_busy(True)
        threading.Thread(target=self._stop_worker, daemon=True).start()

    def _stop_worker(self):
        try:
            self._set_state("stopping", "Disconnecting…")
            self._teardown()
            self._set_state("stopped", "Disconnected")
            self.set_exit(None)
            self.log("stopped; network is back to normal")
        finally:
            self._set_busy(False)

    def _drop_ctl(self, shutdown=False):
        if self.ctl:
            if shutdown:
                try:
                    self.ctl.cmd("SIGNAL SHUTDOWN")
                except (OSError, RuntimeError, ConnectionError):
                    pass
            self.ctl.close()
            self.ctl = None

    def _teardown(self):
        self.running = False
        time.sleep(1)
        with self.ctl_lock:
            self._drop_ctl(shutdown=True)
        if self.tor:
            try:
                self.tor.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.tor.kill()
            self.tor = None
        if self.sb:
            # CTRL_BREAK lets sing-box remove its TUN adapter and routes cleanly.
            try:
                self.sb.send_signal(signal.CTRL_BREAK_EVENT)
                self.sb.wait(timeout=10)
            except (subprocess.TimeoutExpired, OSError, ValueError):
                self.log("sing-box did not stop cleanly; killing it (check your routes)")
                self.sb.kill()
            self.sb = None
        if self.sb_log:
            self.sb_log.close()
            self.sb_log = None

    def on_close(self):
        if self.busy:
            return
        if self.running or self.sb or self.tor:
            self._set_busy(True)
            self._teardown()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
