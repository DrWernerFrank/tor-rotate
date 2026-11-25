"""TorRotate - one window that runs the whole stack.

  apps -> sing-box TUN -> tor (socks 9050) -> sing-box socks (10810) -> your proxy -> internet

Start/Stop bring up sing-box (system-wide TUN) and tor, rotate exits through
tor's control port, and show the current exit. Settings live in %APPDATA%\\TorRotate.
"""
import json
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
import make_config  # noqa: E402  (link -> sing-box config)

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
LINK_FILE = os.path.join(DATA, "proxy.txt")
BRIDGE_FILE = os.path.join(DATA, "bridges.txt")

SOCKS_PORT = make_config.TOR_SOCKS
CONTROL_PORT = 9051
UPSTREAM_PORT = make_config.UPSTREAM_PORT
INTERVALS = [60, 180, 300, 600, 1800, 3600]
NO_WINDOW = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP


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


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("TorRotate")
        self.geometry("720x680")
        self.minsize(640, 600)
        self.q = queue.Queue()
        self.sb = None
        self.sb_log = None
        self.tor = None
        self.ctl = None
        self.running = False
        self.busy = False
        self.rotate_now = False
        self.rot_mode = "Manual"
        self.rot_interval = 300
        self.rotations = 0
        self.last_rot = 0.0
        self.last_exit_poll = 0.0
        self.status_var = tk.StringVar(value="Stopped")
        self.exit_var = tk.StringVar(value="Exit: -")
        self.mode_var = tk.StringVar(value="Manual")
        self.interval_var = tk.StringVar(value="300")
        self.link_var = tk.StringVar()
        self._build_ui()
        self._load_settings()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(150, self._pump)
        self.log(f"settings folder: {DATA}")
        for path in (TOR_EXE, SINGBOX, LYREBIRD):
            if not os.path.exists(path):
                self.log(f"ERROR: missing bundled file: {path}")

    def _build_ui(self):
        pad = {"padx": 10, "pady": 4}
        top = ttk.Frame(self)
        top.pack(fill="x", **pad)
        tk.Label(top, textvariable=self.status_var, font=("Segoe UI", 16, "bold")).pack(anchor="w")
        tk.Label(top, textvariable=self.exit_var, font=("Segoe UI", 11)).pack(anchor="w")

        btns = ttk.Frame(self)
        btns.pack(fill="x", **pad)
        self.btn_start = ttk.Button(btns, text="Start", command=self.start)
        self.btn_stop = ttk.Button(btns, text="Stop", command=self.stop)
        self.btn_rot = ttk.Button(btns, text="Rotate now", command=self.request_rotate)
        self.btn_start.pack(side="left", padx=(0, 6))
        self.btn_stop.pack(side="left", padx=6)
        self.btn_rot.pack(side="left", padx=6)

        rot = ttk.LabelFrame(self, text="Rotation")
        rot.pack(fill="x", **pad)
        ttk.Label(rot, text="Mode").pack(side="left", padx=6)
        mode = ttk.Combobox(rot, textvariable=self.mode_var, values=["Manual", "Timer"],
                            state="readonly", width=10)
        mode.pack(side="left")
        mode.bind("<<ComboboxSelected>>", lambda e: self.on_rotation_change())
        ttk.Label(rot, text="every").pack(side="left", padx=6)
        iv = ttk.Combobox(rot, textvariable=self.interval_var,
                          values=[str(i) for i in INTERVALS], state="readonly", width=8)
        iv.pack(side="left")
        iv.bind("<<ComboboxSelected>>", lambda e: self.on_rotation_change())
        ttk.Label(rot, text="seconds").pack(side="left", padx=4)

        link = ttk.LabelFrame(self, text="Proxy link (ss:// vless:// vmess:// trojan://)")
        link.pack(fill="x", **pad)
        ttk.Entry(link, textvariable=self.link_var).pack(side="left", fill="x", expand=True,
                                                         padx=6, pady=6)
        ttk.Button(link, text="Save", command=self.save_settings).pack(side="left", padx=6)

        br = ttk.LabelFrame(self, text="Bridges (optional, one per line)")
        br.pack(fill="x", **pad)
        self.bridges_txt = tk.Text(br, height=4)
        self.bridges_txt.pack(fill="x", padx=6, pady=6)

        lg = ttk.LabelFrame(self, text="Log")
        lg.pack(fill="both", expand=True, **pad)
        self.log_txt = tk.Text(lg, height=12, state="disabled", font=("Consolas", 9))
        self.log_txt.pack(fill="both", expand=True, padx=6, pady=6)

    def _load_settings(self):
        os.makedirs(TOR_DATA, exist_ok=True)
        if os.path.exists(LINK_FILE):
            with open(LINK_FILE, encoding="utf-8") as f:
                self.link_var.set(f.read().strip())
        if os.path.exists(BRIDGE_FILE):
            with open(BRIDGE_FILE, encoding="utf-8") as f:
                self.bridges_txt.insert("1.0", f.read())

    def bridges(self):
        return [l.strip() for l in self.bridges_txt.get("1.0", "end").splitlines()
                if l.strip() and not l.strip().startswith("#")]

    # ---- thread-safe UI updates -------------------------------------------
    def log(self, text):
        self.q.put(("log", f"[{datetime.now():%H:%M:%S}] {text}"))

    def set_status(self, text):
        self.q.put(("status", text))

    def set_exit(self, info):
        self.q.put(("exit", info))

    def _pump(self):
        try:
            while True:
                kind, val = self.q.get_nowait()
                if kind == "status":
                    self.status_var.set(val)
                elif kind == "log":
                    self.log_txt.configure(state="normal")
                    self.log_txt.insert("end", val + "\n")
                    self.log_txt.see("end")
                    self.log_txt.configure(state="disabled")
                elif kind == "exit":
                    self._show_exit(val)
        except queue.Empty:
            pass
        self._refresh_buttons()
        self.after(150, self._pump)

    def _show_exit(self, info):
        if info is None:
            self.exit_var.set("Exit: (building circuit)" if self.running else "Exit: -")
            return
        self.exit_var.set(f"Exit: {info['nick']}   IP {info['ip']}   {info['cc']}"
                          f"   rotations {self.rotations}")

    def _refresh_buttons(self):
        self.btn_start.state(["disabled"] if (self.busy or self.running) else ["!disabled"])
        self.btn_stop.state(["disabled"] if (self.busy or not self.running) else ["!disabled"])
        self.btn_rot.state(["disabled"] if (self.busy or not self.running) else ["!disabled"])

    # ---- settings ----------------------------------------------------------
    def on_rotation_change(self):
        self.rot_mode = self.mode_var.get()
        self.rot_interval = int(self.interval_var.get())
        self.log(f"rotation: {self.rot_mode}" +
                 (f" every {self.rot_interval}s" if self.rot_mode == "Timer" else ""))

    def save_settings(self):
        link = self.link_var.get().strip()
        try:
            make_config.build(link)
        except (SystemExit, Exception) as e:
            messagebox.showerror("Proxy link", f"That link is not valid:\n{e}")
            return False
        os.makedirs(TOR_DATA, exist_ok=True)
        with open(LINK_FILE, "w", encoding="utf-8") as f:
            f.write(link + "\n")
        with open(BRIDGE_FILE, "w", encoding="utf-8") as f:
            f.write("\n".join(self.bridges()) + "\n")
        self.log("settings saved")
        return True

    def request_rotate(self):
        self.rotate_now = True

    # ---- start / stop ------------------------------------------------------
    def _set_busy(self, flag):
        self.busy = flag

    def start(self):
        if self.busy or self.running:
            return
        if not self.save_settings():
            return
        for port, name in ((SOCKS_PORT, "tor socks"), (CONTROL_PORT, "tor control"),
                           (UPSTREAM_PORT, "sing-box local")):
            if port_busy(port):
                messagebox.showerror(
                    "Port in use",
                    f"Port {port} ({name}) is already in use.\n"
                    "Close any old tor.exe or sing-box.exe (Task Manager) and try again.")
                return
        self._set_busy(True)
        threading.Thread(target=self._start_worker, daemon=True).start()

    def _start_worker(self):
        try:
            self.set_status("Starting sing-box...")
            with open(SB_CONFIG, "w", encoding="utf-8") as f:
                json.dump(make_config.build(self.link_var.get().strip()), f, indent=2)
            chk = subprocess.run([SINGBOX, "check", "-c", SB_CONFIG], capture_output=True,
                                 text=True, creationflags=NO_WINDOW, cwd=DATA)
            if chk.returncode != 0:
                raise RuntimeError("sing-box config invalid: " + (chk.stderr or chk.stdout)[-300:])

            self.sb_log = open(SB_LOG, "w", encoding="utf-8")
            self.sb = subprocess.Popen([SINGBOX, "run", "-c", SB_CONFIG], cwd=DATA,
                                       stdout=self.sb_log, stderr=subprocess.STDOUT,
                                       creationflags=NO_WINDOW)
            time.sleep(3)
            if self.sb.poll() is not None:
                raise RuntimeError(f"sing-box exited, see {SB_LOG}")
            self.log("sing-box TUN is up")

            with open(TORRC, "w", encoding="utf-8") as f:
                f.write(torrc_text(self.bridges()))
            self.set_status("Starting Tor...")
            self.tor = subprocess.Popen([TOR_EXE, "-f", TORRC], cwd=DATA,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                        creationflags=NO_WINDOW)

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
                    self.set_status(f"Connecting to Tor... {pct}%")
                    if pct >= 100:
                        break
                except (OSError, RuntimeError, ConnectionError):
                    if ctl:
                        ctl.close()
                    ctl = None
                time.sleep(1.5)
            else:
                raise RuntimeError(f"Tor did not finish bootstrapping in 4 minutes, see {TOR_LOG}")

            self.ctl = ctl
            self.rotations = 0
            self.last_rot = time.time()
            self.running = True
            self.set_status("Connected")
            self.log("Tor is up. Everything is routed through Tor.")
            threading.Thread(target=self._monitor, daemon=True).start()
        except Exception as e:
            self.log(f"start failed: {e}")
            self.set_status("Failed")
            self._teardown()
        finally:
            self._set_busy(False)

    def _monitor(self):
        while self.running:
            try:
                now = time.time()
                timer_due = self.rot_mode == "Timer" and now - self.last_rot >= self.rot_interval
                if self.rotate_now or timer_due:
                    self.rotate_now = False
                    self.ctl.newnym()
                    self.rotations += 1
                    self.last_rot = now
                    self.log(f"exit rotated (#{self.rotations})")
                if now - self.last_exit_poll >= 3:
                    self.last_exit_poll = now
                    self.set_exit(read_exit(self.ctl))
            except (OSError, RuntimeError, ConnectionError, AttributeError) as e:
                if self.running:
                    self.log(f"control error: {e}")
                time.sleep(3)
            time.sleep(0.5)

    def stop(self):
        if self.busy or not (self.running or self.sb or self.tor):
            return
        self._set_busy(True)
        threading.Thread(target=self._stop_worker, daemon=True).start()

    def _stop_worker(self):
        try:
            self.set_status("Stopping...")
            self._teardown()
            self.set_status("Stopped")
            self.set_exit(None)
            self.log("stopped; network is back to normal")
        finally:
            self._set_busy(False)

    def _teardown(self):
        self.running = False
        time.sleep(1)
        if self.ctl:
            try:
                self.ctl.cmd("SIGNAL SHUTDOWN")
            except (OSError, RuntimeError, ConnectionError):
                pass
            self.ctl.close()
            self.ctl = None
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
