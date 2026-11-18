"""Extend the request-path mock on the same listener used by both gateways."""

import base64
from hashlib import sha1
import json
import struct
from threading import Event, Thread
from urllib.parse import urlsplit

from client import receive_frame, send_frame
from fixtures import BUFFERED, CHAT_ENVELOPE, MODEL, PROVIDER_TOKEN, SSE_TERMINAL, SSE_TEXT, USAGE, encode, sse
from mock import Handler as BenchmarkHandler, Mock as BenchmarkMock
from upstream import CATALOG_MODEL

TOOL = {"id": "call_fixture", "type": "function", "function": {"name": "lookup", "arguments": '{"city":"Oslo"}'}}
CODEX_MODEL = CATALOG_MODEL["slug"]
CODEX_USAGE = {"input_tokens": 11, "output_tokens": 5, "total_tokens": 16}


def codex_events(mode):
    content = {"type": "output_text", "text": "codex fixture answer", "annotations": []}
    item = {"id": "msg_fixture", "type": "message", "role": "assistant", "status": "completed", "content": [content]}
    response = {"id": "resp_fixture", "object": "response", "created_at": 1780000001, "model": CODEX_MODEL,
                "status": "completed", "output": [item], "usage": CODEX_USAGE}
    events = [{"type": "response.created", "response": {**response, "status": "in_progress", "output": [], "usage": None}}]
    if mode == "codex-tool":
        item = {"id": "fc_fixture", "call_id": TOOL["id"], "type": "function_call", "name": TOOL["function"]["name"],
                "arguments": TOOL["function"]["arguments"], "status": "completed"}
        events.extend([
            {"type": "response.output_item.added", "output_index": 0, "item": {**item, "arguments": "", "status": "in_progress"}},
            {"type": "response.function_call_arguments.delta", "output_index": 0, "item_id": item["id"], "delta": item["arguments"]},
            {"type": "response.function_call_arguments.done", "output_index": 0, "item_id": item["id"], "arguments": item["arguments"]},
        ])
    else:
        events.extend([
            {"type": "response.output_item.added", "output_index": 0, "item": {**item, "content": [], "status": "in_progress"}},
            {"type": "response.content_part.added", "output_index": 0, "content_index": 0, "item_id": item["id"], "part": {**content, "text": ""}},
            {"type": "response.output_text.delta", "output_index": 0, "content_index": 0, "item_id": item["id"], "delta": content["text"]},
        ])
        if mode == "codex-error":
            return events + [{"type": "error", "code": "server_error", "message": "Synthetic fault after a delivered delta."}]
        events.extend([
            {"type": "response.output_text.done", "output_index": 0, "content_index": 0, "item_id": item["id"], "text": content["text"]},
            {"type": "response.content_part.done", "output_index": 0, "content_index": 0, "item_id": item["id"], "part": content},
        ])
    return events + [
        {"type": "response.output_item.done", "output_index": 0, "item": item},
        {"type": "response.completed", "response": {**response, "output": [item]}},
    ]


class Handler(BenchmarkHandler):
    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/codex/models":
            self.server.dispatches.append({"route": "codex.models", "target": self.path})
            self.respond(200, encode({"models": [CATALOG_MODEL]}))
        elif path == "/live/rtc_fixture" and self.headers.get("Upgrade", "").lower() == "websocket":
            self.relay()
        else:
            super().do_GET()

    def do_POST(self):
        path = urlsplit(self.path).path
        mode = self.server.mode
        if path == "/chat/completions" and mode == "benchmark":
            super().do_POST()
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 2 * 1024 * 1024:
                raise ValueError("Expected bounded upstream request")
            body = json.loads(self.rfile.read(size))
            if path == "/chat/completions":
                if self.headers.get("Authorization") != "Bearer " + PROVIDER_TOKEN or body.get("model") != MODEL:
                    raise ValueError("Unexpected DeepSeek mock authentication or model")
                self.server.dispatches.append({"route": "deepseek." + mode, "body": body})
                self.deepseek(body, mode)
            elif path == "/codex/responses":
                self.record_codex("codex.responses", body)
                events = codex_events(mode)
                frames = [b"event: " + event["type"].encode() + b"\n" + sse({**event, "sequence_number": index})
                          for index, event in enumerate(events)]
                self.stream_frames(frames, gate_after=4)
            elif path == "/codex/realtime/calls":
                self.record_codex("codex.realtime", body)
                answer = b"v=0\r\ns=synthetic-smoke-answer\r\n"
                self.send_response(201)
                self.send_header("Content-Type", "application/sdp")
                self.send_header("Content-Length", str(len(answer)))
                self.send_header("Location", "/v1/live/rtc_fixture")
                self.end_headers()
                self.wfile.write(answer)
            else:
                raise ValueError("Unexpected POST route " + path)
        except (OSError, ValueError, TypeError, KeyError) as error:
            self.server.errors.append(f"{path}: {error}")
            self.close_connection = True

    def record_codex(self, route, body):
        if (self.headers.get("Authorization") != "Bearer " + self.server.codex_token
                or self.headers.get("chatgpt-account-id") != "uos-smoke-account"):
            raise ValueError("Unexpected Codex mock authentication")
        self.server.dispatches.append({"route": route, "body": body})

    def deepseek(self, request, mode):
        if mode == "unavailable":
            self.respond(503, encode({"error": {"code": "server_error", "message": "Synthetic upstream unavailable."}}))
        elif mode == "rate-limit":
            self.respond(429, encode({"error": {"code": "rate_limit_exceeded", "message": "Synthetic upstream rate limit."}}))
        elif mode == "broken":
            self.stream_frames([SSE_TEXT[0], sse({"error": {"code": "server_error", "message": "Synthetic mid-stream fault."}})], gate_after=1)
        elif mode == "tool":
            if request.get("stream"):
                chunks = [{"delta": {"role": "assistant", "tool_calls": [{"index": 0, **TOOL}]}, "finish_reason": None},
                          {"delta": {}, "finish_reason": "tool_calls"}]
                self.stream_frames([sse({**CHAT_ENVELOPE, "object": "chat.completion.chunk", "choices": [{"index": 0, **choice}]})
                                    for choice in chunks] + list(SSE_TERMINAL[1:]))
            else:
                self.respond(200, encode({**CHAT_ENVELOPE, "object": "chat.completion", "usage": USAGE,
                    "choices": [{"index": 0, "message": {"role": "assistant", "content": None, "tool_calls": [TOOL]}, "finish_reason": "tool_calls"}]}))
        elif request.get("stream"):
            self.stream_frames([*SSE_TEXT, *SSE_TERMINAL], gate_after=1)
        else:
            self.respond(200, BUFFERED)

    def stream_frames(self, frames, gate_after=None):
        gate = self.server.gate
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("X-Request-Id", "synthetic-provider-request")
        self.end_headers()
        for index, frame in enumerate(frames, 1):
            self.chunk(frame)
            if gate is not None and index == gate_after:
                self.server.progressive = gate.wait(2)
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def relay(self):
        if self.headers.get("Authorization") != "Bearer " + self.server.codex_token:
            self.server.errors.append("Unexpected Codex WebSocket mock authentication")
            self.respond(401)
            return
        dispatch = {"route": "codex.websocket"}
        self.server.dispatches.append(dispatch)
        key = self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", base64.b64encode(sha1(key.encode()).digest()).decode())
        self.end_headers()
        self.wfile.flush()
        try:
            opcode, data = receive_frame(self.connection)
            dispatch["frame"] = {"opcode": opcode, "utf8": data.decode()}
            send_frame(self.connection, opcode, data, False)
            if receive_frame(self.connection)[0] == 8:
                send_frame(self.connection, 8, struct.pack("!H", 1000), False)
        except (OSError, RuntimeError):
            pass  # The probe closes its socket after sending its close frame.
        self.close_connection = True


class Upstream(BenchmarkMock):
    def __init__(self, codex_token):
        super().__init__()
        self.RequestHandlerClass = Handler
        self.codex_token = codex_token
        self.dispatches = []
        self.errors = []
        self.begin({})

    def record(self, scenario, size):
        super().record(scenario, size)
        self.dispatches.append({"route": "deepseek." + scenario})

    def begin(self, spec):
        self.mode = spec.get("mock", "benchmark")
        self.gate = Event() if spec.get("progressive") else None
        self.progressive = None
        return len(self.dispatches), len(self.errors), self.failures

    def observe(self, start):
        calls, errors, failures = start
        return {"dispatches": self.dispatches[calls:], "mock_errors": self.errors[errors:],
                "benchmark_failures": self.failures - failures, "progressive": self.progressive}

    def __enter__(self):
        self.thread = Thread(target=self.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.shutdown()
        self.thread.join()
        self.server_close()
