"""Mixed-size concurrent traffic for the allocator retention check."""

from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
import json
import math
import random
import time

from driver import connection, require
from fixtures import encode
from programmed import MARKER, MODEL, TEXT, payload
from sessions import Scripted

CONNECTIONS = 8
TURN_BATCHES = 4
SIZES = (1000, 4000, 15000, 56000, 110000, 195000, 280000, 470000)


def run_round(pool, port, version, index, kind, source):
    # Vary both total wire size and individual JSON string size. An identical
    # large request on every connection lets glibc reuse the same size bins.
    rng = random.Random(index)
    turns = []
    for _ in range(TURN_BATCHES):
        sizes = [int(size * rng.uniform(0.8, 1.2)) for size in SIZES]
        rng.shuffle(sizes)
        turns.append([request_body(source, size) for size in sizes])

    def lane(bodies):
        client = Scripted(port, version, "")
        latencies = []
        with closing(connection(port)) as conn:
            conn.request("GET", "/v1/models?client_version=" + version, headers=client.headers())
            catalog = conn.getresponse()
            require(catalog.status == 200 and json.loads(catalog.read())["models"][0]["slug"] == MODEL,
                    "Burst client did not receive the exact-version catalog")
            for body in bodies:
                started = time.perf_counter_ns()
                text, _ = client.send(conn, body)
                require(text == TEXT[kind], "Burst lost reply text")
                latencies.append(time.perf_counter_ns() - started)
        return latencies

    started = time.perf_counter()
    latencies = [elapsed for lane_times in pool.map(lane, zip(*turns, strict=True)) for elapsed in lane_times]
    return latencies, time.perf_counter() - started


def request_body(source, size):
    width = (size + 6) // 7
    items = [{"type": "message", "role": "user", "content": [{"type": "input_text", "text": source[offset:offset + width]}]}
             for offset in range(0, size, width)]
    return encode({"model": MODEL, "instructions": MARKER, "input": items, "stream": True,
                   "store": False, "reasoning": {"effort": "none"}})


def source_text():
    return payload(math.ceil(max(SIZES) * 1.2), 437)


def percentile_ms(sorted_ns, fraction):
    return sorted_ns[math.ceil(len(sorted_ns) * fraction) - 1] / 1_000_000
