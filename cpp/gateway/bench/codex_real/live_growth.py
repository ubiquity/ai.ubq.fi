"""API-key traffic for measuring live allocator bytes after warm-up."""

from contextlib import closing
import json

from driver import connection, require, validate_json
from fixtures import API_TOKEN, STREAM_TEXT, encode
from programmed import MODEL, payload
from sessions import Scripted

TRAFFIC = ("public", "buffered", "versioned", "stream")
MEASURED = ("versioned", "stream")
INFERENCE = ("buffered", "stream")
CONNECTIONS = 4
WARMUP = 32
BATCH = 50
CHECK_REQUESTS = 300
BUDGET_BYTES = 1024 * 1024
FLASH = "ubiquity/deepseek-flash"


def batches(pool, port, version, count, traffic, catalog):
    source = payload(200_000, 437)
    bodies = {
        "buffered": [encode({"model": FLASH, "input": source, "stream": False})],
        "stream": [encode({"model": FLASH, "input": text, "stream": True}) for text in (source[:2_000], source)],
    }
    client = Scripted(port, version, "")
    headers = {"Authorization": "Bearer " + API_TOKEN}

    def lane(work):
        kind, indices = work
        with closing(connection(port)) as conn:
            for i in indices:
                if kind == "stream":
                    text, _ = client.send(conn, bodies[kind][i % 2], headers=headers)
                    require(text == "".join(STREAM_TEXT), "Lithos stream lost its programmed text")
                elif kind == "buffered":
                    conn.request("POST", "/v1/responses", bodies[kind][0], {**headers, "Content-Type": "application/json"})
                    response = conn.getresponse()
                    value = json.loads(response.read())
                    require(response.status == 200, "Buffered Lithos request failed")
                    validate_json("responses", value)
                else:
                    endpoint = "/v1/models?client_version=" + version if kind == "versioned" else "/v1/models"
                    conn.request("GET", endpoint, headers=headers)
                    response = conn.getresponse()
                    body = response.read()
                    require(response.status == 200, f"{kind} models request failed")
                    if kind == "versioned":
                        require(response.getheader("x-uos-cache") in ("hit", "stale") and len(body) == catalog["bytes"]
                                and response.getheader("etag") == catalog["etag"]
                                and json.loads(body)["models"][0]["slug"] == MODEL, "Exact-version catalog changed")
                    else:
                        require(json.loads(body)["object"] == "list", "Public models schema changed")

    def run(kind, start, size):
        list(pool.map(lane, ((kind, range(start + offset, start + size, CONNECTIONS)) for offset in range(CONNECTIONS))))

    dispatches = 0
    for kind in traffic:
        run(kind, 0, WARMUP)
        if kind in INFERENCE:
            dispatches += WARMUP
    yield "warmup", 0, dispatches
    for kind in traffic:
        yield kind, 0, dispatches
        for start in range(0, count, BATCH):
            size = min(BATCH, count - start)
            run(kind, start, size)
            if kind in INFERENCE:
                dispatches += size
            yield kind, start + size, dispatches


def growth_budget(samples):
    growth = {}
    before = {}
    for row in samples:
        kind = row["kind"]
        if kind not in MEASURED:
            continue
        live = row["gateway_memory"]["allocator_in_use_bytes"]
        if row["session"] == 0:
            before[kind] = live
        growth[kind] = max(growth.get(kind, 0), live - before[kind])
    return {"allocator_live_growth_bytes": growth, "total_growth_bytes": sum(growth.values()), "budget_bytes": BUDGET_BYTES}
