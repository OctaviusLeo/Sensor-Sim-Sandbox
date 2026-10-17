#!/usr/bin/env python3
"""Minimal stub rosbridge-style WebSocket server (task 11 live verification).

Stands in for the real rosbridge_server so the Rosbridge_Client advertise+publish
round-trip can be exercised WITHOUT ROS/WSL. Implements just enough of RFC 6455
to:

  * complete the opening HTTP-101 handshake for ANY request path (so UE's
    bundled libwebsockets `GET //` double-slash request is accepted, matching
    the M0 finding), and
  * read client text frames, parse each rosbridge v2 op JSON, and record every
    received op (advertise / publish) to an output file.

It uses ONLY the Python standard library (no `websockets` dependency), so it runs
on a bare `py -3.12` with no venv. It is a VERIFICATION aid, not production code:
single connection at a time, text frames only, no TLS, bound to localhost.

Usage:
    python scripts/stub_ws_server.py --host 127.0.0.1 --port 9091 \
        --out runs/logs/bridge/stub_received.jsonl [--seconds 60]

Each received op is appended to --out as one JSON line:
    {"recv_t": <epoch>, "op": "publish", "topic": "/bridge_test", "raw": "<frame text>"}
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import socket
import struct
import sys
import time

# RFC 6455 GUID appended to the client's Sec-WebSocket-Key before hashing.
_WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _accept_key(client_key: str) -> str:
    digest = hashlib.sha1((client_key + _WS_GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def _read_http_headers(conn: socket.socket) -> dict[str, str]:
    """Read the HTTP request up to the blank line; return a header map.

    Also logs the request line (e.g. 'GET // HTTP/1.1') so the double-slash path
    UE emits is visible in the server output.
    """
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            break
        data += chunk
        if len(data) > 65536:
            break
    text = data.decode("latin-1", errors="replace")
    lines = text.split("\r\n")
    if lines:
        print(f"[stub_ws] request line: {lines[0]!r}", flush=True)
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    return headers


def _handshake(conn: socket.socket) -> bool:
    headers = _read_http_headers(conn)
    key = headers.get("sec-websocket-key")
    if not key:
        conn.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
        print("[stub_ws] no Sec-WebSocket-Key; rejected", flush=True)
        return False
    resp = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {_accept_key(key)}\r\n"
        "\r\n"
    )
    conn.sendall(resp.encode("ascii"))
    print("[stub_ws] sent HTTP 101 Switching Protocols", flush=True)
    return True


def _recv_exact(conn: socket.socket, n: int) -> bytes | None:
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def _read_frame(conn: socket.socket) -> tuple[int, bytes] | None:
    """Read one RFC 6455 frame. Returns (opcode, payload) or None on close/EOF."""
    hdr = _recv_exact(conn, 2)
    if hdr is None:
        return None
    b0, b1 = hdr[0], hdr[1]
    opcode = b0 & 0x0F
    masked = (b1 & 0x80) != 0
    length = b1 & 0x7F
    if length == 126:
        ext = _recv_exact(conn, 2)
        if ext is None:
            return None
        length = struct.unpack("!H", ext)[0]
    elif length == 127:
        ext = _recv_exact(conn, 8)
        if ext is None:
            return None
        length = struct.unpack("!Q", ext)[0]
    mask = b""
    if masked:
        mask = _recv_exact(conn, 4)
        if mask is None:
            return None
    payload = _recv_exact(conn, length) if length else b""
    if payload is None:
        return None
    if masked and mask:
        payload = bytes(payload[i] ^ mask[i % 4] for i in range(len(payload)))
    return opcode, payload


def serve(host: str, port: int, out_path: str, seconds: float) -> int:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    srv.settimeout(1.0)
    print(f"[stub_ws] listening on ws://{host}:{port} (any path), out={out_path}", flush=True)

    deadline = time.time() + seconds if seconds > 0 else None
    received = 0

    with open(out_path, "a", encoding="utf-8") as out:
        while True:
            if deadline is not None and time.time() > deadline:
                print(f"[stub_ws] time limit reached; received {received} op(s)", flush=True)
                break
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            print(f"[stub_ws] connection from {addr}", flush=True)
            conn.settimeout(1.0)
            try:
                if not _handshake(conn):
                    conn.close()
                    continue
                while True:
                    if deadline is not None and time.time() > deadline:
                        break
                    try:
                        frame = _read_frame(conn)
                    except socket.timeout:
                        continue
                    if frame is None:
                        print("[stub_ws] client closed connection", flush=True)
                        break
                    opcode, payload = frame
                    if opcode == 0x8:  # close
                        print("[stub_ws] received close frame", flush=True)
                        break
                    if opcode in (0x1, 0x2):  # text or binary
                        text = payload.decode("utf-8", errors="replace")
                        op = "?"
                        topic = "?"
                        try:
                            parsed = json.loads(text)
                            op = parsed.get("op", "?")
                            topic = parsed.get("topic", "?")
                        except json.JSONDecodeError:
                            pass
                        record = {"recv_t": time.time(), "op": op, "topic": topic, "raw": text}
                        out.write(json.dumps(record) + "\n")
                        out.flush()
                        received += 1
                        print(f"[stub_ws] received op={op} topic={topic} (#{received})", flush=True)
            finally:
                conn.close()

    srv.close()
    print(f"[stub_ws] done; total received {received} op(s)", flush=True)
    return 0 if received > 0 else 2


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Minimal stub rosbridge WebSocket server")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9091)
    ap.add_argument("--out", default="runs/logs/bridge/stub_received.jsonl")
    ap.add_argument("--seconds", type=float, default=60.0,
                    help="serve for this many seconds then exit (0 = forever)")
    args = ap.parse_args(argv)
    return serve(args.host, args.port, args.out, args.seconds)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
