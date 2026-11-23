"""Closed-loop HTTP workloads and semantic checks for the synthetic request mix."""

from concurrent.futures import ThreadPoolExecutor
from http.client import HTTPConnection
import json
import math
import statistics
import time

from fixtures import API_TOKEN, COMPLETION, INFERENCE, MODEL, SCENARIOS, STREAM_TEXT


def connection(port):
    return HTTPConnection("127.0.0.1", port, timeout=20)


def control(port, path, token=None, body=None, status=200):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    client = connection(port)
    try:
        client.request("GET" if body is None else "POST", path, body, headers)
        result = client.getresponse()
        data = result.read()
        if result.status != status:
            raise RuntimeError(f"Control request {path} expected {status}, received {result.status}")
        return json.loads(data)
    finally:
        client.close()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_stream(response, acknowledge):
    require(response.getheader("Content-Type", "").startswith("text/event-stream"), "Expected a Chat SSE response")
    parts, event = [], []
    usage, done, stops, first_delta = None, False, 0, None
    for line in response:
        if line.startswith(b"data:"):
            event.append(line[5:].strip())
        elif line in (b"\n", b"\r\n") and event:
            data = b"\n".join(event)
            event.clear()
            require(not done, "SSE data appeared after the terminal marker")
            if data == b"[DONE]":
                done = True
                continue
            value = json.loads(data)
            for choice in value.get("choices", []):
                if choice.get("finish_reason") == "stop":
                    stops += 1
                text = choice.get("delta", {}).get("content")
                if text:
                    if first_delta is None:
                        first_delta = time.perf_counter_ns()
                    parts.append(text)
                    if acknowledge:
                        acknowledge()
            if value.get("usage") is not None:
                usage = value["usage"].get("total_tokens")
    require(tuple(parts) == STREAM_TEXT and not event and done and stops == 1 and usage == 16,
            "Stream lost a text delta, finish reason, usage or [DONE]")
    return first_delta


def validate_json(name, value):
    if name == "models":
        identifiers = tuple(row["id"] for row in value["data"] if row["object"] == "model")
        require(value["object"] == "list" and len(identifiers) == len(value["data"]) and MODEL in identifiers,
                "Models response omitted its configured DeepSeek model or list shape")
        return identifiers
    if name == "reject":
        error = value["error"]
        require(error == {"message": "Unauthorized", "type": "invalid_request_error", "code": "invalid_api_key"},
                "Expected unauthenticated error envelope")
    elif name == "responses":
        content = "".join(part["text"] for item in value["output"] for part in item.get("content", []) if part["type"] == "output_text")
        require(value["object"] == "response" and value["status"] == "completed" and content == COMPLETION
                and value["usage"]["total_tokens"] == 16, "Responses output or usage differs from the fixture")
    else:
        choice = value["choices"][0]
        require(value["object"] == "chat.completion" and choice["message"]["content"] == COMPLETION
                and choice["finish_reason"] == "stop" and value["usage"]["total_tokens"] == 16,
                "Chat output or usage differs from the fixture")
    return None


def request(client, name, acknowledge=None):
    fixture = SCENARIOS[name]
    headers = {} if name == "reject" else {"Authorization": "Bearer " + API_TOKEN}
    if fixture.body is not None:
        headers["Content-Type"] = "application/json"
    starts_connected = client.sock is not None
    started = time.perf_counter_ns()
    client.request("GET" if fixture.body is None else "POST", fixture.path, fixture.body, headers)
    response = client.getresponse()
    if response.status != fixture.status:
        raw = response.read(2000)
        raise RuntimeError(f"{name} expected HTTP {fixture.status}, received {response.status}: {raw.decode(errors='replace')}")
    first_delta, models = None, None
    if name == "stream":
        first_delta = read_stream(response, acknowledge) - started
        finished = time.perf_counter_ns()
    else:
        raw = response.read()
        finished = time.perf_counter_ns()
        models = validate_json(name, json.loads(raw))
    identifier = response.getheader("x-uos-request-id") if name in INFERENCE else None
    require(name not in INFERENCE or identifier, f"{name} omitted the gateway request ID")
    return finished - started, first_delta, identifier, models, starts_connected


def batch(port, name, count, concurrency):
    def worker(size):
        client = connection(port)
        try:
            return [request(client, name) for _ in range(size)]
        finally:
            client.close()

    concurrency = min(concurrency, count)
    sizes = [count // concurrency + (index < count % concurrency) for index in range(concurrency)]
    # The window includes client connection setup and the full reads. Each worker reuses its HTTP/1.1 connection.
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        started = time.perf_counter_ns()
        results = [row for result in pool.map(worker, sizes) for row in result]
        elapsed = (time.perf_counter_ns() - started) / 1e9
    return results, elapsed


def metrics(results, elapsed):
    latencies = sorted(row[0] / 1e6 for row in results)
    models = {row[3] for row in results if row[3] is not None}
    require(len(models) <= 1, "Model catalogue changed during the batch")
    summary = {"requests": len(results), "wall_s": elapsed, "requests_per_s": len(results) / elapsed,
               "p50_ms": statistics.median(latencies), "p95_ms": latencies[math.ceil(0.95 * len(latencies)) - 1],
               "p99_ms": latencies[math.ceil(0.99 * len(latencies)) - 1], "reused_connections": sum(row[4] for row in results),
               "models": next(iter(models), None)}
    if results[0][1] is not None:
        summary["first_delta_p50_ms"] = statistics.median(row[1] for row in results) / 1e6
    return summary
