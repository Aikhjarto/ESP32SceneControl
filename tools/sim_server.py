#!/usr/bin/env python3
"""Serve the web UI against the desktop simulator (no ESP32 needed).

    python3 tools/sim_server.py [--port 8080]

Builds tools/sim/main.cpp together with src/core (g++), then serves web/ on
http://localhost:8080/ with the same /ws protocol and /config.txt endpoints as
the firmware. Saved settings go to .pio/sim/sim-config.txt (see --config).
"""
import argparse
import asyncio
import os
import pathlib
import subprocess
import tempfile

from aiohttp import WSMsgType, web

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / ".pio" / "sim"
REALTIME = "!~?\x18"


def build_sim():
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / "scenecontrol-sim"
    sources = [ROOT / "tools/sim/main.cpp", *sorted((ROOT / "src/core").glob("*.cpp"))]
    if not exe.exists() or any(s.stat().st_mtime > exe.stat().st_mtime for s in sources):
        print("building simulator ...")
        subprocess.run(["g++", "-std=gnu++17", "-O2", "-Wall", "-I", str(ROOT / "src"), "-o", str(exe),
                        *map(str, sources)], check=True)
    return exe


class Sim:
    def __init__(self, exe, config):
        self.exe = exe
        self.config = config
        self.lock = asyncio.Lock()
        self.proc = None

    async def start(self):
        self.proc = await asyncio.create_subprocess_exec(
            str(self.exe), str(self.config),
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE)
        await self.request("S")  # swallow boot output

    async def request(self, line):
        async with self.lock:
            self.proc.stdin.write((line + "\n").encode())
            await self.proc.stdin.drain()
            out = []
            while True:
                l = (await self.proc.stdout.readline()).decode().rstrip("\n")
                if l == ".":
                    return "".join(x + "\n" for x in out)
                out.append(l)


async def ws_handler(request):
    sim, clients = request.app["sim"], request.app["clients"]
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    clients.add(ws)
    try:
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                continue
            text = msg.data
            if len(text) == 1 and text in REALTIME:
                reply = await sim.request(f"R {ord(text)}")
                if reply:
                    await ws.send_str(reply)
                continue
            for line in text.split("\n"):
                line = line.rstrip("\r")
                if line:
                    await ws.send_str(await sim.request("L " + line))
    finally:
        clients.discard(ws)
    return ws


async def status_pusher(app):
    while True:
        await asyncio.sleep(0.2)
        if app["clients"]:
            text = await app["sim"].request("S")
            for ws in list(app["clients"]):
                try:
                    await ws.send_str(text)
                except ConnectionError:
                    pass


async def config_get(request):
    text = await request.app["sim"].request("D")
    return web.Response(text=text, content_type="text/plain")


async def config_post(request):
    body = await request.text()
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(body)
    try:
        text = await request.app["sim"].request("P " + f.name)
    finally:
        os.unlink(f.name)
    status = 200 if text.rstrip().endswith("ok") else 400
    return web.Response(text=text, status=status, content_type="text/plain")


async def update_post(request):
    await request.read()
    return web.Response(status=501, text="Firmware upload needs the real ESP32 (this is the simulator)")


def static(name, ctype):
    async def handler(_):
        return web.Response(body=(ROOT / "web" / name).read_bytes(), content_type=ctype,
                            headers={"Cache-Control": "no-cache"})
    return handler


async def on_startup(app):
    await app["sim"].start()
    app["pusher"] = asyncio.create_task(status_pusher(app))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--config", type=pathlib.Path, default=BUILD / "sim-config.txt",
                    help="file the simulator saves its settings to (default: .pio/sim/sim-config.txt)")
    args = ap.parse_args()

    app = web.Application()
    app["sim"] = Sim(build_sim(), args.config)
    app["clients"] = set()
    app.on_startup.append(on_startup)
    app.router.add_get("/", static("index.html", "text/html"))
    app.router.add_get("/app.js", static("app.js", "application/javascript"))
    app.router.add_get("/style.css", static("style.css", "text/css"))
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/config.txt", config_get)
    app.router.add_post("/config.txt", config_post)
    app.router.add_post("/update", update_post)
    web.run_app(app, port=args.port)


if __name__ == "__main__":
    main()
