"""Bounded real HTTP/SSE and RFC6455 client probes using the Python standard library."""

import base64
import hashlib
import http.client
import json
import os
import socket
import struct
import time
from urllib.parse import urlsplit

MAX_BODY = 4 * 1024 * 1024


def endpoint(value):
    parsed = urlsplit(value)
    if parsed.scheme != "http" or parsed.hostname != "127.0.0.1" or not parsed.port or parsed.port in (7999, 8000):
        raise RuntimeError("Smoke target must be exact loopback HTTP on a non-prohibited assigned port")
    if parsed.username or parsed.password or parsed.query or parsed.fragment or parsed.path not in ("", "/"):
        raise RuntimeError("Smoke origin must contain no credentials, path or suffix")
    return parsed.hostname, parsed.port


def request(origin, path, token=None, body=None, method=None, on_delta=None, content_type=None, timeout=15):
    host, port = endpoint(origin)
    if not path.startswith("/"):
        raise RuntimeError("Smoke request needs an absolute path")
    payload = body if isinstance(body, bytes) else json.dumps(body, separators=(",", ":")).encode() if body is not None else None
    headers = {"Connection": "close"}
    if token:
        headers["Authorization"] = "Bearer " + token
    if payload is not None:
        headers["Content-Type"] = content_type or "application/json"
    started = time.perf_counter_ns()
    connection = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        connection.request(method or ("POST" if payload is not None else "GET"), path, payload, headers)
        response = connection.getresponse()
        head_at = time.perf_counter_ns()
        chunks = []
        size = 0
        buffer = b""
        events = []
        acknowledged = False
        streamed = "text/event-stream" in response.getheader("content-type", "")
        while True:
            chunk = response.read1(65536)
            if not chunk:
                break
            size += len(chunk)
            if size > MAX_BODY:
                raise RuntimeError("Smoke client response exceeded 4 MiB")
            now = time.perf_counter_ns()
            chunks.append({"at_us": (now - started) / 1000, "base64": base64.b64encode(chunk).decode()})
            buffer += chunk
            if streamed:
                normalized = buffer.replace(b"\r\n", b"\n")
                while b"\n\n" in normalized:
                    frame, normalized = normalized.split(b"\n\n", 1)
                    lines = [line[5:].lstrip() for line in frame.split(b"\n") if line.startswith(b"data:")]
                    data = b"\n".join(lines)
                    if not data:
                        continue
                    event = "[DONE]" if data == b"[DONE]" else json.loads(data)
                    events.append({"at_us": (now - started) / 1000, "data": event})
                    if on_delta and not acknowledged and is_delta(event):
                        acknowledged = True
                        on_delta(event)
                buffer = normalized
        finished = time.perf_counter_ns()
        raw = b"".join(base64.b64decode(item["base64"]) for item in chunks)
        result = {"path": path, "method": method or ("POST" if payload is not None else "GET"),
                  "status": response.status, "headers": {key.lower(): value for key, value in response.getheaders()},
                  "body_bytes": len(raw), "body_sha256": hashlib.sha256(raw).hexdigest(), "chunks": chunks,
                  "events": events, "headers_us": (head_at - started) / 1000, "elapsed_us": (finished - started) / 1000}
        if not streamed:
            result["json"] = json.loads(raw) if raw and response.getheader("content-type", "").startswith("application/json") else None
            if result["json"] is None:
                result["text"] = raw.decode("utf-8", errors="replace")
        return result
    finally:
        connection.close()


def is_delta(event):
    if not isinstance(event, dict):
        return False
    if event.get("type") in ("response.output_text.delta", "response.function_call_arguments.delta"):
        return bool(event.get("delta"))
    return any(choice.get("delta", {}).get("content") for choice in event.get("choices", []))


def _read_exact(connection, size):
    output = bytearray()
    while len(output) < size:
        chunk = connection.recv(size - len(output))
        if not chunk:
            raise RuntimeError("WebSocket closed before complete frame")
        output.extend(chunk)
    return bytes(output)


def send_frame(connection, opcode, payload, masked):
    payload = payload.encode() if isinstance(payload, str) else payload
    if len(payload) > MAX_BODY:
        raise RuntimeError("WebSocket fixture frame exceeded bound")
    flag = 0x80 if masked else 0
    head = bytes([0x80 | opcode])
    if len(payload) < 126:
        head += bytes([flag | len(payload)])
    elif len(payload) < 65536:
        head += bytes([flag | 126]) + struct.pack("!H", len(payload))
    else:
        head += bytes([flag | 127]) + struct.pack("!Q", len(payload))
    if masked:
        mask = os.urandom(4)
        head += mask
        payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    connection.sendall(head + payload)


def receive_frame(connection):
    head = _read_exact(connection, 2)
    opcode, masked, size = head[0] & 15, bool(head[1] & 128), head[1] & 127
    if size == 126:
        size = struct.unpack("!H", _read_exact(connection, 2))[0]
    elif size == 127:
        size = struct.unpack("!Q", _read_exact(connection, 8))[0]
    if size > MAX_BODY:
        raise RuntimeError("WebSocket received frame exceeded bound")
    mask = _read_exact(connection, 4) if masked else None
    data = _read_exact(connection, size)
    if mask:
        data = bytes(value ^ mask[index % 4] for index, value in enumerate(data))
    return opcode, data


def websocket(origin, path, token=None, relay=False):
    host, port = endpoint(origin)
    key = base64.b64encode(os.urandom(16)).decode()
    headers = ["GET " + path + " HTTP/1.1", f"Host: {host}:{port}", "Upgrade: websocket", "Connection: Upgrade",
               "Sec-WebSocket-Version: 13", "Sec-WebSocket-Key: " + key]
    if token:
        headers.append("Authorization: Bearer " + token)
    started = time.perf_counter_ns()
    with socket.create_connection((host, port), timeout=10) as connection:
        connection.settimeout(10)
        connection.sendall(("\r\n".join(headers) + "\r\n\r\n").encode())
        raw = bytearray()
        while not raw.endswith(b"\r\n\r\n"):
            raw.extend(_read_exact(connection, 1))
            if len(raw) > 65536:
                raise RuntimeError("WebSocket response head exceeded bound")
        lines = raw.decode("latin1").split("\r\n")
        status = int(lines[0].split()[1])
        result = {"path": path, "status": status, "head": lines, "elapsed_us": (time.perf_counter_ns() - started) / 1000}
        if status == 101:
            values = {name.lower(): value for line in lines[1:] if ": " in line for name, value in [line.split(": ", 1)]}
            expected = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            if values.get("sec-websocket-accept") != expected:
                raise RuntimeError("WebSocket accept digest did not match")
            if relay:
                sent = '{"type":"smoke.echo","value":"fixture-wire-ok"}'
                send_frame(connection, 1, sent, True)
                opcode, received = receive_frame(connection)
                result.update({"opcode": opcode, "sent": sent, "received": received.decode()})
                if opcode != 1 or received.decode() != sent:
                    raise RuntimeError("WebSocket relay changed the fixture text")
            send_frame(connection, 8, struct.pack("!H", 1000), True)
        return result
