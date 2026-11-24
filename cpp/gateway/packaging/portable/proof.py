#!/usr/bin/python3
"""Exercise the real launcher and a scripted Codex response inside a capped container."""

from concurrent.futures import ThreadPoolExecutor
import ctypes
import json
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, "/workspace/cpp/gateway/tests/production_smoke")
from cases import ADMIN_TOKEN, API_TOKEN, CATALOG_MODEL, VERSION, checked, inference_body, synthetic_auth, usage
from client import request
from network import sha256, sparse_environment, write_json
from process import GatewayProcess
from upstream import Upstream

CONCURRENCY = 8
REQUESTS_PER_TRIAL = 384
TRIALS = 3
MIB = 1024 * 1024
CHAT = "/v1/chat/completions"
MODEL = CATALOG_MODEL["slug"]
ANSWER = "fixture-wire-ok"
CGROUP = Path("/sys/fs/cgroup")
STAGED = Path("/staged")


class CodexUpstream(Upstream):
    def __init__(self):
        super().__init__()
        self.hold_group = None
        upstream = self

        class Handler(self.server.RequestHandlerClass):
            def do_POST(self):
                if self.path != "/codex/responses":
                    super().do_POST()
                    return
                try:
                    self.connection.settimeout(10)
                    length = int(self.headers["Content-Length"])
                    if not 0 < length <= 4096:
                        raise ValueError("Expected a bounded small chat request")
                    body = json.loads(self.rfile.read(length))
                    if body.get("model") != MODEL or body.get("stream") is not True or "input" not in body:
                        raise ValueError("Unexpected Codex request")
                    with upstream.lock:
                        if len(upstream.calls) >= upstream.maximum:
                            raise ValueError("Scripted upstream dispatch bound exceeded")
                        upstream.calls.append({"path": self.path})
                        sequence = len(upstream.calls)
                    if upstream.hold_group:
                        arrived, release = upstream.hold_group
                        arrived.wait(timeout=10)
                        if not release.wait(timeout=10):
                            raise ValueError("Concurrent fixture was never released")
                    events = [
                        {"type": "response.created", "response": {"id": f"resp_fixture_{sequence}", "created_at": 1780000001}},
                        {"type": "response.output_text.delta", "item_id": "item-1", "output_index": 0, "delta": ANSWER},
                        {"type": "response.completed", "response": {"status": "completed", "output": [
                            {"id": "item-1", "type": "message", "content": [{"type": "output_text", "text": ANSWER}]}
                        ], "usage": {"input_tokens": 11, "output_tokens": 5, "total_tokens": 16}}},
                    ]
                    payload = "".join("data: " + json.dumps(event, separators=(",", ":")) + "\n\n" for event in events).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Content-Length", str(len(payload)))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(payload)
                    self.close_connection = True
                except Exception as error:
                    upstream.failures.append(str(error))
                    self.json_response(500, {"error": "Codex fixture failed"})

        self.server.RequestHandlerClass = Handler


def status(pid):
    return dict(line.split(":", 1) for line in Path(f"/proc/{pid}/status").read_text().splitlines() if ":" in line)


def cgroup(name):
    return (CGROUP / name).read_text().strip()


def memory_events():
    return {key: int(value) for key, value in (line.split() for line in cgroup("memory.events").splitlines())}


def memory(gateway, phase):
    samples = [item["rss_bytes"] for item in gateway.samples if item["phase"] == phase and item["rss_bytes"]]
    if len(samples) < 3:
        raise RuntimeError(f"Insufficient RSS samples during {phase}")
    return {"sample_count": len(samples), "median_rss_bytes": statistics.median(samples), "peak_rss_bytes": max(samples)}


def health(gateway, expected_commit):
    result = checked(request(gateway.origin, "/health"))
    if result["json"]["status"] != "available":
        raise RuntimeError("Health did not report availability")
    release = result["json"].get("release") or {}
    reported = release.get("git_sha")
    if reported != expected_commit:
        raise RuntimeError(f"Health release commit {reported!r} does not match the packaged commit {expected_commit!r}")
    if reported == "unknown" or len(reported) != 40 or any(character not in "0123456789abcdef" for character in reported):
        raise RuntimeError(f"Health reported an unbound release commit: {reported!r}")
    if result["headers"].get("x-uos-git-sha") != expected_commit:
        raise RuntimeError("Health x-uos-git-sha header does not match the packaged commit")
    return result["json"]


def favicons(gateway, package):
    hashes = {}
    for route, asset in (("/favicon.ico", "favicon.png"), ("/favicon.png", "favicon.png"), ("/favicon-32.png", "favicon-32.png")):
        result = checked(request(gateway.origin, route))
        if result["headers"].get("content-type") != "image/png" or result["body_sha256"] != sha256(package / asset):
            raise RuntimeError(f"Packaged asset differs at {route}")
        hashes[route] = result["body_sha256"]
    return hashes


def chat(gateway, body):
    result = checked(request(gateway.origin, CHAT, API_TOKEN, body))["json"]
    choice = result["choices"][0]
    if (result["object"] != "chat.completion" or result["model"] != MODEL or choice["finish_reason"] != "stop" or
            choice["message"]["content"] != ANSWER or result["usage"]["total_tokens"] != 16):
        raise RuntimeError(f"Unexpected small chat result: {result}")


def clean_shutdown(gateway):
    result = gateway.close()
    if result["exit_code"] != 0 or not result["settled"] or result["failure"]:
        raise RuntimeError("Gateway did not settle naturally after SIGTERM")


def exercise(gateway, upstream, binding, expected_commit):
    if Path(f"/proc/{gateway.child.pid}/exe").resolve(strict=True) != Path(binding["packageRoot"]) / "bin/uos-gateway":
        raise RuntimeError("The launched process is not the sealed gateway")
    process = status(gateway.child.pid)
    if int(process["NoNewPrivs"]) != 1 or int(process["Seccomp_filters"]) <= int(status("self")["Seccomp_filters"]):
        raise RuntimeError("Gateway did not install its seccomp filter")
    cold = gateway.idle("idle_cold")
    initial_health = health(gateway, expected_commit)
    static_sha256 = favicons(gateway, Path(binding["packageRoot"]))
    body = inference_body("chat", False)
    body["model"] = MODEL
    checked(request(gateway.origin, CHAT, body=body), 401)
    created = checked(request(gateway.origin, "/admin/api-keys", ADMIN_TOKEN,
                      {"name": "Portable fixture", "token": API_TOKEN, "usage_limit_requests": 10000,
                       "window_ms": 3600000, "paid_fallback_enabled": False}))
    key = created["json"]["id"]
    checked(request(gateway.origin, "/admin/codex/auth", ADMIN_TOKEN,
                    {"auth": synthetic_auth(), "models": {"client_version": VERSION, "models": [CATALOG_MODEL]}}))
    chat(gateway, body)
    warm = gateway.idle("idle_warm")

    with ThreadPoolExecutor(max_workers=CONCURRENCY) as pool:
        arrived = threading.Barrier(CONCURRENCY + 1)
        release = threading.Event()
        upstream.hold_group = (arrived, release)
        hold = [pool.submit(chat, gateway, body) for _ in range(CONCURRENCY)]
        try:
            arrived.wait(timeout=10)
            gateway.phase = "eight_in_flight"
            time.sleep(0.24)
            concurrent = memory(gateway, "eight_in_flight")
        finally:
            release.set()
        for future in hold:
            future.result(timeout=20)
        upstream.hold_group = None

        def worker():
            for _ in range(REQUESTS_PER_TRIAL // CONCURRENCY):
                chat(gateway, body)

        rates = []
        for trial in range(TRIALS):
            gateway.phase = f"throughput_{trial + 1}"
            started = time.monotonic()
            pending = [pool.submit(worker) for _ in range(CONCURRENCY)]
            for future in pending:
                future.result(timeout=30)
            rates.append(REQUESTS_PER_TRIAL / (time.monotonic() - started))
    completed = 1 + CONCURRENCY + TRIALS * REQUESTS_PER_TRIAL
    _, accounted = usage(gateway, key)
    if len(upstream.calls) != completed or any(call["path"] != "/codex/responses" for call in upstream.calls) or accounted != completed or upstream.failures or upstream.redirected_requests:
        raise RuntimeError(f"Expected {completed} completed and accounted chats: calls={len(upstream.calls)} usage={accounted} failures={upstream.failures}")
    return key, {"health_initial": initial_health, "health_after_load": health(gateway, expected_commit), "unauthenticated_chat_status": 401,
                 "favicon_sha256": static_sha256,
                 "completed_chat_requests": completed, "accounted_requests": accounted, "idle_cold": cold, "idle_warm": warm,
                 "eight_in_flight": concurrent, "throughput_rss": [memory(gateway, f"throughput_{trial + 1}") for trial in range(TRIALS)],
                 "requests_per_trial": REQUESTS_PER_TRIAL, "requests_per_second": rates, "median_requests_per_second": statistics.median(rates),
                 "seccomp_filters": int(process["Seccomp_filters"])}


def verify_startup_preflight(state, binding):
    package = Path(binding["packageRoot"])
    launch = state / "integrity.json"
    environment = sparse_environment(state / "integrity")
    manifest = {**binding, "schema": "uos-gateway-launch-v1", "stateRoot": str(state), "hostname": "127.0.0.1", "port": 0}

    checked_cases = 0

    def refuse(document, root, expected, overrides=None):
        nonlocal checked_cases
        write_json(launch, document)
        result = subprocess.run([root / "ops/launch-native.sh", launch], cwd=state, env={**environment, **(overrides or {})},
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)
        if result.returncode != 1 or expected not in result.stderr or any((state / ".data").iterdir()):
            raise RuntimeError(f"Startup preflight expected {expected!r} and untouched state. Exit {result.returncode}. {result.stderr}")
        checked_cases += 1

    refuse({**manifest, "packageManifestSha256": "0" * 64}, package, "package manifest integrity check failed")
    copy = state.parent / (state.name + "-tampered")
    try:
        shutil.copytree(package, copy)
        (copy / "static/index.html").write_text("synthetic tamper\n")
        refuse({**manifest, "packageRoot": str(copy)}, copy, "package file integrity check failed: static/index.html")
    finally:
        if copy.exists():
            shutil.rmtree(copy)

    integrity_refusals = checked_cases
    production = {"DENO_TIMELINE": "production", "DENO_DEPLOY_TOKEN": ADMIN_TOKEN}
    listener = {**manifest, "port": 7999}
    denial = "port 7999 requires the production VPS service configuration"
    refuse(listener, package, denial, {"DENO_DEPLOY_TOKEN": ADMIN_TOKEN})
    refuse(listener, package, denial, {"DENO_TIMELINE": "production"})
    refuse(listener, package, denial, {**production, "DENO_DEPLOY": "1"})
    refuse(listener, package, denial, {**production, "UOS_GATEWAY_UPSTREAM_AUTH_FOLLOWER": "1"})
    refuse({**listener, "fixture": {"syntheticOrigin": "http://127.0.0.1:1"}}, package, denial, production)
    refuse({**listener, "port": 8000}, package, "port 8000 is reserved", production)
    # Production passes the listener check and reaches private database validation.
    missing_database = state / ".data/kv-bun.sqlite3"
    refuse(listener, package, f"required regular file is missing or unsafe: {missing_database}", production)
    return {"integrity_refusals": integrity_refusals, "listener_preflight_cases": checked_cases - integrity_refusals}


def run(state, cap):
    binding = json.loads((STAGED / "binding.json").read_text())
    package = Path(binding["packageRoot"])
    if sha256(package / "manifest.sha256") != binding["packageManifestSha256"]:
        raise RuntimeError("Staged package binding changed")
    profile = json.loads((package / "build-profile.json").read_text())
    expected_commit = profile.get("gitCommit")
    if not isinstance(expected_commit, str) or expected_commit == "unknown" or len(expected_commit) != 40 or any(
            character not in "0123456789abcdef" for character in expected_commit):
        raise RuntimeError(f"Staged package has no bound Git commit: {expected_commit!r}")
    (state / ".data").mkdir(mode=0o700)
    startup_preflight = verify_startup_preflight(state, binding)
    startup = state / "launch.json"
    upstream = CodexUpstream()
    environment = {"DENO_TIMELINE": "production", "DENO_DEPLOY_TOKEN": ADMIN_TOKEN}
    command = [str(package / "ops/launch-native.sh"), str(startup)]
    gateway = None
    try:
        write_json(startup, {**binding, "schema": "uos-gateway-launch-v1", "stateRoot": str(state),
                             "hostname": "127.0.0.1", "port": 0, "fixture": {"syntheticOrigin": upstream.origin}})
        gateway = GatewayProcess(command, state, environment)
        key, result = exercise(gateway, upstream, binding, expected_commit)
        clean_shutdown(gateway)
        gateway = None
        # A distinct empty HOME and TMPDIR are supplied for the reopened process.
        (state / "home").rmdir()
        (state / "tmp").rmdir()
        gateway = GatewayProcess(command, state, environment, label="reopen")
        health(gateway, expected_commit)
        _, reopened = usage(gateway, key)
        if reopened != result["completed_chat_requests"] or len(upstream.calls) != reopened:
            raise RuntimeError("Restart changed durable usage or dispatched a request")
        clean_shutdown(gateway)
        gateway = None
    except Exception:
        for name in ("gateway.stderr", "reopen.stderr"):
            log = state / name
            if log.is_file():
                print(log.read_text()[-4000:], file=sys.stderr)
        raise
    finally:
        if gateway is not None and not gateway.stop_monitor.is_set():
            gateway.close(force=True)
        upstream.close()
    return {**result, **startup_preflight, "cap_mib": cap, "concurrency": CONCURRENCY, "reopened_accounted_requests": reopened,
            "binary_sha256": sha256(package / "bin/uos-gateway"), "manifest_sha256": binding["packageManifestSha256"]}


def main():
    cap = int(sys.argv[1])
    quota, period = map(int, cgroup("cpu.max").split())
    if int(cgroup("memory.max")) != cap * MIB or cgroup("memory.swap.max") != "0" or quota != 2 * period:
        raise RuntimeError("Expected a cgroup v2 memory cap, no swap, and exactly two CPUs")
    before = memory_events()
    landlock_abi = ctypes.CDLL(None).syscall(444, 0, 0, 1)  # AArch64 landlock_create_ruleset version query.
    if landlock_abi < 8:
        raise RuntimeError(f"This host/container does not expose Landlock ABI 8 (reported {landlock_abi})")
    with tempfile.TemporaryDirectory(prefix="uos-pi-proof-", dir="/tmp") as state:
        result = run(Path(state), cap)
    after = memory_events()
    if any(after[name] != before[name] for name in ("oom", "oom_kill", "oom_group_kill")):
        raise RuntimeError(f"Memory cgroup reported OOM: {after}")
    print(json.dumps({**result, "cpu_max": cgroup("cpu.max"), "landlock_abi": landlock_abi,
                      "cgroup_memory_peak_bytes": int(cgroup("memory.peak")), "cgroup_memory_events": after}, sort_keys=True))


if __name__ == "__main__":
    main()
