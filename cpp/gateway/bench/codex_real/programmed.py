"""Finite Responses SSE source for the gateway's Codex subscription route."""

from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from itertools import count
import random
import string
import sys
from threading import Thread
from urllib.parse import parse_qs, urlsplit

from cases import CATALOG_MODEL
from driver import require
from fixtures import encode

MODEL = CATALOG_MODEL["slug"]
MARKER = "Programmed reply instructions. "
TEXT = {"short": " test" * 50, "long": " data" * 4000,
        "reasoning": " proof" * 50, "tools": " done" * 50, "resume": " again" * 50}
TOOL_ROUNDS = 10
TOOL_BYTES = 32_000
TOOL_FILE = "fixture.txt"
# Use printable, deterministic data, including repeated runs that a text
# compressor can represent. The marked prefix lets us prove catalog use.
ALPHABET = string.ascii_letters


def payload(length, seed, prefix=""):
    rng = random.Random(seed)
    return prefix + "".join(rng.choices(ALPHABET, k=length - len(prefix)))


def catalog():
    sizes = (69_000, 37_000, 25_000, 13_000, 6_000, 1_500)
    templates = [{"instructions_template": payload(size - 28, i, MARKER)} for i, size in enumerate(sizes)]
    bases = (28_000, 21_100, 10_200, 6_100, 5_300, 4_900, 4_500, 4_300,
             4_100, 4_000, 3_900, 3_700, 3_500, 3_300, 3_100)
    indices = (0, 1, 2, 3, 4, 5, 1, 2, 3, 4, 5, 3, 4, 5, 5)
    return {"models": [dict(CATALOG_MODEL, slug=MODEL if i == 0 else f"gpt-bench-{i:03}",
                            priority=i, base_instructions=payload(size, 100 + i, MARKER),
                            description=("Synthetic catalog metadata. " * 30)[:782],
                            model_messages=templates[indices[i]], supports_reasoning_summaries=True,
                            context_window=400_000, max_context_window=400_000, auto_compact_token_limit=320_000,
                            truncation_policy={"mode": "bytes", "limit": 40_000})
                       for i, size in enumerate(bases)]}


def tool_item(response_id, turn):
    call_id = f"call_{response_id}"
    if turn == 0:
        patch = "*** Begin Patch\n*** Add File: fixture-patch.txt\n+"
        patch += payload(TOOL_BYTES, 1234) + "\n*** End Patch"
        return {"type": "custom_tool_call", "id": call_id, "call_id": call_id,
                "name": "apply_patch", "input": patch, "status": "completed"}
    return {"type": "function_call", "id": call_id, "call_id": call_id, "name": "exec_command",
            "arguments": json.dumps({"cmd": f"cat {TOOL_FILE}", "max_output_tokens": 20_000, "yield_time_ms": 1000}), "status": "completed"}


def reasoning_item(response_id):
    return {"type": "reasoning", "id": f"rs_{response_id}",
            "summary": [{"type": "summary_text", "text": "The fixture will return the programmed result."}],
            "encrypted_content": "YWJjZA=="}


def message_item(response_id, text):
    return {"type": "message", "id": f"msg_{response_id}", "role": "assistant", "phase": "final_answer",
            "status": "completed", "content": [{"type": "output_text", "text": text, "annotations": []}]}


def stream(response_id, kind, turn, input_size):
    response = {"id": response_id, "object": "response", "status": "in_progress", "created_at": 1780000001, "model": MODEL,
                "output": []}
    yield {"type": "response.created", "response": response}
    items = []
    tool = kind == "tools" and turn < TOOL_ROUNDS
    if kind == "reasoning" or (tool and turn == 0):
        items.append(reasoning_item(response_id))
    if tool:
        items.append(tool_item(response_id, turn))
    else:
        text = "Programmed intermediate output." if kind == "replay" else TEXT[kind]
        items.append(message_item(response_id, text))
    for index, item in enumerate(items):
        started = {**item, "status": "in_progress"}
        if item["type"] == "message":
            started["content"] = []
        yield {"type": "response.output_item.added", "output_index": index, "item": started}
        if item["type"] == "message":
            for offset in range(0, len(text), 256):
                yield {"type": "response.output_text.delta", "item_id": item["id"], "output_index": index,
                       "content_index": 0, "delta": text[offset:offset + 256]}
        yield {"type": "response.output_item.done", "output_index": index, "item": item}
    if kind == "replay":
        yield {"type": "response.failed", "response": {**response, "status": "failed",
               "error": {"code": "server_error", "message": "Programmed replay failure"}}}
        return
    sent_tokens = 40 if tool else (4000 if kind == "long" else 50)
    input_tokens = max(11, input_size // 4)
    usage = {"input_tokens": input_tokens, "output_tokens": sent_tokens, "total_tokens": input_tokens + sent_tokens,
             "input_tokens_details": {"cached_tokens": 0}, "output_tokens_details": {"reasoning_tokens": 0}}
    yield {"type": "response.completed", "response": {**response, "status": "completed", "output": items, "usage": usage}}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    disable_nagle_algorithm = True
    timeout = 15

    def log_message(self, *_):
        pass

    def reply(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def reject(self, reason):
        self.server.failures.append(reason)
        self.reply(400, b"{}")

    def do_GET(self):
        url = urlsplit(self.path)
        version = parse_qs(url.query).get("client_version", [None])[0]
        if url.path == "/codex/models" and version == self.server.version:
            self.server.catalog_calls.append(version)
            self.reply(200, self.server.catalog)
        else:
            self.reject("Unexpected upstream GET")

    def do_POST(self):
        size = int(self.headers.get("Content-Length", "0"))
        if (self.path != "/codex/responses" or not 0 < size <= 2 * 1024 * 1024
                or self.headers.get("ChatGPT-Account-ID") != self.server.account
                or self.headers.get("Authorization") != self.server.authorization or self.server.script is None):
            self.reject("Unexpected subscription transport")
            return
        wire = self.rfile.read(size)
        request = json.loads(wire)
        kind, start = self.server.script
        turn = len(self.server.calls) - start
        items = request.get("input", [])
        outputs = [item for item in items if item.get("type") in ("function_call_output", "custom_tool_call_output")]
        if (request.get("model") != MODEL or request.get("stream") is not True
                or (kind == "tools" and (turn > TOOL_ROUNDS or len(outputs) != turn))
                or (kind == "resume" and len(outputs) != TOOL_ROUNDS)
                or MARKER.encode() not in wire):
            self.reject(f"Unexpected {kind} request shape in turn {turn} with {len(outputs)} tool results, body {size} bytes")
            return
        if kind == "tools" and turn:
            previous = outputs[-1].get("output", "")
            text = previous if isinstance(previous, str) else json.dumps(previous)
            if (turn == 1 and "fixture-patch.txt" not in text) or (turn > 1 and self.server.tool_text not in text):
                self.reject(f"Expected executed tool output in turn {turn}")
                return
        response_id = f"resp_bench_{next(self.server.response_ids)}"
        self.server.calls.append({"kind": kind, "input_bytes": size, "input_items": len(items), "tool_outputs": len(outputs)})
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for event in stream(response_id, kind, turn, size):
            data = b"data: " + encode(event) + b"\n\n"
            self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
        self.wfile.write(b"0\r\n\r\n")


class ProgrammedServer(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        # A client can reset an idle keep-alive socket when its CLI exits.
        if not isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError)):
            super().handle_error(request, client_address)


@contextmanager
def upstream(auth, version):
    with ProgrammedServer(("127.0.0.1", 0), Handler) as server:
        server.daemon_threads = True
        require(server.server_port not in (7999, 8000), "Mock received a reserved port")
        server.version = version
        server.catalog = encode(catalog())
        server.account = auth["tokens"]["account_id"]
        server.authorization = "Bearer " + auth["tokens"]["access_token"]
        server.tool_text = payload(TOOL_BYTES, 42)
        server.script = None
        server.response_ids = count(1)
        server.calls, server.catalog_calls, server.failures = [], [], []
        worker = Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            yield server
        finally:
            server.shutdown()
            worker.join()
