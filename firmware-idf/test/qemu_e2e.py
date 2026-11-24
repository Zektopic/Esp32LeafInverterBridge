#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""End-to-end test of the ESP-IDF firmware in Espressif QEMU.

Needs the QEMU test build (OpenCores Ethernet instead of Wi-Fi):

    idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build
    python test/qemu_e2e.py --build build-qemu [--rollback-image build-qemu-rb/leaf_inverter_bridge.bin]

    # keep a fresh device running for the browser test (test/ui/ui_test.mjs)
    python test/qemu_e2e.py --build build-qemu --serve --listen 0.0.0.0

QEMU emulates neither the TWAI controller nor an MCP2515, so this covers the
firmware around the CAN path: boot, NVS, the web UI assets and their headers,
the REST API (auth, forced password change, lockout, settings, Wi-Fi,
diagnostics), the WebSocket live feed, the captive-portal DNS server and
redirect, OTA updates (validation, install, reboot into the new slot, rollback),
restart and factory reset. The CAN message logic itself is covered by the host
tests in lib/.
"""
import argparse
import base64
import gzip
import http.client
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WWW = os.path.abspath(os.path.join(HERE, "..", "main", "www"))
PORT = 18080
DNS_PORT = 18053
GUEST_IP = "10.0.2.15"          # QEMU user-mode network address of the firmware
FACTORY_PASSWORD = "Password"   # CONFIG_BRIDGE_WIFI_PASSWORD (admin password empty -> Wi-Fi password)
NEW_PASSWORD = "e2e-Admin-2026!"
EXPECTED_VERSION = "2.1.0"

failures = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)
    return cond


class Qemu:
    def __init__(self, flash, log_path, listen):
        self.flash = flash
        self.log = open(log_path, "a", encoding="utf-8", errors="replace")
        self.listen = listen
        self.lines = []
        self.proc = None

    def start(self):
        self.lines = []
        self.log.write("\n===== QEMU start =====\n")
        nic = (f"user,model=open_eth,id=lo0,hostfwd=tcp:{self.listen}:{PORT}-:80,"
               f"hostfwd=udp:{self.listen}:{DNS_PORT}-:53")
        self.proc = subprocess.Popen(
            ["qemu-system-xtensa", "-nographic", "-machine", "esp32", "-m", "4M",
             "-drive", f"file={self.flash},if=mtd,format=raw", "-nic", nic, "-no-reboot"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for raw in self.proc.stdout:
            line = raw.decode("utf-8", errors="replace").rstrip()
            self.lines.append(line)
            self.log.write(line + "\n")
            self.log.flush()

    def wait_for(self, text, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if self.seen(text):
                return True
            if self.proc.poll() is not None:
                time.sleep(0.5)
                return self.seen(text)
            time.sleep(0.2)
        return False

    def seen(self, text):
        return any(text in line for line in self.lines)

    def count(self, text):
        return sum(1 for line in self.lines if text in line)

    def wait_exit(self, timeout):
        try:
            self.proc.wait(timeout)
            return True
        except subprocess.TimeoutExpired:
            return False

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


# ---------------------------------------------------------------- HTTP helpers
def request(method, path, body=None, headers=None, timeout=60):
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=timeout)
    if isinstance(body, (dict, list)):
        body = json.dumps(body)
        headers = dict(headers or {}, **{"Content-Type": "application/json"})
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    data = resp.read()
    hdrs = {k.lower(): v for k, v in resp.getheaders()}
    conn.close()
    return resp.status, hdrs, data


def jrequest(method, path, body=None, headers=None, timeout=60):
    st, h, data = request(method, path, body, headers, timeout)
    try:
        doc = json.loads(data) if data else None
    except ValueError:
        doc = None
    return st, h, doc


def bearer(token):
    return {"Authorization": f"Bearer {token}"}


def basic(password, user="admin"):
    return {"Authorization": "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()}


def login(password):
    st, _, doc = jrequest("POST", "/api/login", {"password": password})
    return st, doc


class WsClient:
    def __init__(self, path="/ws"):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {path} HTTP/1.1\r\nHost: bridge\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise ConnectionError("websocket handshake closed")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        self.status_line = head.split(b"\r\n", 1)[0].decode()

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(8192)
            if not chunk:
                raise ConnectionError("websocket closed")
            self.buf += chunk

    def recv(self):
        self._need(2)
        opcode = self.buf[0] & 0x0F
        length = self.buf[1] & 0x7F
        off = 2
        if length == 126:
            self._need(4)
            length = int.from_bytes(self.buf[2:4], "big")
            off = 4
        self._need(off + length)
        payload = self.buf[off:off + length]
        self.buf = self.buf[off + length:]
        if opcode != 1:
            raise ValueError(f"unexpected opcode {opcode}")
        return json.loads(payload)

    def recv_until(self, pred, timeout=10):
        end = time.time() + timeout
        seen = []
        while time.time() < end:
            msg = self.recv()
            seen.append(msg.get("type"))
            if pred(msg):
                return msg, seen
        return None, seen

    def close(self):
        self.sock.close()


def dns_query(name, qtype=1):
    q = struct.pack(">HHHHHH", 0x1234, 0x0100, 1, 0, 0, 0)
    for label in name.split("."):
        q += bytes([len(label)]) + label.encode()
    q += b"\x00" + struct.pack(">HH", qtype, 1)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    try:
        s.sendto(q, ("127.0.0.1", DNS_PORT))
        data, _ = s.recvfrom(512)
    finally:
        s.close()
    return q, data


def boot(qemu, expect_slot=None, label="firmware boots and the web server comes up"):
    qemu.start()
    ok = qemu.wait_for("web server started", 120) and qemu.wait_for("QEMU ethernet got ip", 60)
    check(ok, label)
    if expect_slot:
        check(qemu.seen(f"Loaded app from partition at offset {expect_slot}"),
              f"bootloader started the app in the slot at {expect_slot}")
    time.sleep(1.0)  # give lwIP a moment after DHCP
    return ok


def gz_equals(data, name):
    with open(os.path.join(WWW, name), "rb") as f:
        return gzip.decompress(data) == f.read()


def restart_and_boot(qemu, expect_slot=None, label="bridge comes back after the restart"):
    check(qemu.wait_exit(30), "bridge restarts")
    return boot(qemu, expect_slot, label)


# ---------------------------------------------------------------- the test
def run_tests(qemu, args, app_bin):
    print("[1] first boot", flush=True)
    if not boot(qemu):
        return
    check(qemu.seen("no stored settings, using defaults"), "fresh NVS falls back to default settings")
    check(qemu.seen("VCM bus running"), "TWAI node starts")
    check(qemu.seen("bridge running on CPU 1"), "bridge task and CAN drivers run on CPU 1")
    check(qemu.seen("no MCP2515 found"), "missing MCP2515 is detected and reported (QEMU has none)")
    check(qemu.seen("admin password is the factory default"), "factory default admin password is flagged")
    check(qemu.seen("captive portal DNS listening"), "captive portal DNS server starts")

    print("[2] web UI assets and headers", flush=True)
    st, h, data = request("GET", "/")
    check(st == 200 and h.get("content-encoding") == "gzip" and gz_equals(data, "index.html"),
          "GET / serves www/index.html gzip-compressed")
    csp = h.get("content-security-policy", "")
    check("script-src 'self'" in csp and "frame-ancestors 'none'" in csp, "strict Content-Security-Policy on the page")
    check(h.get("x-content-type-options") == "nosniff" and h.get("x-frame-options") == "DENY",
          "nosniff / frame-deny headers")
    etag = h.get("etag")
    for name in ("app.js", "app.css"):
        st, h2, data = request("GET", "/" + name)
        check(st == 200 and gz_equals(data, name) and h2.get("etag") == etag, f"GET /{name}")
    st, h2, data = request("GET", "/app.js", headers={"If-None-Match": etag})
    check(st == 304 and not data, "unchanged asset revalidates with 304 Not Modified")
    st, _, _ = request("GET", "/nope.js")
    check(st == 404, "unknown path -> 404")
    st, h2, _ = request("GET", "/update")
    check(st == 302 and h2.get("location") == "/#firmware", "old /update page redirects to the firmware tab")

    print("[3] status and settings API", flush=True)
    st, h, s = jrequest("GET", "/api/status")
    check(st == 200 and h.get("cache-control") == "no-store", "GET /api/status is JSON, not cached")
    check(s["firmware"] == EXPECTED_VERSION and s["leaf_can"] == "2.0.0", "status reports versions")
    check(s["shifter"] == "P" and s["parked"] is True, "shifter defaults to P")
    inv = s["inverter"]
    check(inv["mode"] == "110" and inv["modify_torque"] is True and inv["drive_multiplier"] == 0.9
          and inv["regen_multiplier"] == 1.1, f"default 110 kW mode with its multipliers ({inv})")
    check(s["buses"]["vcm"]["running"] is True and s["buses"]["vcm"]["state"] == "active", "VCM bus active")
    check(s["buses"]["inverter"]["running"] is False and s["buses"]["inverter"]["state"] == "stopped",
          "inverter bus reported as stopped")
    check(s["battery"]["soc_percent"] is None and s["security"]["default_password"] is True,
          "no SOC yet, default password flagged")
    st, _, cfg = jrequest("GET", "/api/settings")
    ids = [x["id"] for x in cfg["settings"]]
    check(st == 200 and ids == ["Vehicle", "Inverter", "Battery", "BatterySaver", "Glide", "Capacity",
                                "CurrentControl"], "GET /api/settings lists every setting")
    invs = next(x for x in cfg["settings"] if x["id"] == "Inverter")
    check(invs["implemented"] and invs["value"] == "110" and
          [(o["value"], o["drive_multiplier"]) for o in invs["options"]] == [("110", 0.9), ("160", 1.6), ("0", 1.0)],
          "inverter options carry their torque multipliers")

    print("[4] authentication and the forced password change", flush=True)
    st, _, doc = jrequest("POST", "/api/settings", {"Inverter": "160"})
    check(st == 401 and doc["error"] == "unauthorized", "changes need a login")
    st, doc = login("wrong-password")
    check(st == 401 and doc["error"] == "bad_password", "wrong password rejected")
    st, doc = login(FACTORY_PASSWORD)
    check(st == 200 and len(doc["token"]) == 32 and doc["default_password"] is True, "factory password logs in")
    token = doc["token"]
    st, _, doc = jrequest("GET", "/api/session", headers=bearer(token))
    check(doc["authenticated"] is True, "session endpoint recognises the token")
    st, _, doc = jrequest("POST", "/api/settings", {"Inverter": "160"}, bearer(token))
    check(st == 403 and doc["error"] == "password_change_required", "changes blocked until the password is changed")
    st, _, doc = jrequest("POST", "/update", b"\0" * 1024, dict(basic(FACTORY_PASSWORD),
                                                                **{"Content-Type": "application/octet-stream"}))
    check(st == 403 and doc["error"] == "password_change_required", "OTA blocked with the factory password too")
    st, _, doc = jrequest("POST", "/api/password", {"current": "nope-nope", "new": NEW_PASSWORD}, bearer(token))
    check(st == 403 and doc["error"] == "bad_password", "password change needs the current password")
    st, _, doc = jrequest("POST", "/api/password", {"current": FACTORY_PASSWORD, "new": "short"}, bearer(token))
    check(st == 400 and doc["error"] == "weak_password", "too short a password is refused")
    st, _, doc = jrequest("POST", "/api/password", {"current": FACTORY_PASSWORD, "new": FACTORY_PASSWORD},
                          bearer(token))
    check(st == 400 and doc["error"] == "weak_password", "the factory password cannot be set again")
    st, _, doc = jrequest("POST", "/api/password", {"current": FACTORY_PASSWORD, "new": NEW_PASSWORD}, bearer(token))
    check(st == 200 and len(doc.get("token", "")) == 32 and doc["default_password"] is False,
          "password changed, fresh token issued")
    old_token, token = token, doc["token"]
    st, _, doc = jrequest("POST", "/api/settings", {"Inverter": "110"}, bearer(old_token))
    check(st == 401, "the old session was signed out")
    st, doc = login(FACTORY_PASSWORD)
    check(st == 401, "factory password no longer works")
    st, _, s = jrequest("GET", "/api/status")
    check(s["security"]["default_password"] is False, "status no longer flags the default password")

    print("[5] settings changes and the live feed", flush=True)
    ws = WsClient()
    check(ws.status_line.startswith("HTTP/1.1 101"), "WebSocket handshake")
    first = ws.recv()
    check(first.get("type") == "status" and first["data"]["firmware"] == EXPECTED_VERSION,
          "status pushed immediately after the handshake")
    t0 = time.time()
    n = 0
    while time.time() - t0 < 1.6:
        if ws.recv().get("type") == "status":
            n += 1
    check(n >= 4, f"periodic status push (~4 per second, got {n} in 1.6 s)")
    st, _, doc = jrequest("POST", "/api/settings", {"Inverter": "160", "Vehicle": "AZE0"}, bearer(token))
    check(st == 200 and doc["ok"] and "saved" in doc["message"].lower(), f"settings saved ({st} {doc})")
    msg, seen = ws.recv_until(lambda m: m.get("type") == "settings")
    check(msg is not None, f"settings change announced on the WebSocket ({seen[-5:]})")
    msg, _ = ws.recv_until(lambda m: m.get("type") == "status" and m["data"]["inverter"]["mode"] == "160")
    check(msg is not None and msg["data"]["inverter"]["drive_multiplier"] == 1.6, "live feed shows the 160 kW mode")
    ws.close()
    st, _, doc = jrequest("POST", "/api/settings", {"Inverter": "160"}, basic(NEW_PASSWORD))
    check(st == 200 and doc["message"] == "No changes", "HTTP basic auth works for scripts; unchanged detected")
    st, _, doc = jrequest("POST", "/api/settings", {"Inverter": "999"}, bearer(token))
    check(st == 400 and doc["error"] == "invalid_settings", "invalid option rejected")
    st, _, doc = jrequest("POST", "/api/settings", "{\"Inverter\":", {**bearer(token), "Content-Type": "application/json"})
    check(st == 400, "malformed JSON rejected")
    st, _, doc = jrequest("POST", "/api/settings", "{" + "\"k\":\"v\"," * 300 + "}",
                          {**bearer(token), "Content-Type": "application/json"})
    check(st == 413 and doc["error"] == "too_large", "oversized body rejected (v1.3.0 overflowed here)")
    st, _, cfg = jrequest("GET", "/api/settings")
    vals = {x["id"]: x["value"] for x in cfg["settings"]}
    check(vals["Inverter"] == "160" and vals["Vehicle"] == "AZE0", "rejected requests left the settings alone")

    print("[6] Wi-Fi settings", flush=True)
    st, _, net = jrequest("GET", "/api/network")
    check(st == 200 and net["ssid"] == "Can-bridgeINV" and net["default_password"] is True and "password" not in net,
          "GET /api/network shows the SSID, never the password")
    st, _, doc = jrequest("POST", "/api/network", {"ssid": "LeafBridge-E2E", "password": "short"}, bearer(token))
    check(st == 400 and doc["error"] == "invalid_network", "too short a Wi-Fi password refused")
    st, _, doc = jrequest("POST", "/api/network", {"ssid": "", "password": "longenough1"}, bearer(token))
    check(st == 400, "empty SSID refused")
    st, _, doc = jrequest("POST", "/api/network", {"ssid": "LeafBridge-E2E", "password": "wifi-e2e-pass"},
                          bearer(token))
    check(st == 200 and "LeafBridge-E2E" in doc["message"], "Wi-Fi settings saved")
    st, _, net = jrequest("GET", "/api/network")
    check(net["ssid"] == "LeafBridge-E2E" and net["default_password"] is False, "new SSID reported")

    print("[7] diagnostics", flush=True)
    st, _, sysd = jrequest("GET", "/api/system")
    check(st == 200 and sysd["firmware"]["version"] == EXPECTED_VERSION and sysd["firmware"]["project"] ==
          "leaf_inverter_bridge", "GET /api/system reports the firmware")
    tasks = {t["name"]: t["stack_free"] for t in sysd["tasks"]}
    check(all(tasks.get(n, 0) > 512 for n in ("bridge", "mcp2515", "httpd", "dns")),
          f"every task keeps stack headroom ({tasks})")
    check(sysd["device"]["cpu_mhz"] == 240 and sysd["memory"]["heap_free"] > 50000, "240 MHz, plenty of heap")
    check(sysd["ota"]["running"] == "app0" and sysd["ota"]["next"] == "app1" and sysd["ota"]["slot_bytes"] == 0x140000,
          f"OTA slot information ({sysd['ota']})")
    check(sysd["coredump"]["present"] is False, "no crash dump stored")
    check(qemu.count("no MCP2515 found") == 1, "missing MCP2515 is retried in the background without log spam")
    st, _, doc = jrequest("GET", "/api/coredump", headers=bearer(token))
    check(st == 404, "crash dump download -> 404 when there is none")

    print("[8] captive portal", flush=True)
    q, r = dns_query("connectivitycheck.gstatic.com")
    check(r[:2] == q[:2] and r[3] & 0x0F == 0 and r[6:8] == b"\x00\x01" and
          socket.inet_ntoa(r[-4:]) == GUEST_IP, "DNS answers every A query with the bridge's address")
    _, r = dns_query("captive.apple.com", qtype=28)
    check(r[6:8] == b"\x00\x00" and r[3] & 0x0F == 0, "AAAA queries get an empty answer")
    st, h, _ = request("GET", "/generate_204", headers={"Host": "connectivitycheck.gstatic.com"})
    check(st == 302 and h.get("location") == f"http://{GUEST_IP}/", "Android connectivity check redirected")
    st, h, _ = request("GET", "/hotspot-detect.html", headers={"Host": "captive.apple.com"})
    check(st == 302, "Apple captive-portal check redirected")
    st, _, doc = jrequest("GET", "/api/nope")
    check(st == 404 and doc["error"] == "not_found", "unknown API endpoint -> JSON 404")

    print("[9] OTA validation and update", flush=True)
    hdr = dict(bearer(token), **{"Content-Type": "application/octet-stream"})
    st, _, doc = jrequest("POST", "/api/update", b"\0" * 1024, {"Content-Type": "application/octet-stream"})
    check(st == 401, "OTA needs a login")
    st, _, doc = jrequest("POST", "/api/update", os.urandom(4096), hdr)
    check(st == 400 and doc["error"] == "invalid_image", "random data rejected before flashing")
    with open(app_bin, "rb") as f:
        image = f.read()
    other = bytearray(image)
    other[80:112] = b"some_other_project".ljust(32, b"\0")
    st, _, doc = jrequest("POST", "/api/update", bytes(other), hdr)
    check(st == 400 and "different firmware" in doc["message"], f"firmware of another project rejected ({doc})")
    check(not qemu.seen("firmware update to"), "rejected images never reached the flash")
    t0 = time.time()
    st, _, doc = jrequest("POST", "/api/update", image, dict(basic(NEW_PASSWORD),
                                                             **{"Content-Type": "application/octet-stream"}),
                          timeout=600)
    check(st == 200 and doc["version"] == EXPECTED_VERSION,
          f"real image accepted ({st} {doc}, {len(image)} bytes in {time.time() - t0:.0f} s)")
    if not restart_and_boot(qemu, "0x150000", "updated firmware boots"):
        return
    check(qemu.seen("new firmware is healthy, cancelling rollback"), "new image confirmed itself")
    st, doc = login(NEW_PASSWORD)
    check(st == 200 and doc["default_password"] is False, "admin password survived the update")
    token = doc["token"]
    st, _, cfg = jrequest("GET", "/api/settings")
    check({x["id"]: x["value"] for x in cfg["settings"]}["Inverter"] == "160", "settings survived the update (NVS)")
    st, _, net = jrequest("GET", "/api/network")
    check(net["ssid"] == "LeafBridge-E2E", "Wi-Fi settings survived the update")
    st, _, sysd = jrequest("GET", "/api/system")
    check(sysd["ota"]["running"] == "app1" and sysd["ota"]["next_version"] == EXPECTED_VERSION,
          f"running from app1, previous firmware kept in app0 ({sysd['ota']})")

    print("[10] restart", flush=True)
    st, _, doc = jrequest("POST", "/api/reboot", headers=bearer(token))
    check(st == 200, "restart accepted")
    if not restart_and_boot(qemu, "0x150000"):
        return
    # QEMU runs with -no-reboot, so each boot is a fresh power-on from the emulator's point of view
    check(qemu.seen("last reset: "), "reset reason logged at boot")

    if args.rollback_image:
        print("[11] automatic rollback of an image that fails its self test", flush=True)
        st, doc = login(NEW_PASSWORD)
        token = doc["token"]
        with open(args.rollback_image, "rb") as f:
            bad = f.read()
        st, _, doc = jrequest("POST", "/api/update", bad, dict(bearer(token), **{"Content-Type": "application/octet-stream"}),
                              timeout=600)
        check(st == 200, f"self-test-failing image uploaded ({st} {doc})")
        check(qemu.wait_exit(30), "bridge restarts after the update")
        qemu.start()
        check(qemu.wait_for("Loaded app from partition at offset 0x10000", 60), "new image boots from 0x10000")
        check(qemu.wait_for("failed its self test, rolling back", 90), "self test fails (no MCP2515)")
        check(qemu.wait_exit(30), "image marks itself invalid and resets")
        if not boot(qemu, "0x150000", "previous firmware boots again"):
            return
        st, _, cfg = jrequest("GET", "/api/settings")
        check({x["id"]: x["value"] for x in cfg["settings"]}["Inverter"] == "160",
              "previous firmware is running again with its settings")

    print("[12] factory reset", flush=True)
    st, doc = login(NEW_PASSWORD)
    token = doc["token"]
    st, _, doc = jrequest("POST", "/api/factory-reset", headers=bearer(token))
    check(st == 200, "factory reset accepted")
    if not restart_and_boot(qemu):
        return
    st, _, s = jrequest("GET", "/api/status")
    check(s["security"]["default_password"] is True and s["inverter"]["mode"] == "110",
          "factory defaults restored (password, settings)")
    st, _, net = jrequest("GET", "/api/network")
    check(net["ssid"] == "Can-bridgeINV", "factory Wi-Fi network name restored")

    print("[13] login lockout", flush=True)
    codes = [login("wrong-password")[0] for _ in range(5)]
    check(codes[:4] == [401] * 4 and codes[4] == 429, f"fifth wrong password locks the login ({codes})")
    st, h, doc = jrequest("POST", "/api/login", {"password": FACTORY_PASSWORD})
    check(st == 429 and int(h.get("retry-after", "0")) > 0 and doc["error"] == "locked",
          "even the right password waits for the lock to expire")
    check(not qemu.seen("Guru Meditation") and not qemu.seen("abort()") and not qemu.seen("stack overflow"),
          "no crash in any boot")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(HERE, "..", "build-qemu"))
    ap.add_argument("--log", default=None)
    ap.add_argument("--listen", default="127.0.0.1", help="host address the forwarded ports bind to")
    ap.add_argument("--serve", action="store_true",
                    help="boot a fresh device and keep it running (restarting QEMU after reboots) until killed")
    ap.add_argument("--rollback-image", default=None,
                    help="app image built with CONFIG_BRIDGE_SELFTEST_REQUIRE_CAN=y; it must fail its "
                         "self test in QEMU (no MCP2515) and be rolled back")
    args = ap.parse_args()
    build = os.path.abspath(args.build)
    log_path = args.log or os.path.join(build, "qemu_serve.log" if args.serve else "qemu_e2e.log")
    flash = os.path.join(build, "qemu_flash.bin")
    app_bin = os.path.join(build, "leaf_inverter_bridge.bin")

    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32", "merge-bin", "--pad-to-size", "4MB",
                    "-o", flash, "@flash_args"], cwd=build, check=True, stdout=subprocess.DEVNULL)
    open(log_path, "w").close()
    qemu = Qemu(flash, log_path, args.listen)

    if args.serve:
        try:
            boot(qemu)
            print(f"SERVING http://{args.listen}:{PORT}/", flush=True)
            while True:
                if qemu.proc.poll() is not None:  # the firmware restarted (-no-reboot)
                    print("firmware restarted - booting again", flush=True)
                    boot(qemu)
                    print("SERVING again", flush=True)
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass
        finally:
            qemu.stop()
        return 0

    try:
        run_tests(qemu, args, app_bin)
    finally:
        qemu.stop()

    print(f"\n{'FAILED' if failures else 'PASSED'}: {len(failures)} failure(s); serial log: {log_path}")
    for f in failures:
        print("  - " + f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
