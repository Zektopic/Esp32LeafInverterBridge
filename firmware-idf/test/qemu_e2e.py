#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""End-to-end test of the ESP-IDF firmware in Espressif QEMU.

Needs the QEMU test build (OpenCores Ethernet instead of Wi-Fi):

    idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build
    python test/qemu_e2e.py --build build-qemu

QEMU emulates neither the TWAI controller nor an MCP2515, so this covers the
firmware around the CAN path: boot, NVS settings, web UI assets, the settings
API and its validation, the WebSocket push, the status API, HTTP auth and a
complete OTA update including the reboot into the new slot. The CAN message
logic itself is covered by the host tests in lib/.
"""
import argparse
import base64
import gzip
import http.client
import json
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PORT = 18080
OTA_USER = "admin"
OTA_PASSWORD = "Password"  # default Wi-Fi password (OTA password empty -> Wi-Fi password)

failures = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


class Qemu:
    def __init__(self, flash, log_path):
        self.flash = flash
        self.log = open(log_path, "a", encoding="utf-8", errors="replace")
        self.lines = []
        self.proc = None

    def start(self):
        self.lines = []
        self.log.write("\n===== QEMU start =====\n")
        self.proc = subprocess.Popen(
            ["qemu-system-xtensa", "-nographic", "-machine", "esp32", "-m", "4M",
             "-drive", f"file={self.flash},if=mtd,format=raw",
             "-nic", f"user,model=open_eth,id=lo0,hostfwd=tcp:127.0.0.1:{PORT}-:80",
             "-no-reboot"],
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
            if any(text in l for l in self.lines):
                return True
            if self.proc.poll() is not None:
                time.sleep(0.5)
                return any(text in l for l in self.lines)
            time.sleep(0.2)
        return False

    def seen(self, text):
        return any(text in l for l in self.lines)

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


def request(method, path, body=None, headers=None, timeout=60):
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=timeout)
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    data = resp.read()
    hdrs = {k.lower(): v for k, v in resp.getheaders()}
    conn.close()
    return resp.status, hdrs, data


def auth_header(user, password):
    return {"Authorization": "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()}


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
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("websocket closed")
            self.buf += chunk

    def recv_text(self):
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
        return payload.decode()

    def recv_n(self, n):
        return [self.recv_text() for _ in range(n)]

    def close(self):
        self.sock.close()


def ui_body(**values):
    """Exactly what index.html posts: JSON.stringify(Object.fromEntries(form), null, 2)."""
    fields = {"Vehicle": "LEAF", "Inverter": "110", "Battery": "24", "BatterySaver": "50",
              "Glide": "1", "Capacity": "1", "CurrentControl": "1"}
    fields.update(values)
    return json.dumps(fields, indent=2)


def boot(qemu, expect_slot=None):
    qemu.start()
    ok = qemu.wait_for("web server started", 90) and qemu.wait_for("QEMU ethernet got ip", 60)
    check(ok, "firmware boots and the web server comes up")
    if expect_slot:
        check(qemu.seen(f"Loaded app from partition at offset {expect_slot}"),
              f"bootloader started the app in the slot at {expect_slot}")
    # give lwIP a moment after DHCP
    time.sleep(1.0)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(HERE, "..", "build-qemu"))
    ap.add_argument("--log", default=None)
    ap.add_argument("--rollback-image", default=None,
                    help="app image built with CONFIG_BRIDGE_SELFTEST_REQUIRE_CAN=y; it must fail its "
                         "self test in QEMU (no MCP2515) and be rolled back")
    args = ap.parse_args()
    build = os.path.abspath(args.build)
    log_path = args.log or os.path.join(build, "qemu_e2e.log")
    flash = os.path.join(build, "qemu_flash.bin")
    app_bin = os.path.join(build, "leaf_inverter_bridge.bin")

    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32", "merge-bin", "--pad-to-size", "4MB",
                    "-o", flash, "@flash_args"], cwd=build, check=True, stdout=subprocess.DEVNULL)
    open(log_path, "w").close()
    qemu = Qemu(flash, log_path)

    try:
        print("[1] first boot", flush=True)
        if not boot(qemu):
            return 1
        check(qemu.seen("no stored settings, using defaults"), "fresh NVS falls back to default settings")
        check(qemu.seen("VCM bus running"), "TWAI node starts")
        check(qemu.seen("no MCP2515 found"), "missing MCP2515 is detected and reported (QEMU has none)")

        print("[2] web UI assets", flush=True)
        st, h, data = request("GET", "/")
        check(st == 200 and h.get("content-encoding") == "gzip", "GET / is served gzip-compressed")
        with open(os.path.join(REPO, "web", "index.html"), "rb") as f:
            check(gzip.decompress(data) == f.read(), "GET / decompresses to web/index.html")
        for asset in ("bootstrap.min.css", "bootstrap.min.js", "jquery.min.js", "popper.min.js"):
            st, h, data = request("GET", "/static/" + asset)
            with open(os.path.join(REPO, "web", "static", asset), "rb") as f:
                check(st == 200 and gzip.decompress(data) == f.read(), f"GET /static/{asset}")
        st, _, _ = request("GET", "/static/nope.js")
        check(st == 404, "unknown static asset -> 404")

        print("[3] status API", flush=True)
        st, h, data = request("GET", "/api/status")
        status = json.loads(data)
        check(st == 200 and h.get("content-type", "").startswith("application/json"), "GET /api/status is JSON")
        check(status["firmware"] == "2.0.0" and status["leaf_can"] == "2.0.0", "status reports versions")
        check(status["inverter_setting"] == "EM57 motor with 110 kW inverter", "default inverter setting is 110 kW")
        check(status["shifter"] == "P", "shifter defaults to P")
        check(status["vcm_bus"]["running"] is True and status["inverter_bus"]["running"] is False,
              "bus running flags reflect the hardware")

        print("[4] WebSocket push", flush=True)
        ws = WsClient()
        check(ws.status_line.startswith("HTTP/1.1 101"), "WebSocket handshake")
        msgs = ws.recv_n(7)
        check(msgs == ["Vehicle1", "Inverter1", "Battery1", "BatterySaver1", "Glide1", "Capacity1",
                       "CurrentControl1"], f"initial radio ids pushed ({msgs})")

        print("[5] settings API", flush=True)
        body = ui_body(Vehicle="AZE0", Inverter="160", CurrentControl="-1")
        st, _, data = request("POST", "/settings", body, {"Content-Type": "application/json"})
        check(st == 200 and data == b"Settings saved", f"valid settings saved ({st} {data!r})")
        msgs = ws.recv_n(7)
        check("Inverter2" in msgs and "Vehicle4" in msgs and "CurrentControl6" in msgs,
              f"change broadcast to open WebSocket clients ({msgs})")
        ws.close()
        st, _, data = request("GET", "/api/status")
        check(json.loads(data)["inverter_setting"] == "EM57 motor with 160 kW inverter", "new inverter setting live")

        st, _, data = request("POST", "/settings", body,
                              {"Content-Type": "application/x-www-form-urlencoded; charset=UTF-8"})
        check(st == 200 and data == b"Settings unchanged", "v1 page format (form content type) accepted")
        st, _, data = request("POST", "/settings", ui_body(Inverter="999"), {"Content-Type": "application/json"})
        check(st == 400 and b"invalid value" in data, f"invalid option rejected ({st} {data!r})")
        st, _, data = request("POST", "/settings", "{\"Inverter\":", {"Content-Type": "application/json"})
        check(st == 400, "malformed JSON rejected")
        st, _, data = request("POST", "/settings", "{" + "\"k\":\"v\"," * 300 + "}",
                              {"Content-Type": "application/json"})
        check(st == 400 and b"too large" in data, "oversized body rejected (v1.3.0 parser overflowed here)")
        st, _, data = request("GET", "/api/status")
        check(json.loads(data)["inverter_setting"] == "EM57 motor with 160 kW inverter",
              "rejected requests left the settings alone")

        print("[6] update page auth", flush=True)
        st, h, _ = request("GET", "/update")
        check(st == 401 and "basic" in h.get("www-authenticate", "").lower(), "GET /update needs auth")
        st, _, _ = request("GET", "/update", headers=auth_header(OTA_USER, "wrong-password"))
        check(st == 401, "wrong password rejected")
        st, h, data = request("GET", "/update", headers=auth_header(OTA_USER, OTA_PASSWORD))
        check(st == 200 and b"Firmware update" in gzip.decompress(data), "update page with valid credentials")
        st, _, _ = request("POST", "/update", b"\x00" * 1024, {"Content-Type": "application/octet-stream"})
        check(st == 401, "POST /update needs auth")

        print("[7] OTA update", flush=True)
        hdr = dict(auth_header(OTA_USER, OTA_PASSWORD), **{"Content-Type": "application/octet-stream"})
        st, _, data = request("POST", "/update", os.urandom(4096), hdr)
        check(st == 400 and b"Not a valid" in data, f"garbage image rejected ({st} {data!r})")
        with open(app_bin, "rb") as f:
            image = f.read()
        t0 = time.time()
        st, _, data = request("POST", "/update", image, hdr, timeout=600)
        check(st == 200 and b"Update complete" in data, f"real image accepted ({st} {data!r}, "
                                                        f"{len(image)} bytes in {time.time() - t0:.0f} s)")
        check(qemu.wait_exit(30), "bridge restarts after the update")

        print("[8] boot into the new slot", flush=True)
        if not boot(qemu, expect_slot="0x150000"):
            return 1
        check(qemu.seen("new firmware is healthy, cancelling rollback"), "new image confirmed itself")
        st, _, data = request("GET", "/api/status")
        check(json.loads(data)["inverter_setting"] == "EM57 motor with 160 kW inverter",
              "settings survived the update and reboot (NVS)")
        check(not qemu.seen("Guru Meditation") and not qemu.seen("abort()"), "no crash in either boot")

        if args.rollback_image:
            print("[9] automatic rollback of an image that fails its self test", flush=True)
            with open(args.rollback_image, "rb") as f:
                bad = f.read()
            st, _, data = request("POST", "/update", bad, hdr, timeout=600)
            check(st == 200, f"self-test-failing image uploaded ({st} {data!r})")
            check(qemu.wait_exit(30), "bridge restarts after the update")
            qemu.start()
            check(qemu.wait_for("Loaded app from partition at offset 0x10000", 60), "new image boots from 0x10000")
            check(qemu.wait_for("failed its self test, rolling back", 90), "self test fails (no MCP2515)")
            check(qemu.wait_exit(30), "image marks itself invalid and resets")
            if not boot(qemu, expect_slot="0x150000"):
                return 1
            st, _, data = request("GET", "/api/status")
            check(st == 200 and json.loads(data)["inverter_setting"] == "EM57 motor with 160 kW inverter",
                  "previous firmware is running again with its settings")
    finally:
        qemu.stop()

    print(f"\n{'FAILED' if failures else 'PASSED'}: {len(failures)} failure(s); serial log: {log_path}")
    for f in failures:
        print("  - " + f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
