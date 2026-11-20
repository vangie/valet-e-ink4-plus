#!/usr/bin/env python3
"""A small proxy for checking the firmware's proxy support on the desk.

One port speaks both kinds, told apart by the first byte: SOCKS5 (CONNECT,
with or without a login) and HTTP (CONNECT tunnels, and plain http requests
with an absolute address). Every request is printed as one line, so a test
can see what the device asked for:

    testproxy.py --port 8899                      # open to all
    testproxy.py --port 8899 --login user:secret  # asks for this login

    12:00:01 socks5 CONNECT github.com:443 ok
    12:00:02 http   CONNECT github.com:443 ok
    12:00:03 http   GET http://example.com/ ok
    12:00:04 http   CONNECT github.com:443 refused: login

Only names and ports are printed; nothing that passes through the tunnel is.
Standard library only. It listens on every interface: run it on a network
you trust and stop it when the test is done.
"""
import argparse
import asyncio
import base64
import time
from urllib.parse import urlsplit


def say(kind, what, result):
    print(f"{time.strftime('%H:%M:%S')} {kind:<6} {what} {result}", flush=True)


async def relay(reader, writer):
    try:
        while data := await reader.read(65536):
            writer.write(data)
            await writer.drain()
    except (ConnectionError, OSError):
        pass
    finally:
        try:
            writer.close()
        except OSError:
            pass


async def join(a_reader, a_writer, b_reader, b_writer):
    await asyncio.gather(relay(a_reader, b_writer), relay(b_reader, a_writer))


async def socks5(reader, writer, login):
    count = (await reader.readexactly(1))[0]
    methods = await reader.readexactly(count)
    if login:
        if 2 not in methods:
            writer.write(b"\x05\xff")
            return say("socks5", "-", "refused: no login offered")
        writer.write(b"\x05\x02")
        await reader.readexactly(1)
        user = await reader.readexactly((await reader.readexactly(1))[0])
        password = await reader.readexactly((await reader.readexactly(1))[0])
        ok = f"{user.decode(errors='replace')}:{password.decode(errors='replace')}" == login
        writer.write(b"\x01" + (b"\x00" if ok else b"\x01"))
        if not ok:
            return say("socks5", "-", "refused: login")
    else:
        writer.write(b"\x05\x00")
    _, command, _, kind = await reader.readexactly(4)
    if kind == 1:
        host = ".".join(str(b) for b in await reader.readexactly(4))
    elif kind == 3:
        host = (await reader.readexactly((await reader.readexactly(1))[0])).decode(errors="replace")
    else:
        writer.write(b"\x05\x08\x00\x01" + bytes(6))
        return say("socks5", "-", "refused: address type")
    port = int.from_bytes(await reader.readexactly(2), "big")
    target = f"CONNECT {host}:{port}"
    if command != 1:
        writer.write(b"\x05\x07\x00\x01" + bytes(6))
        return say("socks5", target, "refused: command")
    try:
        up_reader, up_writer = await asyncio.wait_for(asyncio.open_connection(host, port), 15)
    except (OSError, asyncio.TimeoutError) as error:
        writer.write(b"\x05\x05\x00\x01" + bytes(6))
        return say("socks5", target, f"failed: {type(error).__name__}")
    writer.write(b"\x05\x00\x00\x01" + bytes(6))
    say("socks5", target, "ok")
    await join(reader, writer, up_reader, up_writer)


async def http(first, reader, writer, login):
    head = first + await reader.readuntil(b"\r\n\r\n")
    lines = head.decode("latin1").split("\r\n")
    try:
        method, target, _ = lines[0].split(" ", 2)
    except ValueError:
        writer.write(b"HTTP/1.1 400 Bad Request\r\n\r\n")
        return say("http", "-", "refused: not a request")
    what = f"{method} {target.split('?')[0]}"
    headers = {k.lower(): v for k, v in (line.split(": ", 1) for line in lines[1:] if ": " in line)}
    if login and headers.get("proxy-authorization") != "Basic " + base64.b64encode(login.encode()).decode():
        writer.write(b"HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"test\"\r\nContent-Length: 0\r\n\r\n")
        return say("http", what, "refused: login")
    if method == "CONNECT":
        host, _, port = target.rpartition(":")
        try:
            up_reader, up_writer = await asyncio.wait_for(asyncio.open_connection(host, int(port)), 15)
        except (OSError, ValueError, asyncio.TimeoutError) as error:
            writer.write(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")
            return say("http", what, f"failed: {type(error).__name__}")
        writer.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
        say("http", what, "ok")
        return await join(reader, writer, up_reader, up_writer)
    url = urlsplit(target)
    if url.scheme != "http" or not url.hostname:
        writer.write(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
        return say("http", what, "refused: not an absolute http address")
    try:
        up_reader, up_writer = await asyncio.wait_for(asyncio.open_connection(url.hostname, url.port or 80), 15)
    except (OSError, asyncio.TimeoutError) as error:
        writer.write(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")
        return say("http", what, f"failed: {type(error).__name__}")
    path = (url.path or "/") + ("?" + url.query if url.query else "")
    kept = [line for line in lines[1:] if line and not line.lower().startswith(("proxy-authorization:", "proxy-connection:"))]
    up_writer.write(("\r\n".join([f"{method} {path} HTTP/1.1"] + kept) + "\r\n\r\n").encode("latin1"))
    say("http", what, "ok")
    await join(reader, writer, up_reader, up_writer)


async def serve(args):
    async def handle(reader, writer):
        try:
            first = await reader.readexactly(1)
            if first == b"\x05":
                await socks5(reader, writer, args.login)
            else:
                await http(first, reader, writer, args.login)
            await writer.drain()
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError, OSError):
            pass
        finally:
            try:
                writer.close()
            except OSError:
                pass

    server = await asyncio.start_server(handle, "0.0.0.0", args.port)
    print(f"proxy on port {args.port} (HTTP and SOCKS5), {'login required' if args.login else 'open to all'}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", type=int, default=8899)
    parser.add_argument("--login", help="user:password the proxy asks for")
    try:
        asyncio.run(serve(parser.parse_args()))
    except KeyboardInterrupt:
        pass
