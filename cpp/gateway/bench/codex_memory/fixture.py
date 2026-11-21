"""Synthetic Codex catalog, usage and HTTP fixtures on the native KV schema."""

from contextlib import closing, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import sqlite3
from threading import Thread
from urllib.parse import parse_qs, urlsplit

from cases import ADMIN_TOKEN, API_TOKEN, CATALOG_MODEL
from driver import connection, control, require
from fixtures import encode
from run import settled

VERSION = "0.171.0"
SEED_VERSION = "0.170.0"
CATALOG_PATH = "/v1/models?client_version=" + VERSION
MODEL = CATALOG_MODEL["slug"]
MODEL_COUNT = 535
HISTORY = 16
ROWS = 540_000
TEXT = "fixture-wire-ok"
USAGE = {"input_tokens": 11, "output_tokens": 5, "total_tokens": 16}
INSTRUCTIONS = ("Inspect the project files and follow the repository's instructions. "
                "Use the established entry points, keep changes scoped, and verify behavior.\n" * 80)[:8192]
PROMPT = "Review the synthetic catalog entries and confirm receipt.\n" * 265
BODY = encode({"model": MODEL, "instructions": "Report the fixture result.", "input": [
    {"type": "message", "role": "user", "content": [{"type": "input_text", "text": PROMPT}]}
    for _ in range(4)
], "stream": True, "store": False, "reasoning": {"effort": "none"}, "include": ["reasoning.encrypted_content"]})
HEADERS = {"Authorization": "Bearer " + API_TOKEN, "User-Agent": f"codex_cli_rs/{VERSION}", "originator": "codex_cli_rs"}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    disable_nagle_algorithm = True
    timeout = 20

    def log_message(self, *_):
        pass

    def reply(self, body, mime="application/json", status=200):
        self.send_response(status)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def reject(self):
        self.server.failures += 1
        self.reply(b"{}", status=400)

    def do_GET(self):
        url = urlsplit(self.path)
        version = parse_qs(url.query).get("client_version", [None])[0]
        if url.path == "/codex/models" and version in (SEED_VERSION, VERSION):
            self.server.catalog_calls += 1
            self.reply(self.server.catalog if version == VERSION else encode({"models": [CATALOG_MODEL]}))
        else:
            self.reject()

    def do_POST(self):
        size = int(self.headers.get("Content-Length", 0))
        if self.path != "/codex/responses" or not 0 < size <= len(BODY) * 2:
            self.reject()
            return
        value = json.loads(self.rfile.read(size))
        texts = [part.get("text") for item in value.get("input", []) for part in item.get("content", [])
                 if part.get("type") == "input_text"]
        if (value.get("model") != MODEL or value.get("stream") is not True or
                value.get("reasoning", {}).get("effort") != "none" or texts.count(PROMPT) != 4 or
                self.headers.get("ChatGPT-Account-ID") != self.server.account):
            self.reject()
            return
        self.server.completions += 1
        identifier = f"resp_memory_{self.server.completions}"
        result = {"id": identifier, "object": "response", "created_at": 1780000001, "status": "completed", "model": MODEL,
                  "usage": USAGE, "output": [{"type": "message", "id": "msg_" + identifier, "status": "completed",
                  "role": "assistant", "content": [{"type": "output_text", "text": TEXT, "annotations": []}]}]}
        events = ({"type": "response.created", "response": {"id": identifier}},
                  {"type": "response.output_text.delta", "delta": TEXT},
                  {"type": "response.completed", "response": result})
        self.reply(b"".join(b"data: " + encode(event) + b"\n\n" for event in events), "text/event-stream")


@contextmanager
def upstream(account):
    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        server.daemon_threads = True
        require(server.server_port not in (7999, 8000), "Mock received a reserved port")
        models = [dict(CATALOG_MODEL, slug=MODEL if index == 0 else f"gpt-fixture-{index:03}",
                       base_instructions=INSTRUCTIONS) for index in range(MODEL_COUNT)]
        server.catalog = encode({"models": models})
        server.account = account
        server.catalog_calls = server.completions = server.failures = 0
        worker = Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            yield server
        finally:
            server.shutdown()
            worker.join()


def catalog(client, expected_cache):
    client.request("GET", CATALOG_PATH, headers=HEADERS)
    response = client.getresponse()
    require(response.status == 200, f"Catalog returned HTTP {response.status}")
    require(response.getheader("x-uos-cache") == expected_cache, "Unexpected exact-version catalog cache state")
    body = response.read()
    models = json.loads(body)["models"]
    match = [row for row in models if row.get("slug") == MODEL]
    require(len(models) >= MODEL_COUNT and len(match) == 1 and match[0]["base_instructions"] == INSTRUCTIONS,
            "Catalog lost the fixture models or their instructions")
    return len(models), len(body), response.getheader("etag")


def completion(client):
    client.request("POST", "/v1/responses", BODY, {**HEADERS, "Content-Type": "application/json"})
    response = client.getresponse()
    require(response.status == 200, f"Completion returned HTTP {response.status}")
    require(response.getheader("Content-Type", "").startswith("text/event-stream"), "Completion did not stream")
    events = [json.loads(line[5:]) for line in response if line.startswith(b"data:")]
    completed = [event["response"] for event in events if event["type"] == "response.completed"]
    text = "".join(event["delta"] for event in events if event["type"] == "response.output_text.delta")
    require(text == TEXT and len(completed) == 1 and completed[0]["status"] == "completed" and completed[0]["usage"] == USAGE,
            "Completion lost its text, terminal or usage")
    identifier = response.getheader("x-uos-request-id")
    require(identifier, "Completion omitted its request ID")
    return identifier


def seed(port, auth):
    for index in range(8):
        created = control(port, "/admin/api-keys", ADMIN_TOKEN, encode({
            "name": f"Synthetic memory fixture {index}", "token": API_TOKEN if index == 0 else "u_" + format(index, "064x"),
            "usage_limit_requests": 100_000, "window_ms": 3_600_000, "paid_fallback_enabled": False}))
        if index == 0:
            key = created["id"]
    seeded = control(port, "/admin/codex/auth", ADMIN_TOKEN, encode({
        "auth": auth, "models": {"client_version": SEED_VERSION}}))
    require(seeded["stored"] and seeded["catalog_seeded"] and seeded["account_count"] == 1, "Codex pool seed failed")
    with closing(connection(port)) as client:
        models, size, etag = catalog(client, "miss")
        ids = [completion(client) for _ in range(HISTORY)]
    settled(port, key, HISTORY)
    return key, ids, (models, size, etag)


def populate(database, rows):
    # The gateway seeds the marked schema and application rows. Add valid byte
    # values under an unrelated tuple key prefix, using one offline transaction.
    with closing(sqlite3.connect(database)) as sql:
        existing = sql.execute("select count(*) from kv").fetchone()[0]
        if rows > existing:
            sql.execute("""
                with recursive sequence(n) as (select 1 union all select n + 1 from sequence where n < ?)
                insert into kv (k, v, v_encoding, version, expiration_ms)
                select cast(x'026d656d6f72792d666978747572650002' || printf('%07d', n) || x'00' as blob),
                       zeroblob(1750), 3, (select version from data_version where k = 0), -1 from sequence
                """, (rows - existing,))
            sql.commit()
        sql.execute("pragma wal_checkpoint(truncate)")
        count = sql.execute("select count(*) from kv").fetchone()[0]
    return {"rows": count, "application_rows": existing, "database_bytes": database.stat().st_size}
