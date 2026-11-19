"""Actual production-entry smoke and finite request/RSS workloads."""

import base64
import concurrent.futures
import hashlib
import json
import math
import signal
import statistics
import time
from collections import Counter
from pathlib import Path

from client import request, websocket
from network import require_task_path, sha256, write_json
from process import GatewayProcess
from upstream import CATALOG_MODEL, Upstream

ADMIN_TOKEN = "uos-synthetic-smoke-admin"
API_TOKEN = "u_" + "d" * 64
VERSION = "0.163.0"


def synthetic_auth():
    def encode(value):
        return base64.urlsafe_b64encode(json.dumps(value, separators=(",", ":")).encode()).decode().rstrip("=")
    claims = {"exp": int(time.time()) + 86400, "email": "fixture@invalid.example",
              "https://api.openai.com/auth": {"chatgpt_account_id": "uos-smoke-account", "chatgpt_plan_type": "pro",
                                               "chatgpt_user_id": "uos-smoke-user"}}
    token = encode({"alg": "none", "typ": "JWT"}) + "." + encode(claims) + ".fixture"
    return {"auth_mode": "chatgpt", "OPENAI_API_KEY": None, "tokens": {"id_token": token, "access_token": token,
            "refresh_token": "uos-synthetic-refresh-never-used", "account_id": "uos-smoke-account"},
            "last_refresh": "2026-11-17T00:00:00Z"}


def environment(auth):
    return {"DENO_TIMELINE": "production", "DENO_DEPLOY_TOKEN": ADMIN_TOKEN,
            "UOS_AI_TOKEN": "uos-synthetic-model-reader", "DEEPSEEK_API_KEY": "uos-synthetic-deepseek",
            "CODEX_AUTH_JSON_B64": base64.b64encode(json.dumps(auth, separators=(",", ":")).encode()).decode()}


def manifest(template, state, upstream):
    value = json.loads(Path(template).read_text())
    value.update({"stateRoot": str(state), "hostname": "127.0.0.1", "port": 0,
                  "fixture": {"syntheticOrigin": upstream.origin}})
    destination = state / "runtime-manifest.json"
    write_json(destination, value)
    return destination


def checked(result, status=200):
    if result["status"] != status:
        raise RuntimeError(f"{result['path']} returned {result['status']}, expected {status}: " + str(result.get("json", result.get("text"))))
    return result


def inference_body(route, streamed):
    if route == "chat":
        return {"model": "deepseek-flash", "messages": [{"role": "user", "content": "Synthetic smoke: emit fixture text."}],
                "stream": streamed, "max_completion_tokens": 64}
    return {"model": "deepseek-flash", "input": "Synthetic smoke: emit fixture text.", "stream": streamed,
            "max_output_tokens": 64}


def infer(gateway, route, streamed, on_delta=None):
    path = "/v1/chat/completions" if route == "chat" else "/v1/responses"
    result = checked(request(gateway.origin, path, API_TOKEN, inference_body(route, streamed), on_delta=on_delta))
    if streamed:
        values = [item["data"] for item in result["events"]]
        if route == "chat":
            if not values or values[-1] != "[DONE]":
                raise RuntimeError("Chat SSE omitted its terminal marker")
            content = "".join(choice.get("delta", {}).get("content") or "" for value in values if isinstance(value, dict)
                              for choice in value.get("choices", []))
            usage = [value["usage"] for value in values if isinstance(value, dict) and isinstance(value.get("usage"), dict)]
            if not usage or usage[-1].get("total_tokens") != 16:
                raise RuntimeError("Chat SSE lost scripted usage")
        else:
            terminal = [value for value in values if isinstance(value, dict) and value.get("type") == "response.completed"]
            if len(terminal) != 1 or terminal[0].get("response", {}).get("status") != "completed":
                raise RuntimeError("Responses SSE omitted its completed response")
            content = "".join(value.get("delta", "") for value in values if isinstance(value, dict)
                              and value.get("type") == "response.output_text.delta")
        if not content.startswith("fixture-wire-ok"):
            raise RuntimeError("Streaming text differs from the scripted provider")
    else:
        value = result["json"]
        if route == "chat":
            content = value["choices"][0]["message"]["content"]
            if value["usage"]["total_tokens"] != 16:
                raise RuntimeError("Buffered Chat lost scripted usage")
        else:
            if value.get("status") != "completed":
                raise RuntimeError("Buffered Responses did not complete")
            content = "".join(part.get("text", "") for item in value.get("output", []) for part in item.get("content", []))
        if not content.startswith("fixture-wire-ok"):
            raise RuntimeError("Buffered text differs from the scripted provider")
    return result


def usage(gateway, key_id):
    result = checked(request(gateway.origin, "/admin/api-keys?include_usage=true", ADMIN_TOKEN))
    rows = [row for row in result["json"]["data"] if row["id"] == key_id]
    if len(rows) != 1 or "usage_requests" not in rows[0]:
        raise RuntimeError("Durable admin usage read omitted the synthetic key")
    return result, rows[0]["usage_requests"]


def terminals(lines):
    result = []
    for line in lines:
        marker = "[ai.ubq.fi] request_terminal "
        if marker in line["line"]:
            value = json.loads(line["line"].split(marker, 1)[1])
            result.append({"at_ns": line["at_ns"], "value": value})
    return result


def validate_delivery_terminals(events, upstream_calls):
    calls = [call for call in upstream_calls if call["path"] == "/chat/completions"]
    identifiers = [entry["value"].get("request_id") for entry in events]
    if len(identifiers) != len(calls) or len(identifiers) != len(set(identifiers)) or any(not item for item in identifiers):
        raise RuntimeError("Every scripted inference needs exactly one correlated delivery terminal")
    expected = Counter(502 if call.get("response_status") in (301, 302) else 200 for call in calls)
    observed = Counter(entry["value"].get("status") for entry in events)
    if observed != expected:
        raise RuntimeError("Terminal statuses differ from the finite provider script")
    for entry in events:
        value = entry["value"]
        if value.get("delivery_outcome") != "delivered":
            raise RuntimeError("A complete scripted client read lost physical delivery evidence")
        if value["status"] == 200 and (value.get("stream_terminal_type") != "response.completed" or
                                      value.get("input_tokens") != 11 or value.get("output_tokens") != 5 or value.get("total_tokens") != 16):
            raise RuntimeError("A successful scripted inference lost its semantic terminal or usage")


def metrics(values):
    ordered = sorted(values)
    return {"count": len(values), "median_ms": statistics.median(values) / 1000,
            "p95_ms": ordered[math.ceil(len(ordered) * 0.95) - 1] / 1000,
            "serial_requests_per_second": 1000000 / statistics.mean(values)}


def benchmark(gateway, upstream):
    result = {"serial": {}, "concurrency": 8, "load_requests": 16, "load_frames": 64, "load_interframe_delay_ms": 5}
    gateway.phase = "warmup"
    for route in ("chat", "responses"):
        for streamed in (False, True):
            infer(gateway, route, streamed)
    gateway.phase = "serial"
    for route in ("chat", "responses"):
        for streamed in (False, True):
            values = [infer(gateway, route, streamed)["elapsed_us"] for _ in range(15)]
            result["serial"][route + ("_stream" if streamed else "_buffered")] = {**metrics(values), "samples_us": values}
    gateway.phase = "concurrent"
    upstream.frames, upstream.delay_ms = 64, 5
    started = time.perf_counter_ns()
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        pending = [pool.submit(infer, gateway, "chat" if index % 2 == 0 else "responses", True) for index in range(16)]
        results = [future.result(timeout=20) for future in pending]
    elapsed = (time.perf_counter_ns() - started) / 1e9
    result["load_elapsed_seconds"] = elapsed
    result["load_requests_per_second"] = len(results) / elapsed
    result["load_latency"] = metrics([value["elapsed_us"] for value in results])
    result["load_response_sha256"] = [value["body_sha256"] for value in results]
    upstream.frames, upstream.delay_ms = 4, 0
    gateway.phase = "post_load"
    return result


def run_trial(arguments, trial):
    root = require_task_path(arguments.output / f"{trial}-native")
    root.mkdir(mode=0o700)
    (root / ".data").mkdir(mode=0o700)
    auth = synthetic_auth()
    upstream = Upstream()
    gateway = None
    captures = {}
    process_receipt = None
    result = {"runtime": "native", "trial": trial, "state_root": str(root), "synthetic_only_environment_names": sorted(environment(auth)),
              "initial_state_empty": not any((root / ".data").iterdir()), "captures": captures}
    try:
        startup = manifest(arguments.runtime_template, root, upstream)
        command = [str(arguments.gateway), arguments.fixture_flag, str(startup)]
        result["command"] = command
        result["manifest_sha256"] = sha256(startup)
        gateway = GatewayProcess(command, root, environment(auth))
        result["startup_us"] = gateway.ready_us
        result["cold_idle"] = gateway.idle("idle_cold")
        gateway.phase = "smoke"
        captures["health"] = checked(request(gateway.origin, "/health"))
        captures["health_head"] = checked(request(gateway.origin, "/health", method="HEAD"))
        if captures["health_head"]["body_bytes"] != 0:
            raise RuntimeError("HEAD health delivered a body")
        captures["unauthorized_admin"] = checked(request(gateway.origin, "/admin/api-keys"), 401)
        captures["create_key"] = checked(request(gateway.origin, "/admin/api-keys", ADMIN_TOKEN,
             {"name": "Synthetic production smoke", "token": API_TOKEN, "usage_limit_requests": 10000,
              "window_ms": 3600000, "paid_fallback_enabled": False}))
        key_id = captures["create_key"]["json"]["id"]
        captures["seed_auth_catalog"] = checked(request(gateway.origin, "/admin/codex/auth", ADMIN_TOKEN,
             {"auth": auth, "models": {"client_version": VERSION, "models": [CATALOG_MODEL]}}))
        captures["models"] = checked(request(gateway.origin, "/v1/models", API_TOKEN))
        if captures["models"]["json"].get("object") != "list" or not isinstance(captures["models"]["json"].get("data"), list):
            raise RuntimeError("Bare model response lost its official list schema")
        captures["models_versioned"] = checked(request(gateway.origin, "/v1/models?client_version=" + VERSION, API_TOKEN))
        if not isinstance(captures["models_versioned"]["json"].get("models"), list):
            raise RuntimeError("Versioned model response lost its Codex-native schema")
        for route in ("chat", "responses"):
            for streamed in (False, True):
                if streamed:
                    gate = upstream.hold_next()
                    captures[route + "_stream"] = infer(gateway, route, True, on_delta=lambda _: gate.set())
                    if not gate.is_set():
                        raise RuntimeError("Gateway buffered the entire progressive provider stream")
                else:
                    captures[route + "_buffered"] = infer(gateway, route, False)
        for status in (301, 302):
            upstream.redirect_next(status)
            before_redirect = len(upstream.calls)
            captures["redirect_" + str(status)] = checked(request(gateway.origin, "/v1/chat/completions", API_TOKEN,
                                                                inference_body("chat", False)), 502)
            if len(upstream.calls) != before_redirect + 1 or upstream.redirected_requests:
                raise RuntimeError("Fixture redirect followed a second destination behind its outer URL guard")
        captures["ws_unknown"] = websocket(gateway.origin, "/unknown")
        if captures["ws_unknown"]["status"] != 404:
            raise RuntimeError("Unknown WebSocket endpoint did not refuse")
        captures["ws_unauthorized"] = websocket(gateway.origin, "/v1/live/rtc_fixture")
        if captures["ws_unauthorized"]["status"] != 401:
            raise RuntimeError("WebSocket upgrade did not authenticate")
        boundary = "uos-production-smoke"
        multipart = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"sdp\"\r\n\r\nv=0\r\nfixture-offer\r\n"
                     f"--{boundary}\r\nContent-Disposition: form-data; name=\"session\"\r\n\r\n"
                     + json.dumps({"model": CATALOG_MODEL["slug"]}) + f"\r\n--{boundary}--\r\n").encode()
        captures["live_create"] = checked(request(gateway.origin, "/v1/live", API_TOKEN, multipart,
                                      content_type="multipart/form-data; boundary=" + boundary), 201)
        captures["ws_relay"] = websocket(gateway.origin, captures["live_create"]["headers"]["location"], API_TOKEN, relay=True)
        if captures["ws_relay"]["status"] != 101:
            raise RuntimeError("Authenticated real WebSocket route did not upgrade")
        result["warm_idle"] = gateway.idle("idle_warm")
        if arguments.benchmark:
            result["benchmark"] = benchmark(gateway, upstream)
        captures["usage_before_shutdown"], before = usage(gateway, key_id)
        gate = upstream.hold_next()
        gateway.phase = "shutdown_drain"
        def shutdown_after_delta(_):
            gateway.signal(signal.SIGTERM)
            gate.set()
        captures["shutdown_stream"] = infer(gateway, "responses", True, on_delta=shutdown_after_delta)
        if not gate.is_set():
            raise RuntimeError("Shutdown signal was never triggered by a delivered delta")
        process_receipt = gateway.close()
        if process_receipt["exit_code"] != 0 or process_receipt["forced"] or not process_receipt["settled"]:
            raise RuntimeError("Gateway did not settle naturally after SIGTERM drain")
        events = terminals(gateway.lines)
        validate_delivery_terminals(events, upstream.calls)
        result["terminals"] = events
        for name in ("chat_buffered", "chat_stream", "responses_buffered", "responses_stream", "shutdown_stream"):
            request_id = captures[name]["headers"].get("x-uos-request-id")
            matched = [entry for entry in events if entry["value"].get("request_id") == request_id]
            if not request_id or len(matched) != 1 or matched[0]["value"].get("delivery_outcome") != "delivered":
                raise RuntimeError("Expected one physical delivery terminal for " + name)
        shutdown_id = captures["shutdown_stream"]["headers"]["x-uos-request-id"]
        terminal = next(entry for entry in events if entry["value"].get("request_id") == shutdown_id)
        telemetry = [entry for entry in gateway.lines if "[ai.ubq.fi] optional_telemetry_shutdown " in entry["line"]]
        if telemetry and telemetry[-1]["at_ns"] < terminal["at_ns"]:
            raise RuntimeError("Optional telemetry closed before the required request terminal")
        dispatches = len(upstream.calls)
        gateway = None
        restarted = GatewayProcess(command, root, environment(auth), label="reopen")
        try:
            captures["usage_reopened"], after = usage(restarted, key_id)
            if after != before + 1 or len(upstream.calls) != dispatches:
                raise RuntimeError("Reopened quota lost the terminal write, settled twice, or redispatched")
            result["durability"] = {"usage_before": before, "usage_reopened": after, "expected_additional": 1,
                                    "dispatches_before": dispatches, "dispatches_after": len(upstream.calls)}
        finally:
            result["reopen_process"] = restarted.close()
        result["outcome"] = "passed"
    finally:
        if gateway is not None and process_receipt is None:
            process_receipt = gateway.close(force=True)
        if process_receipt is not None:
            result["process"] = process_receipt
        upstream.close()
        result["upstream"] = {"origin": upstream.origin, "calls": upstream.calls, "websocket_frames": upstream.websocket_frames,
                              "failures": upstream.failures, "redirected_requests": upstream.redirected_requests,
                              "settled": not upstream.thread.is_alive()}
        write_json(root / "raw.json", result)
    return result


def run(arguments):
    return {
        "surface": "primary release gateway",
        "gateway": {"path": str(arguments.gateway), "sha256": sha256(arguments.gateway)},
        "runtime_template_sha256": sha256(arguments.runtime_template),
        "trials": [run_trial(arguments, trial) for trial in range(2 if arguments.benchmark else 1)],
    }
