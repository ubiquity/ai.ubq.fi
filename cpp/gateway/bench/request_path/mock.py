#!/usr/bin/env python3
"""One keep-alive HTTP/1.1 provider fixture per benchmark invocation."""

from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sys
import threading
from urllib.parse import urlsplit

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fixtures import (BUFFERED, CHAT_TEXT, LARGE_MESSAGES, MODEL, PROVIDER_TOKEN, RESPONSES_TEXT, SSE_TERMINAL, SSE_TEXT, encode)


class Mock(ThreadingHTTPServer):
    request_queue_size = 64

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.condition = threading.Condition()
        self.calls = Counter()
        self.wire_sizes = {}
        self.failures = 0
        self.hold = False
        self.sent = 0
        self.released = 0

    def record(self, scenario, size):
        with self.condition:
            self.calls[scenario] += 1
            low, high = self.wire_sizes.get(scenario, (size, size))
            self.wire_sizes[scenario] = (min(low, size), max(high, size))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    disable_nagle_algorithm = True
    timeout = 30

    def log_message(self, *_):
        pass

    def respond(self, status, body=b"{}"):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        owner = self.server
        path = urlsplit(self.path).path
        with owner.condition:
            if path == "/fixture/counts":
                self.respond(200, encode({"calls": owner.calls, "wire_sizes": owner.wire_sizes, "failures": owner.failures}))
            elif path == "/fixture/hold" and not owner.hold:
                owner.hold, owner.sent, owner.released = True, 0, 0
                self.respond(200)
            elif path == "/fixture/release" and owner.hold and owner.sent == owner.released + 1:
                owner.released += 1
                owner.condition.notify_all()
                self.respond(200)
            else:
                owner.failures += 1
                self.respond(404)

    def do_POST(self):
        owner = self.server
        try:
            size = int(self.headers.get("Content-Length", 0))
            if self.path != "/chat/completions" or not 0 < size <= 2 * 1024 * 1024:
                raise ValueError("Unexpected mock route or content length")
            if self.headers.get("Authorization") != f"Bearer {PROVIDER_TOKEN}":
                raise ValueError("Expected the synthetic upstream credential")
            body = json.loads(self.rfile.read(size))
            messages = body["messages"]
            if body["model"] != MODEL or not messages:
                raise ValueError("Unexpected model or missing messages")
            if size > 256 * 1024:
                if [{"role": item["role"], "content": item["content"]} for item in messages] != LARGE_MESSAGES:
                    raise ValueError("Large request text was changed before reaching upstream")
                scenario = "large"
            elif messages[-1]["content"] == RESPONSES_TEXT:
                scenario = "responses"
            elif messages[-1]["content"] == CHAT_TEXT:
                scenario = "stream" if body.get("stream") is True else "small"
            else:
                raise ValueError("Unrecognized synthetic prompt")
            if (body.get("stream") is True) != (scenario == "stream"):
                raise ValueError("Unrecognized synthetic stream mode")
            owner.record(scenario, size)
            if scenario == "stream":
                self.stream()
            else:
                self.respond(200, BUFFERED)
        except (OSError, ValueError, KeyError, TypeError) as error:
            with owner.condition:
                owner.failures += 1
            print(f"Synthetic request failed: {error}", file=sys.stderr)
            self.close_connection = True
            try:
                self.respond(500)
            except OSError:
                pass

    def stream(self):
        owner = self.server
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        if owner.hold:
            for frame in SSE_TEXT:
                with owner.condition:
                    owner.sent += 1
                    self.chunk(frame)
                    if not owner.condition.wait_for(lambda: owner.released == owner.sent, timeout=10):
                        raise ValueError("Gateway failed to forward one SSE delta before the next upstream frame")
            with owner.condition:
                owner.hold = False
        else:
            for frame in SSE_TEXT:
                self.chunk(frame)
        for frame in SSE_TERMINAL:
            self.chunk(frame)
        self.wfile.write(b"0\r\n\r\n")

    def chunk(self, frame):
        self.wfile.write(f"{len(frame):x}\r\n".encode() + frame + b"\r\n")
        self.wfile.flush()


if __name__ == "__main__":
    with Mock() as mock:
        if mock.server_port in (7999, 8000):
            raise RuntimeError("Fixture was assigned a reserved port")
        print(mock.server_port, flush=True)
        mock.serve_forever()
