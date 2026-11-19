#!/usr/bin/python3
"""Run finite, isolated native regex benchmarks and retain every sample."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from build_support import measured as native_measured


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_inputs(workspace):
    gateway = workspace / "cpp/gateway"
    pending = [gateway / "src/codex/supervisor_secret.cpp"]
    pending += [gateway / "src/rt" / (name + ".cpp") for name in ("json", "number", "string", "regex", "regex_compile", "regex_match", "regex_tables")]
    found = set()
    while pending:
        path = pending.pop()
        if path in found:
            continue
        found.add(path)
        for include in re.findall(r'^\s*#include\s+"([^\"]+)"', path.read_text(), re.MULTILINE):
            candidates = (path.parent / include, gateway / "include" / include, gateway / include)
            selected = next((candidate.resolve() for candidate in candidates if candidate.is_file()), None)
            if selected is not None and selected not in found:
                pending.append(selected)
    return sorted(found)


def build_identity(binary, sources):
    profile = binary.parent / "profile-owner.json"
    commands = binary.parent / "compile_commands.json"
    if not profile.is_file() or not commands.is_file():
        raise RuntimeError("Benchmark binary lacks a qualified build profile/compilation database")
    selected = {str(path.resolve()) for path in sources if path.suffix == ".cpp"}
    entries = sorted((row for row in json.loads(commands.read_text()) if str(Path(row["file"]).resolve()) in selected), key=lambda row: row["file"])
    if {str(Path(row["file"]).resolve()) for row in entries} != selected:
        raise RuntimeError("Compilation database does not cover every benchmark source")
    return {"profile_owner": json.loads(profile.read_text()), "profile_owner_sha256": digest(profile), "compile_commands": entries}


def snapshot():
    memory = dict(line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines())
    return {"loadavg": Path("/proc/loadavg").read_text().strip(), "memavailable_kib": int(memory["MemAvailable"].split()[0])}


def measured(command, label, output, scratch, timeout):
    env = {"PATH": "/usr/bin:/bin:/usr/local/bin", "HOME": str(scratch / "h"), "TMPDIR": str(scratch / "t"),
           "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8", "TZ": "UTC"}
    wrapped = ["/usr/bin/unshare", "--user", "--map-root-user", "--net", "--"] + command
    before = snapshot()
    if before["memavailable_kib"] < 2500 * 1024:
        raise RuntimeError("Insufficient physical memory for benchmark admission")
    started = time.monotonic()
    with (output / (label + ".log")).open("w") as log:
        code, process_receipt = native_measured(wrapped, scratch / "w", env, output, label, timeout=timeout, output=log)
    receipt = {"command": wrapped, "cwd": str(scratch / "w"), "environment_names": list(env), "code": code,
               "duration_s": time.monotonic() - started, "before": before, "after": snapshot(), "process_receipt": str(process_receipt),
               "stream_capture": "stdout and stderr combined in .log"}
    (output / (label + ".receipt.json")).write_text(json.dumps(receipt, indent=2) + "\n")
    if code:
        raise RuntimeError(label + " failed; see retained log/process receipt")
    process = json.loads(process_receipt.read_text())
    if not process["settled"] or process["failure"] or process["max_rss_kib"] is None:
        raise RuntimeError(label + " lacks a successful settled wait4 receipt")
    result = json.loads((output / (label + ".log")).read_text())
    result["max_rss_kib"] = process["max_rss_kib"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    if not 1 <= args.samples <= 100 or not 0 <= args.warmups <= 100:
        raise RuntimeError("Invalid sample or warmup bounds")
    workspace = Path(__file__).resolve().parents[4]
    binary = args.binary.resolve()
    if workspace / ".data" not in binary.parents or not binary.is_file():
        raise RuntimeError("Native binary must exist in this worktree's .data")
    output = args.output.resolve()
    if not (Path("/tmp/claude-1002/prod/idna_regex") in output.parents or workspace / ".data" in output.parents):
        raise RuntimeError("Benchmark output must be task/worktree scoped")
    output.mkdir(parents=True, exist_ok=False)
    scratch = output / "scratch"
    for name in ("h", "t", "w"):
        (scratch / name).mkdir(parents=True)
    manifest = args.manifest.resolve()
    sources = [Path(__file__).resolve(), Path(__file__).with_name("regex_bench.cpp"), manifest,
               workspace / "cpp/gateway/tools/build_support.py"] + source_inputs(workspace)
    inputs = {str(path): digest(path) for path in sources}
    identity = {"native_binary": str(binary), "native_sha256": digest(binary), "inputs_sha256": inputs,
                "samples": args.samples, "warmups": args.warmups, "p95_method": "nearest rank; seven samples yields maximum",
                "rss_scope": "whole child maximum resident set including startup, fixture load and runtime", "before": snapshot()}
    compiled_sources = [Path(__file__).with_name("regex_bench.cpp")] + source_inputs(workspace)
    identity["build"] = build_identity(binary, compiled_sources)
    (output / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    fixtures = json.loads(manifest.read_text())
    if args.case:
        fixtures = [fixture for fixture in fixtures if fixture["name"] in args.case]
        if {fixture["name"] for fixture in fixtures} != set(args.case):
            raise RuntimeError("Unknown requested benchmark case")
    rows = []
    for fixture in fixtures:
        command = [str(binary), str(manifest), fixture["name"], str(args.samples), str(args.warmups), str(fixture["iterations"])]
        result = measured(command, fixture["name"], output, scratch, args.timeout)
        samples = result["samples_ms"]
        median = statistics.median(samples)
        size = len(fixture["input"].encode("utf8"))
        row = {"case": fixture["name"], "bytes": size, "samples_ms": samples, "median_ms": median,
               "p95_ms": sorted(samples)[math.ceil(len(samples) * .95) - 1],
               "operations_per_second": 1000 / median, "bytes_per_second": size * 1000 / median,
               "max_rss_kib": result["max_rss_kib"]}
        rows.append(row)
        print(json.dumps({"case": row["case"], "median_ms": median}), flush=True)
        (output / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    changed = [str(path) for path in sources if digest(path) != inputs[str(path)]]
    if changed or digest(binary) != identity["native_sha256"] or build_identity(binary, compiled_sources) != identity["build"]:
        raise RuntimeError("Benchmark consumed changing inputs: " + ", ".join(changed))
    identity["after"] = snapshot()
    identity["unchanged_inputs"] = True
    (output / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
