#!/usr/bin/env python3
"""Run the same declarative HTTP exchanges against the two isolated gateways."""

import argparse
import base64
from collections import defaultdict
from contextlib import contextmanager, ExitStack
import errno
import json
import os
from pathlib import Path
import re
import runpy
import socket
import sys
import time

HERE = Path(__file__).resolve().parent
VOYAGE_CACHE_TOKEN = "uos-synthetic-voyage-cache-only"
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))


def load_cases(path, benchmark):
    document = json.loads(path.read_text())
    if document["version"] != 1:
        raise ValueError("Unsupported case format")
    cases = document["cases"]
    seen = set()
    for case in cases:
        identifier = case["id"]
        if identifier in seen or not re.fullmatch(r"[a-z0-9-]+", identifier):
            raise ValueError("Invalid or duplicate case ID " + identifier)
        seen.add(identifier)
        if not case["path"].startswith(("/", "$")) or case.get("phase", 1) not in (1, 2):
            raise ValueError("Invalid request path or phase in " + identifier)
        if type(case["expect"]["status"]) is not int:
            raise ValueError("Missing status expectation in " + identifier)
        if "expect_bun" in case:
            reference = case["expect_bun"]
            if (case["expect"]["status"] != 400 or case["expect"].get("routes") != []
                    or reference.get("status") != 200 or not reference.get("routes")):
                raise ValueError("A stricter native validation case needs 400 without dispatch and reference 200 with dispatch in " + identifier)
        if "fixture" in case and case["fixture"] not in benchmark["SCENARIOS"]:
            raise ValueError("Unknown benchmark fixture in " + identifier)
        if sum(key in case for key in ("fixture", "raw", "json", "multipart", "ndjson")) > 1:
            raise ValueError("Multiple body specifications in " + identifier)
        if "ndjson" in case and not re.fullmatch(r"[a-z-]+\.ndjson", case["ndjson"]):
            raise ValueError("Invalid NDJSON fixture filename in " + identifier)
    if len(seen) < 80:
        raise ValueError("The conformance matrix must contain at least 80 cases")
    if [case.get("phase", 1) for case in cases] != sorted(case.get("phase", 1) for case in cases):
        raise ValueError("Put restart readbacks after the initial cases")
    return cases


class Gateway:
    def __init__(self, args, benchmark, runtime, identity, credentials, provider_env):
        self.args = args
        self.benchmark = benchmark
        self.runtime = runtime
        self.identity = identity
        self.credentials = credentials
        self.provider_env = provider_env
        self.root = args.out / runtime
        self.root.mkdir()
        (self.root / ".data").mkdir()
        self.variables = {"token_" + alias: token for alias, token in credentials.items()}
        self.ids = {}
        self.request_ids = set()
        self.terminals = defaultdict(list)
        self.launches = []

    @contextmanager
    def launch(self, phase, mock_port):
        tools = self.benchmark
        process_root = self.root / f"process-{phase}"
        process_root.mkdir()
        command = tools["command_for"](self.args, self.runtime, self.root, mock_port, self.identity)
        environment = {**tools["sparse"](process_root), **self.provider_env}
        if phase == 2:
            environment["VOYAGEAI_API_KEY"] = VOYAGE_CACHE_TOKEN
        started = time.monotonic()
        with tools["process"](command, self.root, environment, f"gateway-{phase}.log") as (child, log):
            port = tools["wait_port"](child, log, tools["LISTEN"])
            self.origin = f"http://127.0.0.1:{port}"
            launch = {"phase": phase, "startup_ms": round((time.monotonic() - started) * 1000, 3)}
            self.launches.append(launch)
            yield
            tools["stop"](child)
            launch["exit_code"] = child.returncode
            launch["wall_ms"] = round((time.monotonic() - started) * 1000, 3)
        prefix = tools["TERMINAL_PREFIX"]
        with log.open() as stream:
            for line in stream:
                if line.startswith(prefix):
                    terminal = json.loads(line[len(prefix):])
                    self.terminals[terminal["request_id"]].append(terminal)

    def run(self, case, mock):
        from compare import comparable_headers, expected_for, lookup, MISSING, OPAQUE_ID
        from wire import event_kind, expand, request

        spec = expand(case, self.variables)
        point = mock.begin(spec)
        gate = mock.gate

        def acknowledge(event):
            if event_kind(event) in ("chat.content", "response.output_text.delta"):
                gate.set()

        deadline = time.monotonic() + (2 if spec.get("settle") else 0)
        expected = expected_for(spec, self.runtime)
        checks = expected.get("values", {})
        while True:
            response = request(self.origin, spec, self.credentials, acknowledge if gate else None)
            retry = (response["status"] == expected["status"] and not response.get("wire_error")
                     and any(lookup(response, path) != expected for path, expected in checks.items()))
            if not retry or time.monotonic() >= deadline:
                break
            time.sleep(0.025)
        response.update(mock.observe(point))
        response["headers"] = comparable_headers(response["headers"])
        for name, pointer in spec.get("capture", {}).items():
            value = lookup(response, pointer)
            self.variables[name] = value if isinstance(value, str) else f"missing-{name}"
            if isinstance(value, str) and OPAQUE_ID.fullmatch(value):
                self.ids[value] = f"<{name}>"
            if value is MISSING or not isinstance(value, str) or not value:
                response["capture_error"] = f"No string at {pointer} for {name}"
        request_ids = response["headers"].get("x-uos-request-id", [])
        if len(request_ids) == 1:
            request_id = request_ids[0]
            if request_id in self.request_ids:
                response["capture_error"] = "Gateway reused its request ID"
            self.request_ids.add(request_id)
        return spec, response


def read_terminals(gateway, result):
    identifiers = result["headers"].get("x-uos-request-id", [])
    rows = gateway.terminals.get(identifiers[0], []) if len(identifiers) == 1 else []
    fields = ("status", "delivery_outcome", "stream_terminal_type", "input_tokens", "output_tokens",
              "total_tokens", "provider", "provider_request_id", "api_key_usage_recorded", "paid_fallback_usage_recorded")
    result["telemetry"] = [{name: row[name] for name in fields if name in row} for row in rows]


def verify_network(benchmark, parent):
    current = benchmark["verify_namespace"](parent)
    denied = {}
    for family, address in [(socket.AF_INET, "192.0.2.1"), (socket.AF_INET6, "2001:db8::1")]:
        with socket.socket(family, socket.SOCK_DGRAM) as probe:
            outcome = probe.connect_ex((address, 443))
        if outcome not in (errno.ENETUNREACH, errno.EHOSTUNREACH):
            raise RuntimeError("Network boundary did not deny " + address)
        denied[address] = errno.errorcode[outcome]
    return {"namespace": current, "parent": parent, "route_denial": denied}


def run(args):
    benchmark = runpy.run_path(str(args.bench / "run.py"))
    sys.path.insert(0, str(HERE))
    smoke = args.source / "cpp/gateway/tests/production_smoke"
    sys.path.insert(2, str(smoke))
    from cases import synthetic_auth
    from compare import case_outcome, compare, expectations, markdown, redact
    from fixtures import ADMIN_TOKEN, API_TOKEN, PROVIDER_TOKEN
    from network import write_json
    from providers import Upstream

    case_specs = load_cases(HERE / "cases.json", benchmark)
    network = verify_network(benchmark, args.parent_netns)
    identities = benchmark["identities_for"](args)
    credentials = {"primary": API_TOKEN, "admin": ADMIN_TOKEN, "other": "u_" + "c" * 64,
                   "quota": "u_" + "d" * 64, "unknown": "u_" + "e" * 64, "fanout": "u_" + "f" * 64,
                   "reader": "uos-conformance-model-reader"}
    auth = synthetic_auth()
    codex_auth = base64.b64encode(json.dumps(auth, separators=(",", ":")).encode()).decode()
    provider_env = {"DENO_TIMELINE": "production", "DENO_DEPLOY_TOKEN": ADMIN_TOKEN, "UOS_AI_TOKEN": credentials["reader"],
                    "DEEPSEEK_API_KEY": PROVIDER_TOKEN, "CODEX_AUTH_JSON_B64": codex_auth}
    secrets = (*credentials.values(), PROVIDER_TOKEN, VOYAGE_CACHE_TOKEN, codex_auth,
               *[auth["tokens"][name] for name in ("access_token", "refresh_token")])
    gateways = {name: Gateway(args, benchmark, name, identities, credentials, provider_env) for name in ("bun", "native")}
    receipt = {"schema": 1, "source_revision": args.source_revision, "source_tree": args.source_tree, "suite_revision": args.suite_revision,
               "suite_tree": args.suite_tree, "binaries_sha256": identities, "network": network,
               "benchmark_revision": args.benchmark_revision,
               "benchmark_sha256": {path.name: benchmark["digest"](path) for path in sorted(args.bench.iterdir()) if path.is_file()},
               "oracle_smoke_sha256": {path.name: benchmark["digest"](path) for path in sorted(smoke.iterdir()) if path.is_file()},
               "fixture_sha256": {path.name: benchmark["digest"](path) for path in sorted(HERE.iterdir()) if path.is_file()},
               "launches": {name: gateway.launches for name, gateway in gateways.items()}, "cases": []}
    started = time.monotonic()
    observations = {}
    with Upstream(auth["tokens"]["access_token"]) as mock:
        if mock.server_port in (7999, 8000):
            raise RuntimeError("Upstream was assigned a reserved port")
        for phase in (1, 2):
            selected = [case for case in case_specs if case.get("phase", 1) == phase]
            with ExitStack() as stack:
                for gateway in gateways.values():
                    stack.enter_context(gateway.launch(phase, mock.server_port))
                print(f"Phase {phase}: {len(selected)} cases against both gateways", flush=True)
                for case in selected:
                    observations[case["id"]] = {name: gateway.run(case, mock) for name, gateway in gateways.items()}
            for case in selected:
                pair = observations.pop(case["id"])
                expected, captures = {}, {}
                for name, (spec, result) in pair.items():
                    if case.get("log"):
                        read_terminals(gateways[name], result)
                    expected[name] = expectations(spec, result, name)
                    if result.get("capture_error"):
                        expected[name].append({"path": "/capture", "expected": "valid identity", "actual": result["capture_error"]})
                    captures[name] = result
                changes = compare(case, captures["bun"], captures["native"], {name: gateway.ids for name, gateway in gateways.items()})
                receipt["cases"].append(redact({"id": case["id"], "observations": captures, "expectation_failures": expected,
                                               "outcome": case_outcome(case, changes, expected),
                                               "differences": changes}, secrets))
            write_json(args.out / "results.partial.json", receipt)

    counts = {"match": 0, "cosmetic": 0, "functional": 0, "native_stricter": 0, "differential_functional": 0, "shared_expectation_failures": 0}
    for row in receipt["cases"]:
        classes = {item["classification"] for item in row["differences"]}
        failures = row["expectation_failures"]
        outcome = row["outcome"]
        counts["differential_functional"] += outcome != "native_stricter" and ("FUNCTIONAL" in classes or failures["bun"] != failures["native"])
        counts["shared_expectation_failures"] += bool(failures["bun"]) and failures["bun"] == failures["native"]
        counts[outcome] += 1
    receipt.update(counts=counts, wall_s=round(time.monotonic() - started, 3))
    write_json(args.out / "results.json", receipt)
    (args.out / "results.partial.json").unlink()
    (args.out / "table.md").write_text(markdown(receipt))
    print(json.dumps({"cases": len(receipt["cases"]), **counts, "wall_s": receipt["wall_s"], "receipt": str(args.out / "results.json")}), flush=True)
    return 1 if counts["functional"] else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("bench", "source", "package", "binding", "bun", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("parent-netns", "source-revision", "source-tree", "suite-revision", "suite-tree", "benchmark-revision"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    for name in ("bench", "source", "package", "binding", "bun"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    args.out = args.out.resolve()
    if args.out.is_relative_to(HERE.parents[2]) or args.out.is_relative_to(args.source):
        parser.error("Run output must be outside the source trees")
    os.umask(0o077)
    args.out.mkdir(mode=0o700)
    try:
        return run(args)
    except (OSError, RuntimeError, ValueError, KeyError, socket.timeout) as error:
        print(f"Conformance fixture failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
