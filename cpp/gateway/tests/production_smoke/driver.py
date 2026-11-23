"""Bounded smoke orchestration; no gateway runs without an explicit reviewed startup protocol."""

import argparse
import json
import os
import sys
import time
import traceback
from pathlib import Path

from network import WORKTREE, qualify, require_task_path, run_namespace, sha256, source_inputs, write_json


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--preflight", action="store_true", help="Prove OS network isolation without launching an application")
    result.add_argument("--fixture-self-test", action="store_true", help="Validate the new loopback client/upstream only, without a gateway")
    result.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    result.add_argument("--parent-namespace", help=argparse.SUPPRESS)
    result.add_argument("--output", type=Path, required=True)
    result.add_argument("--gateway", type=Path)
    result.add_argument("--gateway-sha256")
    result.add_argument("--build-receipt", type=Path, help="Exact primary build/profile identity receipt supplied by the build owner")
    result.add_argument("--fixture-flag", help="Exact startup flag agreed with the build/runtime owners; no default")
    result.add_argument("--runtime-template", type=Path, help="Reviewed runtime fixture document whose inputs stay task-scoped")
    result.add_argument("--benchmark", action="store_true")
    result.add_argument("--json-benchmark", type=Path, help="Run only the qualified production JSON hot-path target")
    result.add_argument("--json-sha256", help="Accepted primary JSON benchmark binary hash")
    result.add_argument("--timeout", type=int, default=180)
    return result


def inside(arguments):
    output = require_task_path(arguments.output)
    result = {"schema": 1, "contract": "v7", "started_ns": time.time_ns(), "source_root": str(WORKTREE),
              "harness_sha256": {str(path.relative_to(WORKTREE)): sha256(path)
                                 for path in sorted((WORKTREE / "cpp/gateway/tests/production_smoke").glob("*.py"))}}
    status = 0
    before = source_inputs()
    result["source_inputs_sha256"] = before
    if arguments.build_receipt:
        result["build_receipt"] = {"path": str(arguments.build_receipt), "sha256": sha256(arguments.build_receipt)}
    try:
        result["network"] = qualify(arguments.parent_namespace)
        if arguments.preflight:
            result["outcome"] = "network_preflight_passed"
        elif arguments.fixture_self_test:
            from client import request, websocket
            from upstream import Upstream
            upstream = Upstream()
            try:
                gate = upstream.hold_next()
                streamed = request(upstream.origin, "/chat/completions", body={"model": "deepseek-flash", "stream": True},
                                   on_delta=lambda _: gate.set())
                if streamed["status"] != 200 or streamed["events"][-1]["data"] != "[DONE]" or not gate.is_set():
                    raise RuntimeError("Fixture SSE self-test did not complete progressively")
                buffered = request(upstream.origin, "/chat/completions", body={"model": "deepseek-flash", "stream": False})
                if buffered["status"] != 200 or buffered["json"]["usage"]["total_tokens"] != 16:
                    raise RuntimeError("Fixture buffered self-test failed")
                upgraded = websocket(upstream.origin, "/live/rtc_fixture", relay=True)
                if upgraded["status"] != 101:
                    raise RuntimeError("Fixture WebSocket self-test failed")
                result["fixture_self_test"] = {"streamed": streamed, "buffered": buffered, "websocket": upgraded,
                                               "upstream_calls": upstream.calls, "websocket_frames": upstream.websocket_frames,
                                               "upstream_failures": upstream.failures}
                result["outcome"] = "fixture_self_test_passed"
            finally:
                upstream.close()
        elif arguments.json_benchmark:
            sys.path.insert(0, str(WORKTREE / "cpp/gateway/bench/paths"))
            from run_json import run
            result["json_benchmark"] = run(arguments.json_benchmark, output / "json")
            result["outcome"] = "passed"
        else:
            from cases import run
            result.update(run(arguments))
            result["outcome"] = "passed"
    except Exception as error:
        status = 1
        result.update({"outcome": "failed", "error": str(error), "traceback": traceback.format_exc()})
    result["finished_ns"] = time.time_ns()
    after = source_inputs()
    changed = sorted(path for path in before.keys() | after.keys() if before.get(path) != after.get(path))
    result["changed_source_inputs"] = changed
    if changed:
        result["outcome"] = "source_changed_during_probe"
        status = 1
    write_json(output / "result.json", result)
    print(json.dumps({"outcome": result["outcome"], "result": str(output / "result.json"), "error": result.get("error")}), flush=True)
    return status


def main():
    arguments = parser().parse_args()
    arguments.output = require_task_path(arguments.output)
    if arguments.build_receipt:
        arguments.build_receipt = require_task_path(arguments.build_receipt).resolve(strict=True)
    if not 5 <= arguments.timeout <= 600:
        raise RuntimeError("Harness timeout must be from 5 through 600 seconds")
    if arguments.inside:
        return inside(arguments)
    if arguments.json_benchmark:
        if not arguments.build_receipt or not arguments.json_sha256:
            raise RuntimeError("JSON benchmark requires the primary build receipt and accepted native binary hash")
        arguments.json_benchmark = arguments.json_benchmark.resolve(strict=True)
        if not arguments.json_benchmark.is_relative_to(WORKTREE / ".data"):
            raise RuntimeError("JSON target must be the worktree's qualified build output")
        accepted = json.loads(arguments.build_receipt.read_text())
        expected = accepted.get("artifacts", {}).get("uos-json-bench", {}).get("sha256")
        if accepted.get("changed_inputs") != [] or expected != arguments.json_sha256 or sha256(arguments.json_benchmark) != expected:
            raise RuntimeError("JSON binary has no matching immutable, current-input build receipt")
        owner_path = arguments.json_benchmark.parent / "profile-owner.json"
        if sha256(owner_path) != accepted.get("profile_owner_sha256"):
            raise RuntimeError("JSON build profile owner changed after qualification")
        owner = json.loads(owner_path.read_text())
        if owner.get("profile") != "release" or owner.get("compiler") != "/usr/bin/g++" or owner.get("sanitizer") is not None:
            raise RuntimeError("JSON measurements require the selected native GCC release profile")
        if sha256(owner["compiler"]) != owner.get("compiler_sha256"):
            raise RuntimeError("Qualified JSON compiler identity changed")
    elif not (arguments.preflight or arguments.fixture_self_test):
        if not arguments.gateway or not arguments.gateway_sha256 or not arguments.fixture_flag or not arguments.runtime_template or not arguments.build_receipt:
            raise RuntimeError("Runtime launch requires binary/hash, reviewed fixture flag and runtime template")
        arguments.gateway = arguments.gateway.resolve(strict=True)
        arguments.runtime_template = arguments.runtime_template.resolve(strict=True)
        if not arguments.gateway.is_relative_to(WORKTREE / ".data"):
            raise RuntimeError("Only the task build's primary gateway binary may run")
        if sha256(arguments.gateway) != arguments.gateway_sha256:
            raise RuntimeError("Gateway binary does not match its accepted build hash")
        require_task_path(arguments.runtime_template)
    arguments.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    command = ["/usr/bin/python3", str(WORKTREE / "cpp/gateway/tools/smoke.py"), *sys.argv[1:],
               "--inside", "--parent-namespace", os.readlink("/proc/self/ns/net")]
    receipt = run_namespace(command, arguments.output, arguments.timeout)
    print(json.dumps({"namespace_exit_code": receipt["exit_code"], "settled": receipt["settled"],
                      "receipt": str(arguments.output / "namespace-receipt.json")}), flush=True)
    return 0 if receipt["exit_code"] == 0 and not receipt["timed_out"] else 1
