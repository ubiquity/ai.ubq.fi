#!/usr/bin/python3
"""Bounded, source-bound clang-tidy execution for the native gateway profiles."""
import argparse
import json
import os
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
from build_support import GATEWAY, GIB, WORKSPACE, acquire_slots, admission, atomic_json, digest, local_inputs, measured

TOOL_ROOT = Path("/home/codex/toolchains/uos-cpp-2026.11")
TIDY = TOOL_ROOT / "bin/clang-tidy"
EVIDENCE = Path("/tmp/claude-1002/prod/analysis_fuzz")
RESOURCE_LOG = Path("/tmp/claude-1002/codex/resources.log")
POLICY = GATEWAY / "analysis/checks.json"


def environment():
    scratch = WORKSPACE / ".data/cpp-gateway-analysis"
    for name in ("h", "t", "w"):
        (scratch / name).mkdir(parents=True, exist_ok=True)
    return {"PATH": str(TOOL_ROOT / "bin") + ":/usr/bin:/bin", "HOME": str(scratch / "h"),
            "TMPDIR": str(scratch / "t"), "TZ": "UTC", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8",
            "PYTHONPYCACHEPREFIX": str(WORKSPACE / ".data/cpp-gateway-pycache")}


def run(command, label, timeout=180, source=None):
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    locks = acquire_slots("compile")
    try:
        before = admission("compile", RESOURCE_LOG)
        inputs = local_inputs(source) if source else {}
        log = EVIDENCE / (label + ".log")
        if log.exists():
            raise RuntimeError("Evidence already exists; select a new --label: " + label)
        with log.open("wb") as output:
            code, receipt = measured(command, GATEWAY, environment(), EVIDENCE / "commands", label,
                                     timeout=timeout, address_limit=2 * GIB, low_priority=True, output=output)
        result = json.loads(receipt.read_text())
        after = local_inputs(source) if source else {}
        result.update(log=str(log), input_sha256=inputs, changed_inputs=sorted(
            name for name in inputs.keys() | after.keys() if inputs.get(name) != after.get(name)),
            tidy_sha256=digest(TIDY.resolve()), policy_sha256=digest(POLICY), admission=before)
        atomic_json(receipt, result)
        if result["changed_inputs"]:
            raise RuntimeError("Analysis consumed changing inputs: " + str(receipt))
        return code, log, receipt
    finally:
        for lock in locks:
            lock.close()


def inventory(label):
    code, log, receipt = run([str(TIDY), "--list-checks", "--checks=*"], label)
    supported = sorted(set(re.findall(r"^\s+([a-z][A-Za-z0-9_.-]+)\s*$", log.read_text(), re.M)))
    policy = json.loads(POLICY.read_text())
    wanted = policy["checks"]
    selected = [check for check in wanted if check in supported]
    selected += [check for check in supported if check.startswith("clang-analyzer-") and "debug" not in check]
    missing = sorted(set(wanted) - set(supported))
    result = {"code": code, "tidy": str(TIDY.resolve()), "tidy_sha256": digest(TIDY.resolve()),
              "supported": supported, "checks": sorted(set(selected)), "unsupported_requested": missing,
              "policy_sha256": digest(POLICY), "receipt": str(receipt), "log": str(log)}
    atomic_json(EVIDENCE / (label + ".json"), result)
    print(json.dumps({key: value for key, value in result.items() if key != "supported"}, indent=2))
    return code


def analyze(options):
    inventory_path = Path(options.inventory)
    selection = json.loads(inventory_path.read_text())
    if selection["tidy_sha256"] != digest(TIDY.resolve()) or selection["policy_sha256"] != digest(POLICY):
        raise RuntimeError("Analysis tool/policy identity changed after inventory")
    checks = "-*," + ",".join(selection["checks"])
    source = (GATEWAY / options.source).resolve()
    if GATEWAY not in source.parents or not source.is_file():
        raise RuntimeError("Source must be a real gateway file")
    command = [str(TIDY), str(source), "--checks=" + checks, "--warnings-as-errors=*",
               "--header-filter=.*", "--quiet", "--format-style=none",
               "--export-fixes=" + str(EVIDENCE / (options.label + ".diagnostics.yaml"))]
    if options.database:
        command += ["-p", str(Path(options.database).resolve())]
    else:
        command += ["--", "-std=c++20", "-DUOS_GATEWAY_NATIVE_LIBRARIES=1", "-DBOOST_ASIO_HAS_CO_AWAIT=1",
                    "-I" + str(GATEWAY / "include"), "-I" + str(GATEWAY),
                    "-isystem", str(TOOL_ROOT / "boost-1.92.0")]
    code, log, receipt = run(command, options.label, options.timeout, source)
    diagnostics = []
    for line in log.read_text(errors="replace").splitlines():
        match = re.match(r"(.+):(\d+):(\d+): (warning|error): (.*?) \[([^]]+)\]$", line)
        if match:
            diagnostics.append(dict(zip(("path", "line", "column", "severity", "message", "check"), match.groups())))
    result = {"code": code, "source": str(source), "source_sha256": digest(source),
              "inventory_sha256": digest(inventory_path), "checks": selection["checks"],
              "diagnostics": diagnostics, "log": str(log), "receipt": str(receipt),
              "classification": "unclassified" if diagnostics else "clean" if code == 0 else "frontend-or-tool-failure"}
    atomic_json(EVIDENCE / (options.label + ".json"), result)
    print(json.dumps(result, indent=2))
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("inventory", "analyze"))
    parser.add_argument("--label", required=True)
    parser.add_argument("--source")
    parser.add_argument("--database")
    parser.add_argument("--inventory")
    parser.add_argument("--timeout", type=int, default=180)
    options = parser.parse_args()
    if options.timeout < 1 or options.timeout > 300:
        parser.error("timeout must be 1..300 seconds")
    return inventory(options.label) if options.operation == "inventory" else analyze(options)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print("gateway analysis: " + str(error), file=sys.stderr)
        raise SystemExit(3)
