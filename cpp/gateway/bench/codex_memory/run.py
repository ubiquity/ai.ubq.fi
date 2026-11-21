#!/usr/bin/env python3
"""Measure one packaged native gateway on synthetic Codex CLI traffic."""

import argparse
from contextlib import ExitStack, closing
import json
import os
from pathlib import Path
import platform
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE.parent / "request_path"), str(HERE.parents[1] / "tests/production_smoke"), str(HERE)]
from cases import ADMIN_TOKEN, synthetic_auth
from driver import connection, control, require
from fixture import BODY, HISTORY, ROWS, catalog, completion, populate, seed, upstream
from run import LISTEN, check_terminals, command_for, identities_for, process, settled, sparse, stop, verify_namespace, wait_port

CHECKPOINTS = (1, 10, 100, 1000)
SMAPS_HEAP = re.compile(r"^[0-9a-f]+-[0-9a-f]+ .* \[heap\]\n(?:(?!^[0-9a-f]+-[0-9a-f]+ ).*\n)*", re.M)
TRIM_FIELDS = ("trim_return", "arenas", "allocated_before", "free_before", "mmap_before",
               "allocated_after", "free_after", "mmap_after")


def kib(source, field):
    return int(re.search(rf"^{field}:\s*(\d+) kB$", source, re.M)[1])


def memory(pid):
    proc = Path(f"/proc/{pid}")
    status = (proc / "status").read_text()
    heap = SMAPS_HEAP.search((proc / "smaps").read_text())[0]
    return {**{field: kib(status, field) for field in ("VmRSS", "RssAnon", "VmHWM")},
            "heap_rss": kib(heap, "Rss"), "heap_size": kib(heap, "Size")}


def pause_for_idle():
    # The socket EOF arrives before local asynchronous terminal writes settle.
    time.sleep(0.15)


def launched_pid(child, package, profiled):
    if not profiled:
        return child.pid
    children = Path(f"/proc/{child.pid}/task/{child.pid}/children").read_text().split()
    return next(int(pid) for pid in children
                if (Path("/proc") / pid / "exe").resolve() == package / "bin/uos-gateway")


def traffic(args, client, pid, expected_catalog):
    ids, samples = [], []
    pause_for_idle()
    samples.append({"after": "start", "iteration": 0, **memory(pid)})
    for index in range(1, args.requests + 1):
        if args.traffic in ("session", "catalog"):
            require(catalog(client, "hit") == expected_catalog, "Catalog body or ETag changed across requests")
            if index in CHECKPOINTS:
                pause_for_idle()
                samples.append({"after": "catalog", "iteration": index, **memory(pid)})
        if args.traffic in ("session", "completion"):
            ids.append(completion(client))
            if index in CHECKPOINTS:
                pause_for_idle()
                samples.append({"after": "completion", "iteration": index, **memory(pid)})
    return ids, samples


def measure(args, state, command, environment, key, expected_catalog):
    with ExitStack() as stack:
        pass_fds = ()
        probe = None
        if args.heaptrack:
            command = ["heaptrack", "--record-only", "-o", args.out / "heaptrack", *command]
        elif not args.no_trim:
            library = args.out / "trim.so"
            subprocess.run(["c++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared", "-pthread",
                            HERE / "trim.cpp", "-o", library], env=environment, check=True, timeout=30)
            probe, inherited = socket.socketpair(type=socket.SOCK_SEQPACKET)
            stack.enter_context(probe)
            stack.enter_context(inherited)
            probe.settimeout(5)
            pass_fds = (inherited.fileno(),)
            environment = dict(environment, LD_PRELOAD=str(library), UOS_MEMORY_TRIM_FD=str(inherited.fileno()))
        child, log = stack.enter_context(process(command, state, environment, str(args.out / "gateway.log"), pass_fds=pass_fds))
        if probe:
            inherited.close()
        port = wait_port(child, log, LISTEN)
        pid = launched_pid(child, args.package, args.heaptrack)
        sqlite_mapping = next(line.split()[-1] for line in Path(f"/proc/{pid}/maps").read_text().splitlines()
                              if "/libsqlite3.so." in line)
        with closing(connection(port)) as client:
            ids, samples = traffic(args, client, pid, expected_catalog)
        trimmed = None
        if probe:
            probe.sendall(b"t")
            values = [int(item) for item in probe.recv(256).split()]
            trimmed = dict(zip(TRIM_FIELDS, values, strict=True))
            require(trimmed["trim_return"] in (0, 1) and trimmed["arenas"] > 0, "Invalid glibc trim receipt")
            samples.append({"after": "trim", "iteration": args.requests, **memory(pid)})
        expected_usage = HISTORY + len(ids)
        if args.heaptrack:
            # Keep the live allocations in Heaptrack's end snapshot.
            os.kill(pid, signal.SIGKILL)
            require(child.wait(timeout=30) == 128 + signal.SIGKILL, "Heaptrack did not finish recording")
        else:
            settled(port, key, expected_usage)
            stop(child)
        check_terminals(log, ids)
        return {"samples_kib": samples, "malloc_bytes": trimmed, "sqlite_mapping": sqlite_mapping,
                "completions": len(ids), "expected_total_key_usage": expected_usage}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--parent-netns", required=True)
    parser.add_argument("--traffic", choices=("session", "catalog", "completion", "idle"), default="session")
    parser.add_argument("--requests", type=int, choices=CHECKPOINTS, default=1000)
    parser.add_argument("--rows", type=int, default=ROWS)
    parser.add_argument("--no-trim", action="store_true")
    parser.add_argument("--heaptrack", action="store_true")
    parser.add_argument("--glibc-tunables", help="Set GLIBC_TUNABLES on the measured process")
    args = parser.parse_args()
    require(0 < args.rows <= ROWS, "Fixture row count outside its supported range")
    args.package = args.package.resolve(strict=True)
    args.out = args.out.resolve()
    require(not args.out.is_relative_to(HERE.parents[3]), "Run output must stay outside the source checkout")
    namespace = verify_namespace(args.parent_netns)
    os.umask(0o077)
    args.out.mkdir(mode=0o700)
    identities = identities_for(args)
    auth = synthetic_auth()
    with tempfile.TemporaryDirectory(prefix="state-", dir=args.out) as directory, upstream(auth["tokens"]["account_id"]) as mock:
        state = Path(directory)
        (state / ".data").mkdir(mode=0o700)
        command = command_for(args, state, mock.server_port, identities)
        environment = {"DENO_TIMELINE": "production", "DENO_DEPLOY_TOKEN": ADMIN_TOKEN}
        seed_env, measure_env = state / "seed", state / "measure"
        seed_env.mkdir(mode=0o700)
        measure_env.mkdir(mode=0o700)
        with process(command, state, {**sparse(seed_env), **environment}, str(args.out / "seed.log")) as (child, log):
            port = wait_port(child, log, LISTEN)
            key, ids, expected_catalog = seed(port, auth)
            release = control(port, "/health")["release"]
            stop(child)
            check_terminals(log, ids)
        db = populate(state / ".data/kv-bun.sqlite3", args.rows)
        before = mock.catalog_calls, mock.completions
        new_env = {**sparse(measure_env), **environment}
        if args.glibc_tunables:
            new_env["GLIBC_TUNABLES"] = args.glibc_tunables
        result = measure(args, state, command, new_env, key, expected_catalog)
        require(mock.failures == 0 and mock.catalog_calls == before[0] and
                mock.completions == before[1] + result["completions"], "Unexpected loopback upstream dispatch")
    models, catalog_bytes, etag = expected_catalog
    receipt = {"package": identities, "release": release, "host": platform.platform(), "network_namespace": namespace,
               "database": db, "request_bytes": len(BODY), "catalog_bytes": catalog_bytes, "models": models, "etag": etag,
               "traffic": args.traffic, "requests": args.requests, "heaptrack": args.heaptrack, "glibc_tunables": args.glibc_tunables,
               **result}
    result_file = args.out / "results.json"
    result_file.write_text(json.dumps(receipt, indent=2) + "\n")
    print(result_file)


if __name__ == "__main__":
    main()
