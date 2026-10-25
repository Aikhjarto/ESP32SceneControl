#!/usr/bin/env python3
"""Serve the signed firmware for over-the-air updates.

    python3 tools/serve_firmware.py [--port 9999] [--env esp32dev] [--file path]

Join the ESP's access point with this computer, start this server, then press
"Update from server" in the web UI (Settings → Firmware) or send
$Firmware/Update over serial. The ESP downloads http://<this computer>:9999/firmware.bin.

Serves .pio/build/<env>/firmware-signed.bin (written by every `pio run` when the
signing key is present). The file is re-read on every request, so rebuilding is
enough; no restart needed. Your firewall must allow incoming TCP on the port.
"""
import argparse
import http.server
import os
import pathlib
import socket
import struct
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
SIG_BLOCK = 68
DESC_OFFSET = 24 + 8  # esp_image_header_t + first segment header


def describe(data):
    """Version, project and build time from the esp_app_desc_t, or None."""
    d = data[DESC_OFFSET:DESC_OFFSET + 256]
    if len(d) < 144 or struct.unpack_from("<I", d, 0)[0] != 0xABCD5432:
        return None
    text = lambda a, b: d[a:b].split(b"\0")[0].decode(errors="replace")
    return {"version": text(16, 48), "project": text(48, 80), "built": f"{text(96, 112)} {text(80, 96)}"}


def local_addresses():
    addrs = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.add(info[4][0])
    except OSError:
        pass
    try:  # address used to reach the ESP's subnet
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.168.4.1", 80))
            addrs.add(s.getsockname()[0])
    except OSError:
        pass
    return sorted(a for a in addrs if not a.startswith("127."))


class Handler(http.server.BaseHTTPRequestHandler):
    firmware: pathlib.Path
    unsigned: pathlib.Path

    def do_GET(self):
        if self.path.split("?")[0] != "/firmware.bin":
            self.send_error(404, "only /firmware.bin is served here")
            return
        if not self.firmware.exists():
            self.send_error(404, f"{self.firmware} not found - build first (pio run) with the signing key present")
            return
        if self.unsigned.exists() and self.unsigned.stat().st_mtime > self.firmware.stat().st_mtime + 1:
            self.send_error(409, "firmware-signed.bin is older than firmware.bin - signing failed, see the build log")
            return
        data = self.firmware.read_bytes()
        info = describe(data) or {}
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        start = time.monotonic()
        self.wfile.write(data)
        print(f"  sent {len(data)} bytes, version {info.get('version', '?')}, to {self.client_address[0]} "
              f"in {time.monotonic() - start:.1f} s", flush=True)

    def log_message(self, fmt, *args):
        print(f"{self.client_address[0]} - {fmt % args}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=9999)
    ap.add_argument("--env", default="esp32dev", help="PlatformIO environment (default: esp32dev)")
    ap.add_argument("--file", type=pathlib.Path, help="serve this signed image instead")
    args = ap.parse_args()

    build = ROOT / ".pio" / "build" / args.env
    Handler.firmware = args.file or build / "firmware-signed.bin"
    Handler.unsigned = build / "firmware.bin" if not args.file else pathlib.Path("/nonexistent")

    if Handler.firmware.exists():
        info = describe(Handler.firmware.read_bytes()) or {}
        print(f"Firmware: {Handler.firmware.relative_to(ROOT) if not args.file else Handler.firmware}")
        print(f"  {info.get('project', '?')} {info.get('version', '?')}, built {info.get('built', '?')}, "
              f"{Handler.firmware.stat().st_size} bytes")
    else:
        print(f"Warning: {Handler.firmware} does not exist yet - build first.", file=sys.stderr)

    server = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    for a in local_addresses() or ["<this computer>"]:
        print(f"  http://{a}:{args.port}/firmware.bin")
    print(f"Serving on port {args.port} (Ctrl-C to stop)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    os.chdir(ROOT)
    main()
