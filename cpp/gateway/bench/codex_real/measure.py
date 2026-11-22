#!/usr/bin/env python3
"""Measure a sealed gateway with a real Codex client and realistic KV state."""

import argparse
from collections import Counter
from contextlib import ExitStack, closing
import json
import os
from pathlib import Path
import platform
import re
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE.parent / "request_path"), str(HERE.parents[1] / "tests/production_smoke"), str(HERE)]
from cases import synthetic_auth
from sessions import Codex, Scripted, check_auth, codex_version, native_codex
from driver import connection, control, require
from fixtures import ADMIN_TOKEN, API_TOKEN, encode
from programmed import MODEL, TOOL_ROUNDS, upstream
from run import LISTEN, TERMINAL_PREFIX, command_for, digest, identities_for, process, sparse, stop, verify_namespace, wait_port
from sampler import gateway_pid, sample, trim, trim_probe

VERSION = "0.171.0"
MIX = ("tools", "short", "long", "reasoning", "resume")
HISTORY = 5000
CHECK_SESSIONS = 20
ANONYMOUS_BUDGET_KIB = 26 * 1024
PEAK_RESIDENT_BUDGET_KIB = 67 * 1024
REPLAY_KEY = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"  # Fixed synthetic key.
COUNTS = {
    ("uos_ai", "api_key_usage"): 413_480,
    ("uos_ai", "sentinel_replay"): 10_595,
    ("uos_ai", "paid_fallback"): 47_089,
    ("ubq_ai", "api_keys"): 17_786,
    ("uos_ai", "admin_error_log"): 6_948,
    ("uos_ai", "provider_capacity"): 3_116,
    ("embeddings", "v2"): 848,
    ("uos_ai", "metered_quota"): 2_051,
    ("uos_ai", "prompt_cache_analytics"): 6_792,
    ("uos_ai", "prompt_cache_telemetry_gate"): 2_848,
    ("uos_ai", "sentinel_incident"): 538,
    ("ubq_ai", "kernel_auth"): 72,
    ("uos_ai", "provider_health"): 52,
    ("ubq_ai", "codex_auth"): 1,
}


def inventory(database):
    families = {}
    with closing(sqlite3.connect(database)) as db:
        db.execute("pragma wal_checkpoint(truncate)")
        prefixes = [*COUNTS, ("ubq_ai", "codex_catalog_chunk"), ("ubq_ai", "codex_catalog_source_chunk")]
        for prefix in prefixes:
            packed = b"".join(b"\x02" + part.encode() + b"\x00" for part in prefix)
            rows, size, largest = db.execute("select count(*), sum(length(v)), max(length(v)) from kv where k>=? and k<?",
                                             (packed, packed[:-1] + b"\x01")).fetchone()
            # The catalog and health paths can also write a small number of live rows during seeding.
            if prefix in COUNTS:
                extra = {"provider_health": 2, "provider_capacity": 4}.get(prefix[-1], 0)
                require(COUNTS[prefix] <= rows <= COUNTS[prefix] + extra,
                        f"State prefix {prefix} has {rows} rows, expected {COUNTS[prefix]}")
            families["/".join(prefix)] = {"rows": rows, "value_bytes": size, "max_value_bytes": largest}
        total, values = db.execute("select count(*), sum(length(v)) from kv").fetchone()
    return {"rows": total, "value_bytes": values, "database_bytes": database.stat().st_size, "families": families}


def fetch_catalog(port, version, expected_cache):
    with closing(connection(port)) as conn:
        conn.request("GET", "/v1/models?client_version=" + version, headers=Scripted(port, version, "").headers())
        response = conn.getresponse()
        data = response.read()
        require(response.status == 200 and response.getheader("x-uos-cache") == expected_cache,
                f"Expected {expected_cache} versioned catalog, received HTTP {response.status} with cache state "
                f"{response.getheader('x-uos-cache')}")
    rows = json.loads(data)["models"]
    require(len(rows) == 15 and rows[0]["slug"] == MODEL, "Expected 15 exact-version models")
    return {"models": len(rows), "bytes": len(data), "etag": response.getheader("etag"),
            "model_messages_bytes": sum(len(encode(row["model_messages"])) for row in rows),
            "model_messages_distinct": len({encode(row["model_messages"]) for row in rows}),
            "base_instructions_bytes": sum(len(row["base_instructions"]) for row in rows)}


def seed(args, state, command, environment, mock, auth):
    database = state / ".data/kv-bun.sqlite3"
    root = state / "seeding"
    root.mkdir()
    env = {**sparse(root), **environment}
    with process(command, state, env, str(args.out / "seed.log")) as (child, log):
        port = wait_port(child, log, LISTEN)
        seeded = control(port, "/admin/codex/auth", ADMIN_TOKEN,
                         encode({"auth": auth, "models": {"client_version": mock.version}}))
        require(seeded["stored"] and seeded["catalog_seeded"] and seeded["account_count"] == 1, "Could not seed the synthetic Codex pool")
        stop(child)
    subprocess.run([args.seed_state, database], cwd=state, env=env, check=True, timeout=240)
    with process(command, state, env, str(args.out / "seed-catalog.log")) as (child, log):
        port = wait_port(child, log, LISTEN)
        exact = fetch_catalog(port, mock.version, "hit")
        with closing(connection(port)) as conn:
            conn.request("GET", "/v1/models", headers={"Authorization": "Bearer " + API_TOKEN})
            response = conn.getresponse()
            response_body = response.read()
            require(response.status == 200, f"Public catalog returned {response.status}")
            public = json.loads(response_body)
            require(public["object"] == "list" and all(row["object"] == "model" for row in public["data"]),
                    "Plain models endpoint omitted its public schema")
        stop(child)
    return inventory(database), exact, {"models": len(public["data"]), "bytes": len(response_body)}


def terminals(log, reader, count, pending, *, failure=False, check_memory=False):
    deadline = time.monotonic() + 10
    while len(pending) < count:
        line = reader.readline()
        if not line:
            require(time.monotonic() < deadline, f"Timed out waiting for {count} delivered terminals in {log}")
            time.sleep(0.01)
            continue
        if not line.startswith(TERMINAL_PREFIX):
            continue
        record = json.loads(line[len(TERMINAL_PREFIX):])
        identifier = record["request_id"]
        require(identifier not in pending and record["delivery_outcome"] == "delivered" and record["provider"] == "chatgpt_codex",
                "Terminal must have a unique ID, subscription provider and physical delivery evidence")
        if failure:
            require(record["stream_terminal_type"] == "response.failed", "Replay control did not produce a semantic failure")
        else:
            require(record["status"] == 200 and record["stream_terminal_type"] == "response.completed" and record["total_tokens"] > 0,
                    "Terminal lost HTTP, provider completion or token usage evidence")
        if check_memory:
            check_telemetry(record.get("memory"))
        pending.add(identifier)


def check_telemetry(memory):
    require(isinstance(memory, dict), "Memory telemetry is missing")
    for key in ("resident_bytes", "anonymous_bytes", "allocator_in_use_bytes", "allocator_free_bytes", "allocator_mmap_bytes"):
        require(type(memory.get(key)) is int and memory[key] >= 0, f"Invalid memory telemetry for {key}")
    caches = memory.get("caches", {})
    require(set(("api_key_policy", "codex_catalog", "json_parser", "sqlite_pages", "sqlite_statements")) <= caches.keys(),
            "The required cache gauges are missing")
    require(all(type(cache.get(key)) is int and cache[key] >= 0 for cache in caches.values() for key in ("entries", "bytes")),
            "A cache gauge has an invalid entry count or byte size")


def measure(args, state, command, environment, mock):
    with ExitStack() as stack:
        probe, inherited = None, None
        if args.heaptrack:
            command = ["heaptrack", "--record-only", "-o", args.out / "heaptrack", *command]
        if args.trim:
            probe, inherited, environment = stack.enter_context(trim_probe(args.out, environment))
        child, log = stack.enter_context(process(command, state, environment, str(args.out / "gateway.log"),
                                     pass_fds=(inherited.fileno(),) if inherited else ()))
        if inherited:
            inherited.close()
        port = wait_port(child, log, LISTEN)
        pid = gateway_pid(child, args.package, args.heaptrack)
        maps = Path(f"/proc/{pid}/maps").read_text()
        sqlite_mapping = next(line.split()[-1] for line in maps.splitlines() if "/libsqlite3.so." in line)
        closure = json.loads((args.package / "runtime-closure.json").read_text())
        pinned = next(row["sha256"] for row in closure["libraries"] if row.get("soname") == "libsqlite3.so.0")
        require(digest(Path(sqlite_mapping)) == pinned, "SQLite map differs from sealed runtime")
        started = time.monotonic()
        health = control(port, "/health")
        release = health["release"]
        profile = json.loads((args.package / "build-profile.json").read_text())
        require(release["git_sha"] == profile["git_commit"], "Health release differs from the sealed build profile")
        codex = Codex(args.codex, mock.version, state, port, mock.tool_text) if args.codex else None
        client = codex or Scripted(port, mock.version, mock.tool_text)
        samples = []
        pending = set()

        with (args.out / "samples.jsonl").open("w", buffering=1) as samples_log, log.open() as reader:
            def record(index, kind, memory=None):
                if memory is None and "memory" in health:
                    memory = control(port, "/health")["memory"]
                if args.check:
                    check_telemetry(memory)
                values = sample(pid)
                row = {"session": index, "kind": kind, "requests_served": len(pending), "elapsed_s": round(time.monotonic() - started, 3),
                       **values}
                if memory is not None:
                    row["gateway_memory"] = memory
                samples.append(row)
                print(json.dumps(row), file=samples_log)
                if index in (0, 1, 10, 20, 100, 500) or kind == "trim":
                    print(f"{kind} {index}: {values['RssAnon']} KiB anonymous, {values['VmHWM']} KiB peak, "
                          f"{len(pending)} inference requests", flush=True)
            record(0, "start", health.get("memory"))
            for index in range(1, args.sessions + 1):
                kind = MIX[(index - 1) % len(MIX)] if args.traffic == "mix" else args.traffic
                start = len(mock.calls)
                mock.script = kind, start
                if kind == "auth":
                    check_auth(port, mock.version)
                elif kind == "catalog":
                    Scripted(port, mock.version, mock.tool_text).catalog()
                elif kind != "idle":
                    client.run(index, kind)
                    count = TOOL_ROUNDS + 1 if kind == "tools" else 1
                    require(len(mock.calls) == start + count and not mock.failures,
                            f"{kind} session had unexpected mock calls or validation failures")
                    terminals(log, reader, start + count, pending, failure=kind == "replay", check_memory=args.check)
                mock.script = None
                record(index, kind)
            malloc_bytes = trim(probe) if probe else None
            if malloc_bytes is not None:
                record(args.sessions, "trim")
            if args.heaptrack:
                # Retain the live allocation snapshot at the end of the trace.
                os.kill(pid, signal.SIGKILL)
                require(child.wait(timeout=30) == 128 + signal.SIGKILL, "Heaptrack did not close its trace")
            else:
                stop(child)
            require(not any(line.startswith(TERMINAL_PREFIX) for line in reader), "Unexpected extra request terminal")
        return {"samples_kib": samples, "malloc_bytes": malloc_bytes, "sqlite_mapping": sqlite_mapping,
                "release": release, "client": "codex " + codex.version if codex else "scripted", "requests_served": len(pending),
                "elapsed_s": round(time.monotonic() - started, 3)}


def main():
    # CI can test for an installed native executable without running a launcher.
    if len(sys.argv) == 3 and sys.argv[1] == "--find-codex":
        binary = native_codex(sys.argv[2])
        if binary:
            print(binary)
        return 0 if binary else 1

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--seed-state", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--parent-netns", required=True)
    client = parser.add_mutually_exclusive_group(required=True)
    client.add_argument("--codex", help="Path to the installed Codex CLI or its native binary")
    client.add_argument("--scripted", action="store_true")
    parser.add_argument("--version", help="Exact catalog version, normally taken from the installed CLI")
    parser.add_argument("--sessions", type=int)
    parser.add_argument("--check", action="store_true", help="Enforce anonymous and peak resident budgets for 20 sessions")
    parser.add_argument("--traffic", choices=("mix", "auth", "catalog", "idle", "replay"), default="mix")
    parser.add_argument("--disable-replay", action="store_true")
    profiling = parser.add_mutually_exclusive_group()
    profiling.add_argument("--trim", action="store_true")
    profiling.add_argument("--heaptrack", action="store_true")
    args = parser.parse_args()
    if args.check:
        require(args.sessions in (None, CHECK_SESSIONS) and args.traffic == "mix" and not (args.trim or args.heaptrack or args.disable_replay),
                "The budget uses the fixed 20-session mix without allocator probes")
    if args.sessions is None:
        args.sessions = CHECK_SESSIONS if args.check else 1
    require(1 <= args.sessions <= 500, "Unsupported session count")
    require(not (args.traffic == "replay" and args.disable_replay), "Replay control needs a synthetic capture key")
    require(args.traffic == "mix" or args.scripted, "Select the scripted client for isolated attribution")
    namespace = verify_namespace(args.parent_netns)
    if args.codex:
        args.codex = native_codex(args.codex)
        require(args.codex, "Pass the installed native Codex executable")
    args.package = args.package.resolve(strict=True)
    args.seed_state = args.seed_state.resolve(strict=True)
    args.out = args.out.resolve()
    require(not args.out.is_relative_to(HERE.parents[3]), "Output must stay outside the source checkout")
    os.umask(0o077)
    args.out.mkdir(mode=0o700)
    identities = identities_for(args)
    if args.codex:
        with tempfile.TemporaryDirectory(prefix="codex-version-", dir=args.out) as version_root:
            version = codex_version(args.codex, Path(version_root))
        require(args.version in (None, version), "Codex client and requested catalog version differ")
        args.version = version
    else:
        args.version = args.version or VERSION
    require(re.fullmatch(r"[0-9]+[.][0-9]+[.][0-9]+", args.version), "Expected an exact Codex version")
    auth = synthetic_auth()
    with tempfile.TemporaryDirectory(prefix="state-", dir=args.out) as temporary, upstream(auth, args.version) as mock:
        state = Path(temporary)
        (state / ".data").mkdir()
        command = command_for(args, state, mock.server_port, identities)
        env = {"DENO_TIMELINE": "production", "DENO_DEPLOY_TOKEN": ADMIN_TOKEN}
        database, catalog, public = seed(args, state, command, env, mock, auth)
        (args.out / "fixture.json").write_text(json.dumps({"database": database, "catalog": catalog, "public": public}, indent=2) + "\n")
        print(f"Seeded {database['rows']} rows, {database['database_bytes']} database bytes, {catalog['bytes']} catalog bytes", flush=True)
        measured = state / "measurement"
        measured.mkdir()
        if not args.disable_replay:
            env["SENTINEL_REPLAY_KEY"] = REPLAY_KEY
        baseline_fetches = list(mock.catalog_calls)
        try:
            result = measure(args, state, command, {**sparse(measured), **env}, mock)
        finally:
            (args.out / "upstream.json").write_text(json.dumps({"catalog_versions": mock.catalog_calls, "calls": mock.calls,
                                                                "failures": mock.failures}, indent=2) + "\n")
            # Keep only failed client transcripts. Successful transcripts can
            # grow to hundreds of megabytes; they are synthetic either way.
            if sys.exc_info()[0] and args.codex:
                for path in state.glob("session-*/*"):
                    if path.suffix in (".stderr", ".jsonl") and path.is_file():
                        (args.out / (path.parent.name + "-" + path.name)).write_bytes(path.read_bytes())
        extra_fetches = mock.catalog_calls[len(baseline_fetches):]
        require(not mock.failures and len(extra_fetches) <= int(result["elapsed_s"] / 300) + 1
                and all(version == args.version for version in extra_fetches), "Unexpected mock dispatch outside measured reply script")
        if not args.heaptrack:
            db = state / ".data/kv-bun.sqlite3"
            verify = state / "verify"
            verify.mkdir()
            usage = int(subprocess.check_output([args.seed_state, db, "--usage"], env=sparse(verify), cwd=state, timeout=10))
            require(usage == HISTORY + result["requests_served"], f"Expected exactly one quota settlement per dispatch, observed {usage}")
            if args.traffic == "replay":
                with closing(sqlite3.connect(db)) as connection_db:
                    ready = connection_db.execute("select count(*) from kv where instr(k, ?) = 1",
                        (b"\x02uos_ai\x00\x02sentinel_replay\x00\x02v1\x00\x02manifest\x00",)).fetchone()[0]
                require(ready > 2000, "Replay trial did not publish a new encrypted capture")
            result["committed_usage"] = usage
        sizes = [call["input_bytes"] for call in mock.calls]
        receipt = {"host": platform.platform(), "network_namespace": namespace, "package": identities, "database": database,
                   "catalog": catalog, "public_models": public, "version": args.version, "traffic": args.traffic,
                   "sessions": args.sessions, "upstream_calls": dict(Counter(call["kind"] for call in mock.calls)),
                   "upstream_request_bytes": [min(sizes), max(sizes)] if sizes else None,
                   "heaptrack": args.heaptrack, "replay_enabled": not args.disable_replay, **result}
        if args.check:
            samples = result["samples_kib"]
            last = samples[-1]
            receipt["budget"] = {"after_run_anonymous_kib": max(last["RssAnon"], last["anonymous_kib"]),
                                 "anonymous_budget_kib": ANONYMOUS_BUDGET_KIB,
                                 "peak_resident_kib": max(row["VmHWM"] for row in samples),
                                 "peak_resident_budget_kib": PEAK_RESIDENT_BUDGET_KIB}
        path = args.out / "results.json"
        path.write_text(json.dumps(receipt, indent=2) + "\n")
        print(path, flush=True)
        if args.check:
            budget = receipt["budget"]
            print(f"Anonymous {budget['after_run_anonymous_kib']} / {ANONYMOUS_BUDGET_KIB} KiB, "
                  f"peak resident {budget['peak_resident_kib']} / {PEAK_RESIDENT_BUDGET_KIB} KiB", flush=True)
            require(budget["after_run_anonymous_kib"] <= ANONYMOUS_BUDGET_KIB and
                    budget["peak_resident_kib"] <= PEAK_RESIDENT_BUDGET_KIB, "The gateway exceeded its session memory budget")


if __name__ == "__main__":
    raise SystemExit(main())
