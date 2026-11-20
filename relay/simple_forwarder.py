# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Xerx. Part of QuestVisor; see LICENSE.
"""Low-latency relay: goggles 192.168.60.2:9100 -> every TCP client of 0.0.0.0:9201.

What it does, and why:
  * Bytes are forwarded the moment they arrive. The previous version only released a NAL once the *next*
    start code had arrived, so the AUD the goggles send right after each frame was held back until the
    following frame started (~one frame interval). The Quest parser closes a frame on that AUD, so the
    whole frame was delivered ~33 ms late.
  * The stream is scanned incrementally (only the last 2 bytes are carried between recv()s), so the cost
    per chunk is one bytes.find() pass instead of a regex over a growing buffer.
  * Latest parameter sets (H.265 VPS/SPS/PPS, H.264 SPS/PPS) are cached; a new client is switched on at the
    next AUD and gets the parameter sets first.
  * Each client has its own sender thread and queue, so a slow Wi-Fi client never stalls the goggles read
    or other clients. If a client falls behind (--max-backlog-ms / --max-backlog-kb) its queue is dropped
    and it is resynced at the next AUD - for FPV, skipping is better than falling further behind.
  * Before the first slice of every frame a small SEI is inserted carrying the PC arrival time of that
    slice (the QRLY-TS v1 format the app already parses), and a UDP clock server answers on port 9200.
    With both, the app's "Relay->displayed" stat is the true PC-arrival -> on-screen latency.
    --no-sei turns the SEI off; it is never handed to the decoder by the app either way.

  * The goggles are found by themselves (192.168.60.2 or .1, port 9100); the live video is also shown on the PC
    in a window (relay_preview.py, its own process; --no-window turns it off).

Usage:  python simple_forwarder.py [--goggles HOST[:PORT]] [--listen 9201] [--clock-port 9200] [--no-window]
        (packaged as QuestVisor-Relay.exe by build_relay_exe.ps1; double-clicking it uses these defaults)
"""
import argparse, collections, socket, struct, sys, threading, time

__version__ = "0.1.0"
APP = "QuestVisor Relay"

UUID = bytes([0x51, 0x52, 0x4C, 0x59, 0x2D, 0x54, 0x53, 0x2D, 0x76, 0x31, 0, 0, 0, 0, 0, 0x01])
SC4 = b"\x00\x00\x00\x01"


def now_us():
    return time.perf_counter_ns() // 1000          # QPC on Windows, same clock as the UDP clock server


def epb(rbsp):
    """Insert emulation-prevention bytes."""
    out, zeros = bytearray(), 0
    for b in rbsp:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def timestamp_sei_body(codec, t_us, counter):
    """NAL body (no start code) of a user_data_unregistered SEI: uuid + BE64 time + BE32 counter."""
    rbsp = bytes([5, 28]) + UUID + struct.pack(">QI", t_us & 0xFFFFFFFFFFFFFFFF, counter & 0xFFFFFFFF) + b"\x80"
    hdr = b"\x4e\x01" if codec == "h265" else b"\x06"
    return hdr + epb(rbsp)


class Scanner:
    """Incremental Annex-B scanner. feed() returns (bytes to forward, AUD header offsets in those bytes)."""

    MAX_PARAM = 64 * 1024

    def __init__(self, sei=True):
        self.sei = sei
        self.codec = None
        self.params = {}                  # nal type -> start code + body
        self.reset()
        self.frames = 0
        self.sei_count = 0

    def reset(self):
        self.tail = b""
        self.hdr_pending = False
        self.collect = None               # bytearray of a parameter set being collected
        self.collect_type = None
        self.need_sei = False

    def params_ready(self):
        need = (32, 33, 34) if self.codec == "h265" else (7, 8)
        return self.codec is not None and all(t in self.params for t in need)

    def param_blob(self):
        return b"".join(self.params[t] for t in sorted(self.params))

    def _nal_type(self, b):
        # The goggles switch codec on the fly (D-Log M = H.264, normal modes = H.265). Follow on an unambiguous
        # parameter-set header of the other codec: an H.264 SPS byte (0x27/0x47/0x67) is odd, which in H.265 would
        # mean nuh_layer_id >= 32; 0x40 (H.265 VPS) is NAL type 0 in H.264, never valid there.
        new = None
        if self.codec == "h265" and b in (0x27, 0x47, 0x67):
            new = "h264"
        elif self.codec == "h264" and b == 0x40:
            new = "h265"
        if new:
            print("codec changed %s -> %s" % (self.codec, new), flush=True)
            self.codec, self.params, self.collect = new, {}, None
        if self.codec is None:
            if (b >> 1) & 0x3F in (32, 33, 34, 35):
                self.codec = "h265"
            elif b & 0x1F in (7, 8, 9):
                self.codec = "h264"
            else:
                return None
        return ((b >> 1) & 0x3F) if self.codec == "h265" else (b & 0x1F)

    def _finish_collect(self):
        if self.collect is not None:
            body = bytes(self.collect).rstrip(b"\x00")
            if body:
                self.params[self.collect_type] = SC4 + body
            self.collect = None

    def _header(self, chunk, h, events):
        t = self._nal_type(chunk[h])
        if t is None:
            return h
        if (self.codec == "h265" and t in (32, 33, 34)) or (self.codec == "h264" and t in (7, 8)):
            self.collect, self.collect_type = bytearray(), t
            return h                                         # collection starts at the header byte
        if (self.codec == "h265" and t == 35) or (self.codec == "h264" and t == 9):
            events.append((h, "aud"))
            self.need_sei = True
            self.frames += 1
        elif self.need_sei and ((self.codec == "h265" and t <= 31) or (self.codec == "h264" and 1 <= t <= 5)):
            self.need_sei = False
            if self.sei:
                events.append((h, "sei"))
        return None

    def feed(self, chunk, t_us):
        events, collect_from = [], 0
        if self.hdr_pending and chunk:
            self.hdr_pending = False
            cf = self._header(chunk, 0, events)
            if cf is not None:
                collect_from = cf
        tl = len(self.tail)
        data = self.tail + chunk
        i = data.find(b"\x00\x00\x01")
        while i >= 0:
            sc = i - tl                                     # start code position in chunk (may be < 0)
            if self.collect is not None:
                self.collect += chunk[collect_from:max(sc, collect_from)]
                self._finish_collect()
            h = i + 3 - tl
            if h >= len(chunk):
                self.hdr_pending = True
                break
            cf = self._header(chunk, h, events)
            if cf is not None:
                collect_from = cf
            i = data.find(b"\x00\x00\x01", i + 3)
        if self.collect is not None:
            self.collect += chunk[collect_from:]
            if len(self.collect) > self.MAX_PARAM:
                self.collect = None
        self.tail = data[-2:]

        if not events:
            return chunk, []
        out, auds, last = bytearray(), [], 0
        for h, kind in events:
            out += chunk[last:h]
            last = h
            if kind == "aud":
                auds.append(len(out))
            else:
                out += timestamp_sei_body(self.codec, t_us, self.sei_count) + SC4
                self.sei_count += 1
        out += chunk[last:]
        return bytes(out), auds


class Client:
    def __init__(self, sock, addr, max_age_us, max_bytes, name=None):
        self.sock, self.addr = sock, addr
        self.name = name or "Quest %s:%d" % addr
        self.active = False
        self.q = collections.deque()          # (enqueue_us, bytes)
        self.qbytes = 0
        self.cv = threading.Condition()
        self.alive = True
        self.resyncs = 0
        self.sent = 0
        self.max_age_us, self.max_bytes = max_age_us, max_bytes
        threading.Thread(target=self._run, daemon=True).start()

    def enqueue(self, data, t_us):
        with self.cv:
            if self.q and (t_us - self.q[0][0] > self.max_age_us or self.qbytes > self.max_bytes):
                self.q.clear()
                self.qbytes = 0
                self.active = False           # caller resyncs at the next AUD with parameter sets
                self.resyncs += 1
                if self.resyncs in (1, 10, 100) or self.resyncs % 1000 == 0:
                    print("%s fell behind - resync #%d" % (self.name, self.resyncs), flush=True)
                return False
            self.q.append((t_us, data))
            self.qbytes += len(data)
            self.cv.notify()
            return True

    def backlog_ms(self):
        with self.cv:
            return (now_us() - self.q[0][0]) / 1000 if self.q else 0.0

    def _run(self):
        try:
            while self.alive:
                with self.cv:
                    while not self.q and self.alive:
                        self.cv.wait(0.5)
                    if not self.alive:
                        break
                    _, data = self.q.popleft()
                    self.qbytes -= len(data)
                self.sock.sendall(data)
                self.sent += len(data)
        except OSError:
            pass
        self.alive = False
        try:
            self.sock.close()
        except OSError:
            pass
        print("%s disconnected" % self.name, flush=True)


class _PipeSock:
    """Lets a Client write to the video window's pipe as if it were a socket."""

    def __init__(self, conn):
        self.conn = conn

    def sendall(self, data):
        try:
            self.conn.send_bytes(data)
        except (EOFError, ValueError) as e:
            raise OSError(str(e))

    def close(self):
        self.conn.close()


def start_video_window(max_age_us, max_bytes):
    """Start the PC video window (relay_preview.py) in its own process; return its Client, or None."""
    import importlib.util
    missing = [m for m in ("av", "pygame") if importlib.util.find_spec(m) is None]
    if missing:
        print("video window: off (needs %s: pip install av pygame-ce)" % " and ".join(missing), flush=True)
        return None
    import multiprocessing
    import relay_preview
    ctx = multiprocessing.get_context("spawn")
    rx, tx = ctx.Pipe(duplex=False)
    p = ctx.Process(target=relay_preview.run, args=(rx,), daemon=True, name="video-window")
    p.start()
    rx.close()
    print("video window: on (--no-window turns it off)", flush=True)
    return Client(_PipeSock(tx), ("PC window", 0), max_age_us, max_bytes, name="video window")


def clock_server(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("0.0.0.0", port))
    except OSError as e:
        print("clock: bind UDP %d failed: %s (relay latency stat disabled)" % (port, e), flush=True)
        return
    print("clock: UDP 0.0.0.0:%d" % port, flush=True)
    while True:
        try:
            req, addr = s.recvfrom(64)
            if len(req) >= 8:
                s.sendto(req[:8] + struct.pack(">Q", now_us()), addr)
        except OSError:
            time.sleep(0.01)


DISCOVERY_PORT = 9202


def discovery_beacon(port, tcp_port, clock_port, interval=1.0):
    """Broadcasts 'QVISOR1 tcp=<port> clock=<port> name=<pc>' once a second on UDP <port> from every local IPv4,
    so the QuestVisor app finds this relay without the user typing an address."""
    msg = ("QVISOR1 tcp=%d clock=%d name=%s" % (tcp_port, clock_port, socket.gethostname()[:32])).encode()
    socks, last_refresh, announced = {}, 0.0, False
    while True:
        now = time.monotonic()
        if now - last_refresh > 10:                     # adapters come and go (Wi-Fi reconnect, USB goggles)
            last_refresh = now
            ips = set(local_ipv4s())
            for ip in list(socks):
                if ip not in ips:
                    socks.pop(ip).close()
            for ip in ips - set(socks):
                try:
                    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                    s.bind((ip, 0))
                    socks[ip] = s
                except OSError:
                    pass
            if socks and not announced:
                print("discovery: announcing on UDP %d from %s" % (port, ", ".join(sorted(socks))), flush=True)
                announced = True
        for ip, s in list(socks.items()):
            try:
                s.sendto(msg, ("255.255.255.255", port))
            except OSError:
                pass
        time.sleep(interval)


def raise_priority():
    if sys.platform == "win32":
        try:
            import ctypes
            k = ctypes.windll.kernel32
            k.SetPriorityClass(k.GetCurrentProcess(), 0x80)          # HIGH_PRIORITY_CLASS
            k.SetThreadPriority(k.GetCurrentThread(), 2)             # THREAD_PRIORITY_HIGHEST
        except Exception as e:                                         # noqa: BLE001
            print("priority:", e, flush=True)


GOGGLES_CANDIDATES = ("192.168.60.2", "192.168.60.1")   # the goggles' side of the USB (RNDIS) link
GOGGLES_PORT = 9100


def goggles_candidates():
    """Addresses to try for the goggles when --goggles is not given: the goggles' end of the USB link is
    192.168.60.2 or 192.168.60.1, whichever this PC does not hold itself."""
    own = all_ipv4s()
    return [ip for ip in GOGGLES_CANDIDATES if ip not in own] or list(GOGGLES_CANDIDATES)


def rndis_adapters():
    """Windows only: [(adapter name, [IPv4 addresses])] for the goggles' USB network card(s)
    ("Remote NDIS based Internet Sharing Device"), or None when it cannot be checked."""
    if sys.platform != "win32":
        return None
    import subprocess
    ps = ("Get-NetAdapter -IncludeHidden | Where-Object { $_.InterfaceDescription -match 'Remote NDIS|RNDIS' "
          "-and $_.Status -ne 'Not Present' } | ForEach-Object { $a = $_; "
          "$ips = @(Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue "
          "| ForEach-Object IPAddress) -join ','; \"$($a.Name)|$ips\" }")
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps], capture_output=True,
                           text=True, timeout=15, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, subprocess.SubprocessError):
        return None
    out = []
    for line in r.stdout.splitlines():
        if "|" in line:
            name, ips = line.rsplit("|", 1)
            out.append((name.strip(), [ip for ip in ips.split(",") if ip]))
    return out


def check_usb_network():
    """Explain what is wrong with the goggles' USB network, if anything can be seen from here."""
    ads = rndis_adapters()
    if ads is None:
        return
    if not ads:
        print("  no goggles network card on this PC: is the USB-C cable a data cable, plugged into the goggles' "
              "USB-C port, and are the goggles on?", flush=True)
        return
    for name, ips in ads:
        if any(ip.startswith("192.168.60.") for ip in ips):
            continue
        print("  the goggles' network card \"%s\" has no 192.168.60.x address (%s). Set one by hand:\n"
              "    Settings > Network & internet > Advanced network settings > %s > Edit IP settings > Manual,\n"
              "    IPv4 address 192.168.60.1, subnet mask 255.255.255.0, no gateway; or in an administrator "
              "PowerShell:\n"
              "    netsh interface ipv4 set address name=\"%s\" static 192.168.60.1 255.255.255.0\n"
              "  If the goggles are still not found, use 192.168.60.2 instead; the relay tries both."
              % (name, ", ".join(ips) or "none", name, name), flush=True)


def all_ipv4s():
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    return ips


def local_ipv4s():
    """Best-effort list of this PC's IPv4 addresses (to tell the user what to enter on the Quest)."""
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    try:                                    # the address of the default route (no packet is sent)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return sorted(ip for ip in ips if not ip.startswith(("127.", "169.254.", "192.168.60.")))


def main():
    ap = argparse.ArgumentParser(
        prog="QuestVisor-Relay" if getattr(sys, "frozen", False) else None,
        description="%s %s: relays the DJI Goggles 3 live video (USB, %s) to the QuestVisor app on a "
                    "Meta Quest over the local network." % (APP, __version__, "192.168.60.2 or .1, port 9100"))
    ap.add_argument("--version", action="version", version="%s %s" % (APP, __version__))
    ap.add_argument("--goggles", default=None,
                    help="goggles video address HOST[:PORT] (default: found automatically, "
                         "192.168.60.2 or 192.168.60.1, port 9100)")
    ap.add_argument("--listen", type=int, default=9201, help="TCP port the Quest connects to (default %(default)s)")
    ap.add_argument("--clock-port", type=int, default=9200, help="UDP clock server port, 0 = off (default %(default)s)")
    ap.add_argument("--discovery-port", type=int, default=DISCOVERY_PORT,
                    help="UDP port the relay announces itself on, 0 = off (default %(default)s)")
    ap.add_argument("--no-sei", action="store_true", help="do not insert relay timestamp SEIs")
    ap.add_argument("--max-backlog-ms", type=float, default=150, help="resync a client this far behind (default %(default)s)")
    ap.add_argument("--max-backlog-kb", type=int, default=1024, help="resync a client with this much queued (default %(default)s)")
    ap.add_argument("--stats", type=float, default=5.0, help="seconds between stats lines, 0 = off (default %(default)s)")
    ap.add_argument("--no-window", action="store_true", help="do not show the video on this PC")
    a = ap.parse_args()
    gport = GOGGLES_PORT
    if a.goggles:
        ghost = a.goggles
        if ":" in a.goggles:
            try:
                ghost, gport = a.goggles.rsplit(":", 1)
                gport = int(gport)
            except ValueError:
                ap.error("--goggles must be HOST or HOST:PORT, e.g. 192.168.60.2:9100")
        hosts = lambda: [ghost]                                              # noqa: E731
    else:
        hosts = goggles_candidates
    where = lambda: " or ".join("%s:%d" % (h, gport) for h in hosts())     # noqa: E731

    def connect_goggles():
        """Try each goggles address in turn; return (socket, host) or raise the last error."""
        err = OSError("no address to try")
        for h in hosts():
            try:
                return socket.create_connection((h, gport), timeout=2), h
            except OSError as e:
                err = e
        raise err

    print("%s %s" % (APP, __version__), flush=True)
    raise_priority()
    clients, lock = [], threading.Lock()
    scanner = Scanner(sei=not a.no_sei)

    srv = socket.socket()
    if sys.platform == "win32":
        # On Windows SO_REUSEADDR lets a second relay bind the same port silently (both then fight over the
        # goggles' single connection); exclusive use makes the second one fail with a clear message instead.
        srv.setsockopt(socket.SOL_SOCKET, getattr(socket, "SO_EXCLUSIVEADDRUSE", -5), 1)
    else:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", a.listen))
    except OSError as e:
        raise SystemExit("ERROR: cannot listen on TCP port %d (%s). Is another relay already running? "
                         "Close it, or pick another port with --listen." % (a.listen, e))
    srv.listen()
    print("listening on 0.0.0.0:%d (TCP) for the Quest" % a.listen, flush=True)
    ips = local_ipv4s()
    if ips:
        print("this PC: %s (QuestVisor finds the relay by itself; enter this address only if it does not)"
              % " or ".join(ips), flush=True)

    def accept():
        while True:
            c, addr = srv.accept()
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            c.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 128 * 1024)   # keep backlog visible to us
            with lock:
                clients.append(Client(c, addr, a.max_backlog_ms * 1000, a.max_backlog_kb * 1024))
            print("Quest connected: %s:%d" % addr, flush=True)

    threading.Thread(target=accept, daemon=True).start()
    if not a.no_window:
        # Up to 500 ms / 8 MB behind before the window is resynced (it is only a preview; the Quest's limits
        # stay tight).
        w = start_video_window(500 * 1000, 8 * 1024 * 1024)
        if w:
            with lock:
                clients.append(w)
    if a.clock_port:
        threading.Thread(target=clock_server, args=(a.clock_port,), daemon=True).start()
    if a.discovery_port:
        threading.Thread(target=discovery_beacon, args=(a.discovery_port, a.listen, a.clock_port),
                         daemon=True).start()

    rx_bytes, rx_frames, t_stat = 0, 0, time.monotonic()
    waiting_since, last_err = None, None
    while True:
        try:
            if waiting_since is None:
                waiting_since = time.monotonic()
                print("waiting for goggles at %s (DJI Goggles 3 on USB, powered on) ..." % where(), flush=True)
            g, gh = connect_goggles()
            g.settimeout(None)
            g.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            g.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
            print("goggles connected at %s:%d" % (gh, gport), flush=True)
            waiting_since, last_err = None, None
            scanner.reset()
            while True:
                b = g.recv(65536)
                if not b:
                    break
                t = now_us()
                out, auds = scanner.feed(b, t)
                rx_bytes += len(b)
                with lock:
                    clients[:] = [c for c in clients if c.alive]
                    for c in clients:
                        if c.active:
                            c.enqueue(out, t)
                        elif auds and scanner.params_ready():
                            if c.enqueue(scanner.param_blob() + SC4 + out[auds[0]:], t):
                                c.active = True
                if a.stats and time.monotonic() - t_stat >= a.stats:
                    dt = time.monotonic() - t_stat
                    with lock:
                        cl = ", ".join("%s backlog %.1f ms resyncs %d%s" % (
                            c.addr[0], c.backlog_ms(), c.resyncs, "" if c.active else " (waiting)") for c in clients)
                    print("in %.2f Mbit/s, %.1f fps, codec %s, params %s | %s" % (
                        rx_bytes * 8 / dt / 1e6, (scanner.frames - rx_frames) / dt, scanner.codec,
                        "yes" if scanner.params_ready() else "no", cl or "no clients"), flush=True)
                    rx_bytes, rx_frames, t_stat = 0, scanner.frames, time.monotonic()
            print("goggles closed the connection", flush=True)
        except OSError as e:
            # Print each distinct error once, plus a reminder every 30 s, instead of a line per retry.
            err = str(e)
            if waiting_since is not None and err != last_err:
                print("goggles: %s" % err, flush=True)
                if last_err is None:
                    check_usb_network()
                last_err = err
            elif waiting_since is not None and time.monotonic() - waiting_since > 30:
                print("still waiting for goggles at %s (USB connected? goggles on? only one relay at a time)"
                      % where(), flush=True)
                check_usb_network()
                waiting_since = time.monotonic()
            elif waiting_since is None:
                print("goggles: %s" % err, flush=True)
            time.sleep(1)


if __name__ == "__main__":
    import multiprocessing
    multiprocessing.freeze_support()          # the .exe starts the video window as a second copy of itself
    try:
        main()
    except KeyboardInterrupt:
        print("stopped", flush=True)
    except SystemExit as e:
        # When started by double-click (frozen exe), keep the window open long enough to read an error.
        if e.code not in (None, 0) and getattr(sys, "frozen", False) and sys.stdin and sys.stdin.isatty():
            print(e.code if isinstance(e.code, str) else "exit %s" % e.code, flush=True)
            try:
                input("press Enter to close")
            except (EOFError, KeyboardInterrupt):
                pass
            raise SystemExit(1)
        raise
