"""Finite, scripted loopback upstream; no provider endpoint can be contacted here."""

import base64
import hashlib
import http.server
import json
import selectors
import socket
import threading
import time
from urllib.parse import urlsplit

from client import MAX_BODY, receive_frame, send_frame
from network import FORBIDDEN_PORTS

CATALOG_MODEL = {
    "slug": "gpt-smoke-fixture", "display_name": "Synthetic smoke model", "description": "Loopback fixture",
    "default_reasoning_level": "none", "supported_reasoning_levels": [{"effort": "none", "description": "Synthetic"}],
    "shell_type": "shell_command", "visibility": "list", "supported_in_api": True,
    "priority": 1, "availability_nux": None, "upgrade": None, "base_instructions": "Synthetic fixture.",
    "supports_reasoning_summaries": False, "support_verbosity": False, "default_verbosity": None,
    "apply_patch_tool_type": "freeform", "truncation_policy": {"mode": "bytes", "limit": 10000},
    "supports_parallel_tool_calls": True, "context_window": 32000, "max_context_window": 32000,
    "auto_compact_token_limit": 24000, "effective_context_window_percent": 95,
    "experimental_supported_tools": [], "input_modalities": ["text"],
}


class Upstream:
    def __init__(self, frames=4, delay_ms=0, maximum=4096):
        if not 1 <= frames <= 256 or not 0 <= delay_ms <= 100 or not 1 <= maximum <= 4096:
            raise RuntimeError("Upstream workload exceeds its finite bounds")
        self.frames = frames
        self.delay_ms = delay_ms
        self.maximum = maximum
        self.calls = []
        self.websocket_frames = []
        self.failures = []
        self.redirect_status = None
        self.redirected_requests = []
        self.lock = threading.Lock()
        self.hold = None
        self.hold_started = threading.Event()
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            disable_nagle_algorithm = True

            def log_message(self, *_):
                pass

            def json_response(self, status, value, extra=None):
                data = json.dumps(value, separators=(",", ":")).encode()
                self.send_response(status)
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(data)))
                self.send_header("connection", "close")
                for key, value in (extra or {}).items():
                    self.send_header(key, value)
                self.end_headers()
                self.wfile.write(data)
                self.wfile.flush()
                self.close_connection = True

            def do_GET(self):
                if self.headers.get("upgrade", "").lower() == "websocket" and urlsplit(self.path).path == "/live/rtc_fixture":
                    self.upgrade()
                elif urlsplit(self.path).path == "/codex/models":
                    self.json_response(200, {"models": [CATALOG_MODEL]})
                else:
                    owner.redirected_requests.append({"path": self.path, "method": "GET", "at_ns": time.perf_counter_ns()})
                    self.json_response(404, {"error": "unmapped synthetic route"})

            def upgrade(self):
                key = self.headers.get("sec-websocket-key", "")
                digest = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
                self.send_response(101)
                self.send_header("upgrade", "websocket")
                self.send_header("connection", "Upgrade")
                self.send_header("sec-websocket-accept", digest)
                self.end_headers()
                self.wfile.flush()
                self.connection.settimeout(10)
                try:
                    for _ in range(8):
                        opcode, data = receive_frame(self.connection)
                        owner.websocket_frames.append({"opcode": opcode, "base64": base64.b64encode(data).decode(),
                                                       "at_ns": time.perf_counter_ns(),
                                                       "header_names": sorted(name.lower() for name in self.headers)})
                        send_frame(self.connection, opcode, data, False)
                        if opcode == 8:
                            break
                except (OSError, RuntimeError) as error:
                    owner.failures.append({"path": self.path, "error": str(error)})
                self.close_connection = True

            def do_POST(self):
                try:
                    length = int(self.headers.get("content-length", "0"))
                    if not 0 < length <= MAX_BODY:
                        raise RuntimeError("Fixture request needs a bounded content-length")
                    raw = self.rfile.read(length)
                    body = json.loads(raw)
                    with owner.lock:
                        if len(owner.calls) >= owner.maximum:
                            raise RuntimeError("Scripted upstream dispatch bound exceeded")
                        call = {"path": self.path, "body": body, "body_sha256": hashlib.sha256(raw).hexdigest(),
                                "header_names": sorted(name.lower() for name in self.headers), "at_ns": time.perf_counter_ns()}
                        owner.calls.append(call)
                    if urlsplit(self.path).path == "/codex/realtime/calls":
                        answer = b"v=0\r\ns=synthetic-smoke-answer\r\n"
                        self.send_response(201)
                        self.send_header("content-type", "application/sdp")
                        self.send_header("content-length", str(len(answer)))
                        self.send_header("location", "/v1/live/rtc_fixture")
                        self.send_header("connection", "close")
                        self.end_headers()
                        self.wfile.write(answer)
                        self.wfile.flush()
                        self.close_connection = True
                    elif self.path == "/chat/completions":
                        with owner.lock:
                            redirect = owner.redirect_status
                            owner.redirect_status = None
                        if redirect:
                            call["response_status"] = redirect
                            self.send_response(redirect)
                            self.send_header("location", owner.origin + "/redirected-sink")
                            self.send_header("content-length", "0")
                            self.send_header("connection", "close")
                            self.end_headers()
                            self.wfile.flush()
                            self.close_connection = True
                        else:
                            self.chat(body, call)
                    else:
                        raise RuntimeError("Scripted upstream received an unmapped path")
                except Exception as error:
                    owner.failures.append({"path": self.path, "error": str(error)})
                    try:
                        self.json_response(500, {"error": "synthetic upstream failed", "detail": str(error)})
                    except OSError:
                        pass

            def chat(self, body, call):
                if body.get("stream") is not True:
                    self.json_response(200, {"id": "chatcmpl-smoke", "object": "chat.completion", "created": 1780000001,
                                            "model": "deepseek-flash", "choices": [{"index": 0, "message": {
                                                "role": "assistant", "content": "fixture-wire-ok"}, "finish_reason": "stop"}],
                                            "usage": {"prompt_tokens": 11, "completion_tokens": 5, "total_tokens": 16,
                                                      "prompt_cache_hit_tokens": 0}})
                    return
                self.send_response(200)
                self.send_header("content-type", "text/event-stream")
                self.send_header("cache-control", "no-cache")
                self.send_header("transfer-encoding", "chunked")
                self.send_header("x-request-id", "synthetic-provider-request")
                self.send_header("connection", "close")
                self.end_headers()
                chunks = []

                def frame(delta, finish=None, usage=None):
                    value = {"id": "chatcmpl-smoke", "object": "chat.completion.chunk", "created": 1780000001,
                             "model": "deepseek-flash", "choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
                    if usage is not None:
                        value["choices"] = []
                        value["usage"] = usage
                    data = ("data: " + json.dumps(value, separators=(",", ":")) + "\n\n").encode()
                    self.wfile.write(format(len(data), "x").encode() + b"\r\n" + data + b"\r\n")
                    self.wfile.flush()
                    chunks.append({"at_ns": time.perf_counter_ns(), "base64": base64.b64encode(data).decode()})

                frame({"role": "assistant", "content": "fixture-"})
                with owner.lock:
                    hold = owner.hold
                    owner.hold = None
                if hold:
                    owner.hold_started.set()
                    if not hold.wait(10):
                        raise RuntimeError("Client did not acknowledge the progressive fixture delta")
                for index in range(owner.frames - 1):
                    if owner.delay_ms:
                        time.sleep(owner.delay_ms / 1000)
                    frame({"content": "wire-ok" if index == 0 else "."})
                frame({}, "stop")
                frame({}, usage={"prompt_tokens": 11, "completion_tokens": 5, "total_tokens": 16, "prompt_cache_hit_tokens": 0})
                done = b"data: [DONE]\n\n"
                self.wfile.write(format(len(done), "x").encode() + b"\r\n" + done + b"\r\n0\r\n\r\n")
                self.wfile.flush()
                call["response_chunks"] = chunks
                call["completed_ns"] = time.perf_counter_ns()
                self.close_connection = True

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.socket.listen(64)
        self.server.daemon_threads = False
        self.server.block_on_close = True
        if self.server.server_port in FORBIDDEN_PORTS:
            self.server.server_close()
            raise RuntimeError("OS assigned a prohibited upstream port")
        self.origin = "http://127.0.0.1:" + str(self.server.server_port)
        self.wakeup_read, self.wakeup_write = socket.socketpair()
        self.thread = threading.Thread(target=self._serve, daemon=False)
        self.thread.start()

    def _serve(self):
        with selectors.DefaultSelector() as selector:
            selector.register(self.server, selectors.EVENT_READ, "request")
            selector.register(self.wakeup_read, selectors.EVENT_READ, "shutdown")
            while True:
                for key, _ in selector.select():
                    if key.data == "shutdown":
                        return
                    self.server._handle_request_noblock()

    def hold_next(self):
        with self.lock:
            if self.hold is not None:
                raise RuntimeError("Only one producer gate may be active")
            self.hold_started.clear()
            self.hold = threading.Event()
            return self.hold

    def redirect_next(self, status):
        if status not in (301, 302):
            raise RuntimeError("Only the two approved redirect-negative fixtures are admitted")
        with self.lock:
            if self.redirect_status is not None:
                raise RuntimeError("A redirect-negative fixture is already pending")
            self.redirect_status = status

    def close(self):
        with self.lock:
            if self.hold is not None:
                self.hold.set()
        self.wakeup_write.send(b"x")
        self.thread.join(timeout=3)
        if self.thread.is_alive():
            raise RuntimeError("Scripted upstream thread did not settle")
        self.server.server_close()
        self.wakeup_read.close()
        self.wakeup_write.close()
