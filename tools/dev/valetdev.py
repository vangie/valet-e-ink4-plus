#!/usr/bin/env python3
"""Drive an E-Ink4 Plus on the desk over its USB serial port.

Needs firmware built with CONFIG_VALET_DEV_LINK (tools/dev/build.sh) and
pyserial; the ESP-IDF Python environment has both pyserial and esptool.

One process holds the port: `serve` opens it (without resetting the device),
keeps the log, and every other command talks to that process over a local
socket. The port is never reopened on its own — after the device drops off USB
(standby, power-off, the re-enumeration right after a reset), run `attach`.

    valetdev.py serve &            # hold the port, write the log
    valetdev.py info               # active screen, refresh state, version
    valetdev.py shot home.png      # the frame the UI drew, upright, as PNG
    valetdev.py tap 342 600        # portrait panel pixels, like the PNG
    valetdev.py swipe 342 900 342 300
    valetdev.py key home|prev|next|ai|vol_up|vol_down|power [--ms 800]
    valetdev.py home               # ask the UI to return to the home screen
    valetdev.py preview standby    # draw the standby page on a throwaway screen (`home` leaves it)
    valetdev.py log -n 80          # recent device log (credentials masked)
    valetdev.py sdlog out.txt      # the log the device keeps on its SD card (covers times nothing was listening)
    valetdev.py flash              # write build-devtest app to ota_0, reattach
    valetdev.py stop

A screenshot is what the firmware asked the panel to show. Grey levels,
ghosting and partial-refresh artefacts on the real ink are not in it.
"""
import argparse
import base64
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
STATE = REPO / "build-devtest" / "devlink"
SOCK = STATE / "valetdev.sock"
LOG = STATE / "device.log"
APP = REPO / "build-devtest" / "Metalio-E-Ink4-Plus.bin"
OTA0 = 0x80000  # partitions/v1/16m.csv; the official image stays in ota_1
PUSH = 16     # newlines after each command; 8 were enough when measured
SECRET = re.compile(r"(?i)(token|secret|password|passwd|authorization|api[_-]?key|code)(\"?\s*[=:]\s*\"?)[^\s\",&}]+")


def find_port():
    import serial.tools.list_ports as lp
    ports = [p.device for p in lp.comports() if p.vid == 0x303A]
    if len(ports) != 1:
        sys.exit(f"expected one Espressif USB serial port, found {ports or 'none'}; pass --port")
    return ports[0]


# ---- daemon ---------------------------------------------------------------

class Link:
    def __init__(self, port):
        self.port_name = port
        self.port = None
        self.lock = threading.Lock()        # one command at a time
        self.cond = threading.Condition()
        self.replies = {}
        self.chunks = {}
        self.next_id = int(time.time()) % 100000 * 10 + 1
        self.note = "detached"
        self.log = open(LOG, "ab", buffering=0)

    def attach(self):
        import serial
        if self.port:
            return "already attached"
        port = serial.Serial()
        port.port, port.baudrate, port.timeout = self.port_name, 115200, 0.2
        # RTS high with DTR low is the chip's reset request, and macOS raises
        # both lines on open. Opening with DTR high and RTS low, then dropping
        # DTR, never passes through that combination, so the device keeps running.
        port.dtr, port.rts = True, False
        port.open()
        port.dtr = False
        self.port = port
        self.note = "attached"
        self.mark(f"attached {self.port_name}")
        threading.Thread(target=self.read_loop, args=(port,), daemon=True).start()
        return "attached"

    def release(self, why="released"):
        port, self.port = self.port, None
        if port:
            try:
                port.close()
            except Exception:
                pass
        self.note = why
        self.mark(why)
        return why

    def mark(self, text):
        self.log.write(f"\n[valetdev {time.strftime('%H:%M:%S')}] {text}\n".encode())

    def read_loop(self, port):
        pending = b""
        while self.port is port:
            try:
                data = port.read(8192)
            except Exception as error:
                if self.port is port:
                    self.release(f"port lost: {error}")
                return
            if not data:
                continue
            pending += data
            *lines, pending = pending.split(b"\n")
            for raw in lines:
                line = raw.rstrip(b"\r")
                if line.startswith(b"@DEVD "):
                    try:
                        _, rid, index, payload = line.split(b" ", 3)
                        with self.cond:
                            self.chunks.setdefault(int(rid), {})[int(index)] = base64.b64decode(payload)
                            self.cond.notify_all()
                    except Exception:
                        self.mark("bad data line")
                    continue
                if line.startswith(b"@DEV {"):
                    try:
                        reply = json.loads(line[5:])
                        with self.cond:
                            self.replies[reply.get("id")] = reply
                            self.cond.notify_all()
                    except Exception:
                        pass
                if line:
                    self.log.write(line + b"\n")

    def command(self, request, timeout=5.0):
        with self.lock:
            if not self.port:
                return {"ok": False, "code": "DETACHED", "note": self.note}
            rid, self.next_id = self.next_id, self.next_id + 1
            request = dict(request, id=rid)
            # The device can hold a line back until a few more bytes arrive (seen on 2026-11-21: a
            # command sent alone was only handled when the next one came, so every reply was one
            # command late and was thrown away as unexpected). Trailing newlines are empty lines to
            # the firmware and push the command through.
            self.port.write(b"\n@DEV " + json.dumps(request, separators=(",", ":")).encode() + b"\n" * PUSH)
            with self.cond:
                self.replies.clear()      # a reply left over from a command that timed out is not ours
            deadline = time.time() + timeout
            with self.cond:
                while rid not in self.replies:
                    left = deadline - time.time()
                    if left <= 0 or not self.port:
                        return {"ok": False, "code": "NO_REPLY",
                                "note": "no answer: is this firmware built with the dev link?"}
                    self.cond.wait(min(left, 0.5))
                reply = self.replies.pop(rid)
                if request["cmd"] in ("shot", "sdlog") and reply.get("ok"):
                    count = reply["chunks"]
                    deadline = time.time() + 20
                    while len(self.chunks.get(rid, {})) < count:
                        left = deadline - time.time()
                        if left <= 0 or not self.port:
                            got = len(self.chunks.pop(rid, {}))
                            return {"ok": False, "code": "SHOT_INCOMPLETE", "note": f"{got}/{count} chunks"}
                        self.cond.wait(min(left, 0.5))
                    parts = self.chunks.pop(rid)
                    reply["data"] = base64.b64encode(b"".join(parts[i] for i in range(count))).decode()
            return reply


def serve(args):
    STATE.mkdir(parents=True, exist_ok=True)
    if SOCK.exists():
        try:
            call({"op": "status"})
            sys.exit("already running; use `valetdev.py stop` first")
        except OSError:
            SOCK.unlink()
    link = Link(args.port or find_port())
    link.attach()
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(SOCK))
    os.chmod(SOCK, 0o600)
    server.listen(4)
    print(f"holding {link.port_name}; log {LOG}", flush=True)
    while True:
        conn, _ = server.accept()
        try:
            request = json.loads(conn.makefile("r").readline())
            op = request.get("op")
            if op == "dev":
                reply = link.command(request["request"])
            elif op == "release":
                reply = {"ok": True, "note": link.release()}
            elif op == "attach":
                try:
                    reply = {"ok": True, "note": link.attach()}
                except Exception as error:
                    reply = {"ok": False, "code": "ATTACH_FAILED", "note": str(error)}
            elif op == "status":
                reply = {"ok": True, "note": link.note, "port": link.port_name, "log": str(LOG)}
            elif op == "stop":
                link.release("stopped")
                reply = {"ok": True, "note": "stopped"}
            else:
                reply = {"ok": False, "code": "UNKNOWN_OP"}
        except Exception as error:
            reply = {"ok": False, "code": "ERROR", "note": str(error)}
            op = None
        try:
            conn.sendall(json.dumps(reply).encode() + b"\n")
        finally:
            conn.close()
        if op == "stop":
            SOCK.unlink(missing_ok=True)
            return


# ---- client ---------------------------------------------------------------

def call(message, timeout=40):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(timeout)
    client.connect(str(SOCK))
    client.sendall(json.dumps(message).encode() + b"\n")
    reply = json.loads(client.makefile("r").readline())
    client.close()
    return reply


def dev(request, check=True):
    try:
        reply = call({"op": "dev", "request": request})
    except OSError:
        sys.exit("not running: start `valetdev.py serve` first")
    if check and not reply.get("ok"):
        sys.exit(f"{request['cmd']} failed: {reply.get('code')} {reply.get('note', '')}".rstrip())
    return reply


def settle(timeout=20.0, quiet=0.6):
    """Wait until the UI has stopped submitting frames and the panel is idle."""
    deadline, last, since = time.time() + timeout, None, time.time()
    while time.time() < deadline:
        info = dev({"cmd": "info"})
        state = (info["epd_submitted"], info["epd_busy"], info["screen"])
        if state != last or info["epd_busy"]:
            last, since = state, time.time()
        elif time.time() - since >= quiet:
            return info
        time.sleep(0.15)
    return dev({"cmd": "info"})


def png(path, width, height, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\0" + row for row in rows)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def shot(path, wait=True):
    info = settle() if wait else None
    reply = dev({"cmd": "shot"})
    frame = zlib.decompress(base64.b64decode(reply["data"]))
    if len(frame) != reply["raw"] or zlib.crc32(frame) != reply["crc"]:
        sys.exit("screenshot damaged in transit; try again")
    fb_w, fb_h = reply["w"], reply["h"]
    stride = fb_w // 2
    # Native frame: landscape, 4 bpp, low nibble first. Upright portrait pixel
    # (x, y) is native (y, fb_h - 1 - x).
    grey = bytearray(fb_w * fb_h)
    grey[0::2] = bytes((b & 0x0F) * 17 for b in frame)
    grey[1::2] = bytes((b >> 4) * 17 for b in frame)
    assert stride * fb_h == len(frame)
    rows = []
    for y in range(fb_w):
        column = grey[y::fb_w]          # native column y, top to bottom
        rows.append(bytes(column[::-1]))
    png(path, fb_h, fb_w, rows)
    return info or dev({"cmd": "info"})


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("serve"); p.add_argument("--port")
    sub.add_parser("info"); sub.add_parser("status"); sub.add_parser("stop")
    sub.add_parser("release"); sub.add_parser("attach"); sub.add_parser("home"); sub.add_parser("settle")
    p = sub.add_parser("preview"); p.add_argument("name", choices=["standby"])
    p = sub.add_parser("shot"); p.add_argument("path"); p.add_argument("--now", action="store_true")
    p = sub.add_parser("tap"); p.add_argument("x", type=int); p.add_argument("y", type=int)
    p.add_argument("--ms", type=int, default=100)
    p = sub.add_parser("swipe")
    for name in ("x", "y", "x2", "y2"):
        p.add_argument(name, type=int)
    p.add_argument("--ms", type=int, default=400)
    p = sub.add_parser("key"); p.add_argument("name"); p.add_argument("--ms", type=int, default=100)
    p = sub.add_parser("log"); p.add_argument("-n", type=int, default=60); p.add_argument("--grep")
    p = sub.add_parser("sdlog"); p.add_argument("path")
    p = sub.add_parser("assistant-url"); p.add_argument("url")
    p = sub.add_parser("livebars"); p.add_argument("state", choices=["on", "off"])
    p = sub.add_parser("micgate"); p.add_argument("state", choices=["on", "off"])
    p = sub.add_parser("voiceprobe", help="play <url>/play.raw, record, upload mic/ref to <url>/upload"); p.add_argument("url")
    p = sub.add_parser("audioprobe", help="measure speaker-to-mic delay (home screen, idle)"); p.add_argument("--stall", type=int, default=0); p.add_argument("--amp", type=int, default=0)
    sub.add_parser("droplink", help="close the link to the assistant (test call reconnection)")
    p = sub.add_parser("booktool", help='run a book tool on the open book: booktool \'{"op":"search","query":"宋万"}\''); p.add_argument("request")
    p = sub.add_parser("wifi", help="save a Wi-Fi network as preferred and reconnect"); p.add_argument("ssid"); p.add_argument("password")
    p = sub.add_parser("wifitx", help="Wi-Fi max tx power in 0.25 dBm (48 = 12 dBm, 80 = 20 dBm)"); p.add_argument("q", type=int)
    sub.add_parser("proxytest", help="run the proxy settings test with the current proxy")
    p = sub.add_parser("reach", help="connect directly (no proxy) to host:port ...; 'proxy' = the configured proxy server")
    p.add_argument("hosts", nargs="+")
    p = sub.add_parser("sd", help="SD card state; --mount retries mounting"); p.add_argument("--mount", action="store_true")
    p = sub.add_parser("ask", help="send TEXT instead of the recording at the next hold-to-talk (no speaking needed)"); p.add_argument("text")
    p = sub.add_parser("proxy", help="set the proxy: proxy http 192.168.0.10 6152 --wifi; proxy off")
    p.add_argument("kind", choices=["http", "socks5", "link", "off"]); p.add_argument("host", nargs="?"); p.add_argument("port", nargs="?", type=int)
    p.add_argument("--wifi", action="store_true"); p.add_argument("--cell", action="store_true")
    p.add_argument("--link-file", help="file holding a whole proxy link (ss://… keeps the password off the command line)")
    p = sub.add_parser("flash"); p.add_argument("--image", default=str(APP))
    args = parser.parse_args()

    if args.command == "serve":
        return serve(args)
    if args.command in ("status", "stop", "release", "attach"):
        try:
            print(json.dumps(call({"op": args.command}), ensure_ascii=False))
        except OSError:
            sys.exit("not running")
    elif args.command == "info":
        print(json.dumps(dev({"cmd": "info"}), ensure_ascii=False))
    elif args.command == "settle":
        print(json.dumps(settle(), ensure_ascii=False))
    elif args.command == "shot":
        info = shot(args.path, wait=not args.now)
        print(f"{args.path}  screen={info['screen']} orient={info['orient']}")
    elif args.command in ("tap", "swipe"):
        request = {"cmd": args.command, "x": args.x, "y": args.y, "ms": args.ms}
        if args.command == "swipe":
            request.update(x2=args.x2, y2=args.y2)
        dev(request)
        time.sleep(args.ms / 1000 + 0.05)
    elif args.command == "key":
        dev({"cmd": "key", "name": args.name, "ms": args.ms})
        time.sleep(args.ms / 1000 + 0.05)
    elif args.command == "home":
        dev({"cmd": "home"})
    elif args.command == "preview":
        dev({"cmd": "preview", "name": args.name})
    elif args.command == "log":
        lines = LOG.read_bytes().decode("utf-8", "replace").splitlines() if LOG.exists() else []
        if args.grep:
            lines = [line for line in lines if re.search(args.grep, line)]
        for line in lines[-args.n:]:
            print(SECRET.sub(r"\1\2***", line))
    elif args.command == "proxy":
        req = {"cmd": "proxy", "wifi": args.wifi and args.kind != "off", "cell": args.cell and args.kind != "off"}
        if args.kind == "link":
            req["link"] = open(args.link_file).read().strip()
        elif args.kind != "off": req.update({"kind": args.kind, "host": args.host or "", "port": args.port or 0})
        dev(req)
    elif args.command == "voiceprobe":
        dev({"cmd": "voiceprobe", "url": args.url})
    elif args.command == "audioprobe":
        dev({"cmd": "audioprobe", "stall": args.stall, "amp": args.amp})
    elif args.command == "droplink":
        dev({"cmd": "droplink"})
    elif args.command == "booktool":
        dev({"cmd": "booktool", "request": json.loads(args.request)})
    elif args.command == "wifi":
        dev({"cmd": "wifi", "ssid": args.ssid, "password": args.password})
    elif args.command == "wifitx":
        dev({"cmd": "wifitx", "q": args.q})
    elif args.command == "proxytest":
        dev({"cmd": "proxytest"})
    elif args.command == "reach":
        dev({"cmd": "reach", "hosts": args.hosts})
    elif args.command == "sd":
        dev({"cmd": "sd", "mount": args.mount})
    elif args.command == "ask":
        dev({"cmd": "ask", "text": args.text})
    elif args.command == "micgate":
        dev({"cmd": "micgate", "on": args.state == "on"})
    elif args.command == "livebars":
        dev({"cmd": "livebars", "on": args.state == "on"})
    elif args.command == "assistant-url":
        dev({"cmd": "assistant_url", "url": args.url})
    elif args.command == "sdlog":
        reply = dev({"cmd": "sdlog"})
        data = zlib.decompress(base64.b64decode(reply["data"]))
        if len(data) != reply["raw"] or zlib.crc32(data) != reply["crc"]:
            sys.exit("log damaged in transit; try again")
        # The file is circular: the oldest bytes follow the newest. Keep the bytes as they are on the card.
        Path(args.path).write_bytes(data)
        print(f"{args.path}  {len(data)} bytes")
    elif args.command == "flash":
        image = Path(args.image)
        if not image.exists():
            sys.exit(f"{image} not found; run tools/dev/build.sh")
        try:
            status = call({"op": "status"})
            call({"op": "release"})
        except OSError:
            sys.exit("not running: start `valetdev.py serve` first")
        time.sleep(0.5)
        # App slot only: NVS, fonts, resources and the official image in ota_1 are not written.
        result = subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s31", "--port", status["port"],
                                 "--before", "usb-reset", "--after", "hard-reset", "--no-stub",
                                 "write-flash", hex(OTA0), str(image)])
        # Stay off the port until the app is up: bytes sent while it boots can leave the chip's USB
        # receive path out of step (see TakeStrandedUsbBytes in panel_vcom.cc).
        time.sleep(15.0)
        print(json.dumps(call({"op": "attach"}), ensure_ascii=False))
        if result.returncode:
            sys.exit("flash failed; the port was reattached")


if __name__ == "__main__":
    main()
