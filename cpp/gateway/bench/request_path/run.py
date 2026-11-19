#!/usr/bin/env python3
"""Compare the packaged native gateway with the Bun production-smoke launcher."""

import argparse
from collections import Counter
from contextlib import contextmanager, nullcontext
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from driver import batch, connection, control, metrics, request, require
from fixtures import ADMIN_TOKEN, API_TOKEN, INFERENCE, PROVIDER_TOKEN, SCENARIOS, encode
from record import record

SMOKE = HERE.parents[1] / "tests/production_smoke"
PATH = os.defpath + ":/usr/sbin:/sbin"
TICKS = os.sysconf("SC_CLK_TCK")
LISTEN = re.compile(rb"Listening on http://127\.0\.0\.1:(\d+)/")
TERMINAL_PREFIX = "[ai.ubq.fi] request_terminal "


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_namespace(parent):
    current = os.readlink("/proc/self/ns/net")
    require(current != parent and os.geteuid() != 0, "Run as an unprivileged child in a fresh network namespace")
    for args in (["address", "show"], ["-4", "route", "show", "table", "all"], ["-6", "route", "show", "table", "all"]):
        rows = json.loads(subprocess.check_output(["ip", "-j", *args], timeout=5))
        require(all(row.get("ifname", row.get("dev")) == "lo" for row in rows), "Network namespace contains a non-loopback address or route")
    return current


def sparse(root):
    for name in ("home", "tmp"):
        (root / name).mkdir(mode=0o700)
    return {"PATH": PATH, "HOME": str(root / "home"), "TMPDIR": str(root / "tmp"), "LC_ALL": "C.UTF-8", "TZ": "UTC"}


def wait_port(child, log, pattern):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        with log.open("rb") as stream:
            found = pattern.search(stream.read(65536))
        if found:
            port = int(found[1])
            require(0 < port <= 65535 and port not in (7999, 8000), "Fixture assigned a reserved port")
            return port
        if child.poll() is not None:
            raise RuntimeError(f"Process exited before readiness ({child.returncode}), see {log}")
        time.sleep(0.01)
    raise RuntimeError(f"Process did not publish readiness, see {log}")


def stop(child, expected=0):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    if child.poll() is None:
        child.send_signal(signal.SIGTERM)
    status = child.wait(timeout=20)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    require(status == expected, f"Process {child.pid} exited {status}, expected {expected}")
    return after.ru_utime - before.ru_utime, after.ru_stime - before.ru_stime


@contextmanager
def process(command, root, environment, log_name):
    log = root / log_name
    with log.open("wb") as output:
        child = subprocess.Popen(command, cwd=root, env=environment, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
        try:
            yield child, log
        finally:
            if child.poll() is None:
                try:
                    stop(child)
                except (RuntimeError, subprocess.TimeoutExpired):
                    child.kill()
                    child.wait(timeout=5)


def cpu(pid):
    # The process stat counts all its threads. Skip the parenthesized comm field.
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return int(fields[11]) / TICKS, int(fields[12]) / TICKS


def peak_rss(pid):
    status = Path(f"/proc/{pid}/status").read_text()
    return int(re.search(r"^VmHWM:\s*(\d+) kB", status, re.M)[1]) / 1024


def settled(port, key, expected):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        rows = control(port, "/admin/api-keys?include_usage=true", ADMIN_TOKEN)["data"]
        used = next(row["usage_requests"] for row in rows if row["id"] == key)
        if used == expected:
            return
        require(used < expected, f"Expected {expected} settled warm-up requests, observed {used}")
        time.sleep(0.01)
    raise RuntimeError(f"Warm-up usage did not settle at {expected}")


def check_terminals(log, ids):
    require(len(ids) == len(set(ids)), "Gateway reused an inference request ID")
    pending = set(ids)
    with log.open() as stream:
        for line in stream:
            if not line.startswith(TERMINAL_PREFIX):
                continue
            row = json.loads(line[len(TERMINAL_PREFIX):])
            identifier = row["request_id"]
            require(identifier in pending, "Unexpected or duplicate inference terminal")
            require(row["status"] == 200 and row["delivery_outcome"] == "delivered" and row["total_tokens"] == 16
                    and row["stream_terminal_type"] == "response.completed", "Gateway terminal lost delivery or usage evidence")
            pending.remove(identifier)
    require(not pending, f"{len(pending)} inference requests lacked a natural-shutdown terminal")


def command_for(args, runtime, root, mock_port, identities):
    manifest = {"schema": "uos-gateway-bun-source-fixture-v1", "stateRoot": str(root), "hostname": "127.0.0.1", "port": 0,
                "fixture": {"syntheticOrigin": f"http://127.0.0.1:{mock_port}"}}
    path = root / "launch.json"
    if runtime == "native":
        manifest.update(schema="uos-gateway-launch-v1", packageRoot=str(args.package),
                        packageManifestSha256=identities["package_manifest"], nativeKvProvenanceSha256=identities["native_kv_provenance"])
        command = [args.package / "bin/uos-gateway", "--runtime-manifest", path]
    else:
        command = [args.bun, "--no-env-file", "--no-install", "--config=" + str(SMOKE / "bunfig.toml"),
                   SMOKE / "bun.ts", args.source, path, args.binding]
    path.write_text(json.dumps(manifest) + "\n")
    return command


def trial(args, runtime, name, repetition, mock_port, mock_pid, identities):
    fixture = SCENARIOS[name]
    count = args.requests or fixture.requests
    warmup = args.warmup if args.warmup is not None else fixture.warmup
    proof = name == "stream"
    root = args.out / f"{repetition:02d}-{name}-{runtime}"
    root.mkdir(mode=0o700)
    (root / ".data").mkdir(mode=0o700)
    command = command_for(args, runtime, root, mock_port, identities)
    environment = {**sparse(root), "DENO_TIMELINE": "production", "DENO_DEPLOY_TOKEN": ADMIN_TOKEN,
                   "DEEPSEEK_API_KEY": PROVIDER_TOKEN}
    mock_before = control(mock_port, "/fixture/counts")
    with process(command, root, environment, "gateway.log") as (child, log):
        port = wait_port(child, log, LISTEN)
        control(port, "/health")
        ids = []
        if name != "reject":
            key = control(port, "/admin/api-keys", ADMIN_TOKEN, encode({"name": "Synthetic request path", "token": API_TOKEN,
                          "usage_limit_requests": count + warmup + 100, "window_ms": 3_600_000, "paid_fallback_enabled": False}))["id"]
        if proof:
            control(mock_port, "/fixture/hold")
            client = connection(port)
            try:
                ids.append(request(client, name, lambda: control(mock_port, "/fixture/release"))[2])
            finally:
                client.close()
        if warmup:
            warmed, _ = batch(port, name, warmup, args.concurrency)
            if name in INFERENCE:
                ids.extend(row[2] for row in warmed)
        if ids:
            settled(port, key, len(ids))

        profile = record(child.pid, root, args.sudo_perf) if args.profile else nullcontext()
        before = cpu(child.pid)
        mock_cpu_before = cpu(mock_pid)
        driver_before = time.process_time_ns()
        with profile:
            results, elapsed = batch(port, name, count, args.concurrency)
        driver_cpu_ms = (time.process_time_ns() - driver_before) / 1e6 / count
        mock_cpu_ms = (sum(cpu(mock_pid)) - sum(mock_cpu_before)) * 1000 / count
        hwm = peak_rss(child.pid)
        if name in INFERENCE:
            ids.extend(row[2] for row in results)
        total = stop(child)
        gateway_cpu_ms = [(end - start) * 1000 / count for start, end in zip(before, total)]

    if name in INFERENCE:
        check_terminals(log, ids)
    mock_after = control(mock_port, "/fixture/counts")
    delta = Counter(mock_after["calls"]) - Counter(mock_before["calls"])
    expected = {name: count + warmup + proof} if name in INFERENCE else {}
    require(delta == expected and mock_after["failures"] == mock_before["failures"],
            f"{runtime} {name}: mock dispatch went outside the fixture expectation")
    return {**metrics(results, elapsed), "runtime": runtime, "scenario": name, "repetition": repetition, "warmup": warmup,
            "request_bytes": len(fixture.body or b""), "gateway_user_ms_per_request": gateway_cpu_ms[0],
            "gateway_system_ms_per_request": gateway_cpu_ms[1], "gateway_cpu_ms_per_request": sum(gateway_cpu_ms),
            "driver_cpu_ms_per_request": driver_cpu_ms, "mock_cpu_ms_per_request": mock_cpu_ms, "peak_rss_mib": hwm,
            "upstream_calls": dict(delta), "upstream_wire_size_range": mock_after["wire_sizes"].get(name),
            "delivered_inference_terminals": len(ids), "progressive_sse_proof": proof, "exit_code": 0}


def identities_for(args):
    binding = json.loads(args.binding.read_text())
    data = {"bun": digest(args.bun), "bun_lock": digest(args.source / "bun.lock"), "bun_launcher": digest(SMOKE / "bun.ts"),
            "bun_config": digest(SMOKE / "bunfig.toml"),
            "native_kv_provenance": binding["expectedProvenanceSha256"], "native_kv_addon": digest(Path(binding["artifactDirectory"]) / "native-kv.node")}
    if args.package:
        require(json.loads((args.package / "package.json").read_text())["nativeKvProvenanceSha256"] == data["native_kv_provenance"],
                "Both gateways must use the same frozen native KV provenance")
        data.update(native=digest(args.package / "bin/uos-gateway"), package_manifest=digest(args.package / "manifest.sha256"))
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="Detached Bun oracle prepared by conformance/oracle.sh --production with frozen dependencies")
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--package", type=Path, help="Independently built native candidate package")
    parser.add_argument("--binding", type=Path, required=True)
    parser.add_argument("--bun", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--parent-netns", required=True)
    parser.add_argument("--concurrency", type=int, default=4)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--scenarios", nargs="+", choices=list(SCENARIOS), default=list(SCENARIOS))
    parser.add_argument("--requests", type=int, help="Override timed batch size for a smoke check or profile")
    parser.add_argument("--warmup", type=int, help="Override warm-up batch size")
    parser.add_argument("--bun-only", action="store_true", help="Verify the oracle fixture before building the native package")
    parser.add_argument("--profile", choices=list(SCENARIOS), help="Run one native instrumented trial for this scenario")
    parser.add_argument("--sudo-perf", action="store_true", help="Use non-interactive sudo for perf sampling")
    args = parser.parse_args()
    if min(args.concurrency, args.repetitions, args.requests if args.requests is not None else 1) < 1 or (args.warmup is not None and args.warmup < 0):
        parser.error("Concurrency, repetitions and request count must be positive and warm-up nonnegative")
    if (not args.bun_only and args.package is None) or (args.bun_only and args.profile):
        parser.error("Native comparison and profiling require --package")
    args.source, args.bun, args.binding, args.out = (path.resolve() for path in (args.source, args.bun, args.binding, args.out))
    if args.package:
        args.package = args.package.resolve(strict=True)
    for tree in (HERE.parents[3], args.source):
        require(not args.out.is_relative_to(tree), "Run output must stay outside source checkouts")
    namespace = verify_namespace(args.parent_netns)
    pinned = subprocess.check_output(["bash", HERE.parents[1] / "conformance/oracle.sh", "--production", "--check", args.source],
                                     text=True).strip()
    require(args.source_revision == pinned, "The Bun source revision must match the oracle pin")
    os.umask(0o077)
    args.out.mkdir(mode=0o700)
    identities = identities_for(args)
    receipt = {"source_revision": args.source_revision, "binaries_sha256": identities, "network_namespace": namespace,
               "host": platform.platform(), "cpus": sorted(os.sched_getaffinity(0)), "concurrency": args.concurrency,
               "clock_ticks_per_second": TICKS, "profile": args.profile,
               "request_sha256": {name: hashlib.sha256(fixture.body or b"").hexdigest() for name, fixture in SCENARIOS.items()},
               "trials": []}
    mock_root = args.out / "mock"
    mock_root.mkdir(mode=0o700)
    with process([sys.executable, "-I", HERE / "mock.py"], mock_root, sparse(mock_root), "mock.log") as (mock, log):
        mock_port = wait_port(mock, log, re.compile(rb"^(\d+)\n"))
        scenarios = args.scenarios if not args.profile else [args.profile]
        for repetition in range(1, 2 if args.profile else args.repetitions + 1):
            order = ("native",) if args.profile else ("bun",) if args.bun_only else (("bun", "native") if repetition % 2 else ("native", "bun"))
            offset = (repetition - 1) % len(scenarios)
            for name in scenarios[offset:] + scenarios[:offset]:
                pair = []
                for runtime in order:
                    print(f"{repetition} {name} {runtime} running", flush=True)
                    row = trial(args, runtime, name, repetition, mock_port, mock.pid, identities)
                    pair.append(row)
                    receipt["trials"].append(row)
                    (args.out / "results.partial.json").write_text(json.dumps(receipt, indent=2) + "\n")
                    print(f"{repetition} {name} {runtime} {row['requests']} passed, {row['requests_per_s']:.1f} req/s", flush=True)
                require(len({tuple(row["models"] or ()) for row in pair}) == 1, "Bun and native model lists differed")
        stop(mock, expected=-signal.SIGTERM)
    (args.out / "results.partial.json").rename(args.out / "results.json")


if __name__ == "__main__":
    main()
