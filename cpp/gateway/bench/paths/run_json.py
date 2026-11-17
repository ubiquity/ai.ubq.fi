"""Interleaved finite JSON trials. Called inside the smoke OS network boundary."""

import json
import math
import statistics
import subprocess
import time
from pathlib import Path

from network import WORKTREE, sha256, sparse_environment, write_json


def summary(values):
    ordered = sorted(values)
    return {"median_us": statistics.median(values), "p95_us": ordered[math.ceil(0.95 * len(ordered)) - 1],
            "throughput_per_second": 1000000 / statistics.mean(values)}


def run(native, bun, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    result = {"schema": 1, "native": {"path": str(native), "sha256": sha256(native)},
              "bun": {"path": str(bun), "sha256": sha256(bun)}, "trials": [], "inputs": []}
    script = WORKTREE / "cpp/gateway/bench/paths/json.ts"
    source = WORKTREE / "cpp/gateway/bench/paths/json.cpp"
    result["sources"] = {str(path): sha256(path) for path in (script, source, Path(__file__))}
    for target, iterations in ((1024, 2000), (16384, 200), (1048576, 4)):
        rows = [{"id": index, "text": "fixture-ascii-" + "abcdefghijklmnop" * 4, "value": 1.25,
                 "enabled": True, "items": [1, 2, None, "fixture"]} for index in range(math.ceil(target / 160))]
        text = json.dumps({"rows": rows}, separators=(",", ":"))
        path = output / (str(target) + ".json")
        path.write_text(text)
        result["inputs"].append({"path": str(path), "sha256": sha256(path), "bytes": len(text.encode()), "target_bytes": target})
        for operation in ("parse", "stringify"):
            comparable = {}
            # Alternate which runtime goes first; every pair uses the same exact input.
            for round_ in range(3):
                order = ("native", "bun") if round_ % 2 == 0 else ("bun", "native")
                for runtime in order:
                    tail = [str(path), operation, str(iterations), "9"]
                    command = [str(native), *tail] if runtime == "native" else [str(bun), "--no-env-file", "--no-install",
                               "--config=" + str(WORKTREE / "ops/runtime-bunfig.toml"), str(script), *tail]
                    started = time.perf_counter_ns()
                    child = subprocess.run(command, cwd=output, env=sparse_environment(output), stdin=subprocess.DEVNULL,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False)
                    name = f"{target}-{operation}-{round_}-{runtime}"
                    (output / (name + ".stdout")).write_bytes(child.stdout)
                    (output / (name + ".stderr")).write_bytes(child.stderr)
                    if child.returncode != 0:
                        raise RuntimeError("JSON trial failed: " + name + " exit " + str(child.returncode))
                    measured = json.loads(child.stdout)
                    comparable[runtime] = (measured["canonical_bytes"], measured["canonical_checksum"], measured["consumed"])
                    samples = [value / iterations for value in measured["samples_us"]]
                    result["trials"].append({"name": name, "runtime": runtime, "command": command, "exit_code": child.returncode,
                                             "input_sha256": sha256(path), "target_bytes": target, "operation": operation,
                                             "cold_us": samples[0], "warm": summary(samples[1:]), "raw": measured,
                                             "process_wall_us": (time.perf_counter_ns() - started) / 1000})
                if comparable.get("native") != comparable.get("bun"):
                    raise RuntimeError("Native/Bun JSON canonical output or observable work mismatch")
    write_json(output / "json-results.json", result)
    return result
