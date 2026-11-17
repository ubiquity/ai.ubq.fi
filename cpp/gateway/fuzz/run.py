#!/usr/bin/python3
"""Run one already-built parser harness in a finite, network-isolated campaign."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys

sys.dont_write_bytecode = True
GATEWAY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GATEWAY / "tools"))
from build_support import GIB, WORKSPACE, admission, atomic_json, digest, local_inputs, measured

EVIDENCE = Path("/tmp/claude-1002/prod/analysis_fuzz")
TOOL_ROOT = Path("/home/codex/toolchains/uos-cpp-2026.11")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target")
    parser.add_argument("--build", type=Path, default=WORKSPACE / ".data/cpp-gateway-native-fuzz-clang")
    parser.add_argument("--label", required=True)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--reproducer", type=Path)
    args = parser.parse_args()
    policy_path = GATEWAY / "fuzz/targets.json"
    policy = json.loads(policy_path.read_text())
    if args.target not in policy["targets"] or not 1 <= args.seconds <= 60:
        parser.error("known target and 1..60 seconds are required")
    target = policy["targets"][args.target]
    binary = args.build.resolve() / ("uos_fuzz_" + args.target)
    if not binary.is_file():
        raise RuntimeError("Build the real sanitizer-linked harness first: " + str(binary))
    owner = args.build / "profile-owner.json"
    environment = args.build / "profile-environment.json"
    profile = json.loads(owner.read_text())
    if profile["profile"] != "fuzz-clang" or profile["sanitizer"] != "address,undefined":
        raise RuntimeError("Campaign requires the exact qualified fuzz-clang profile")
    run_root = WORKSPACE / ".data/cpp-gateway-fuzz-runs" / args.label
    if run_root.exists():
        raise RuntimeError("Campaign labels are immutable; choose a new label")
    for name in ("h", "t", "w", "corpus", "artifacts"):
        (run_root / name).mkdir(parents=True)
    seeds = GATEWAY / "fuzz/corpus" / args.target
    seed_hashes = {path.name: digest(path) for path in sorted(seeds.iterdir()) if path.is_file()}
    for path in seeds.iterdir():
        if path.is_file():
            shutil.copyfile(path, run_root / "corpus" / path.name)
    inputs = {}
    for source in [target["harness"]] + policy["common_sources"]:
        inputs.update(local_inputs(GATEWAY / source))
    env = {"PATH": str(TOOL_ROOT / "bin") + ":/usr/bin:/bin", "HOME": str(run_root / "h"),
           "TMPDIR": str(run_root / "t"), "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8", "TZ": "UTC",
           "ASAN_SYMBOLIZER_PATH": str(TOOL_ROOT / "LLVM-23.1.5-Linux-ARM64/bin/llvm-symbolizer"),
           "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1:allocator_may_return_null=0",
           "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
    command = ["/usr/bin/unshare", "--user", "--map-root-user", "--net", str(binary),
               "-max_len=" + str(target["max_len"]), "-timeout=3", "-rss_limit_mb=768", "-malloc_limit_mb=128",
               "-seed=17112026", "-print_final_stats=1", "-print_coverage=1",
               "-artifact_prefix=" + str(run_root / "artifacts") + "/"]
    if args.reproducer:
        command += ["-runs=1", str(args.reproducer.resolve())]
    else:
        command += ["-max_total_time=" + str(args.seconds), str(run_root / "corpus")]
    before = admission("runtime", EVIDENCE.parents[1] / "codex/resources.log")
    log = EVIDENCE / (args.label + ".log")
    if log.exists():
        raise RuntimeError("Refusing to replace original campaign evidence")
    with log.open("wb") as output:
        # ASan shadow mapping needs uncapped address space. libFuzzer's RSS limit,
        # measured host-reserve enforcement and a finite process group bound real use.
        code, receipt = measured(command, run_root / "w", env, EVIDENCE / "commands", args.label,
                                 timeout=args.seconds + 15, output=output)
    after = {}
    for source in [target["harness"]] + policy["common_sources"]:
        after.update(local_inputs(GATEWAY / source))
    text = log.read_text(errors="replace")
    stat = lambda name: next(iter(re.findall(r"stat::" + name + r":\s*(\d+)", text)), None)
    coverage = re.findall(r"#(\d+)\s+(?:DONE|INITED).*?cov:\s*(\d+).*?ft:\s*(\d+)", text)
    result = {"target": args.target, "code": code, "log": str(log), "receipt": str(receipt),
              "binary": str(binary), "binary_sha256": digest(binary), "profile_sha256": digest(owner),
              "environment_sha256": digest(environment), "policy_sha256": digest(policy_path),
              "source_sha256": inputs, "changed_inputs": sorted(name for name in inputs.keys() | after.keys() if inputs.get(name) != after.get(name)),
              "seed_sha256": seed_hashes, "seed_count": len(seed_hashes), "seed": 17112026,
              "runs": stat("number_of_executed_units"), "new_units": stat("new_units_added"),
              "peak_rss_mb": stat("peak_rss_mb"), "coverage": coverage,
              "corpus_count": len(list((run_root / "corpus").iterdir())),
              "artifacts": {str(path): digest(path) for path in sorted((run_root / "artifacts").iterdir()) if path.is_file()},
              "asan_ubsan": "finding" if any(marker in text for marker in ("ERROR: AddressSanitizer", "runtime error:", "ERROR: LeakSanitizer")) else "none reported",
              "timeout_or_oom": any(marker in text for marker in ("ERROR: libFuzzer: timeout", "ERROR: libFuzzer: out-of-memory")),
              "admission": before, "namespace": "fresh user+network namespace, no interfaces raised", "settled": True}
    atomic_json(EVIDENCE / (args.label + ".json"), result)
    print(json.dumps({name: result[name] for name in ("target", "code", "runs", "coverage", "peak_rss_mb", "corpus_count", "artifacts", "asan_ubsan", "timeout_or_oom", "receipt")}, indent=2))
    return code or (3 if result["changed_inputs"] else 0)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print("gateway fuzz: " + str(error), file=sys.stderr)
        raise SystemExit(3)
