#!/usr/bin/python3
"""Run unchanged Links tests with credential, network, process and memory isolation."""
import argparse
import collections
import contextlib
import datetime
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from build_support import GATEWAY, GIB, WORKSPACE, admission, atomic_json, bind_instruction_snapshot, digest, local_inputs, measured, supervisor
from ctest_support import execute, lock, runtime_lock

SUMMARY = re.compile(r"^(\d+) run, (\d+) passed, (\d+) failed$", re.M)
NETWORK = re.compile(r"^UOS_NETWORK_RECEIPT (.+)$", re.M)


def generator():
    spec = importlib.util.spec_from_file_location("uos_generate_tests", GATEWAY / "tools/generate-tests.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_registry(path):
    registry = json.loads(path.read_text())
    if registry.get("schema") != 1 or registry.get("source_root") != str(GATEWAY):
        raise RuntimeError("Registry schema or source-root identity mismatch")
    if registry.get("test_count") != len(registry["tests"]):
        raise RuntimeError("Registry test count mismatch")
    if len({item["target"] for item in registry["tests"]}) != len(registry["tests"]):
        raise RuntimeError("Duplicate test target identity")
    return registry


def profile_identity(path):
    owner = json.loads(path.read_text())
    if Path(owner["source_root"]).resolve() != GATEWAY:
        raise RuntimeError("Profile belongs to a foreign source root")
    if not isinstance(owner.get("runtime_prefix"), list):
        raise RuntimeError("Profile owner must declare its exact runtime loader prefix")
    return owner


def inputs_for(selected):
    result = {}
    for relative in selected["inputs"]:
        path = (GATEWAY / relative).resolve()
        if GATEWAY not in path.parents or not path.is_file():
            raise RuntimeError("Missing or nonlocal consumed input: " + relative)
        result[relative] = digest(path)
    for relative in selected["sources"]:
        result.update(local_inputs(GATEWAY / relative))
    for relative in ("CMakeLists.txt", "CMakePresets.json", "tools/build.py", "tools/build_support.py", "tools/build_profiles.py", "tools/compiler-limit.py",
                     "tools/cmake-sources.py", "tools/cmake_focus.py", "tools/ctest_support.py", "tools/ctest-network.py",
                     "tools/ctest-run.py", "tools/generate-tests.py"):
        result[relative] = digest(GATEWAY / relative)
    for path in (GATEWAY / "cmake").rglob("*.cmake"):
        result[path.relative_to(GATEWAY).as_posix()] = digest(path)
    return result


def configuration_for(owner_path):
    return {name: digest(owner_path.parent / name) for name in ("CMakeCache.txt", "compile_commands.json", "build.ninja")}


def scratch_parent(options):
    root = options.scratch_root.resolve()
    allowed = (Path("/tmp").resolve(), WORKSPACE / ".data")
    if not any(root == base or base in root.parents for base in allowed):
        raise RuntimeError("Test scratch root must stay under /tmp or this worktree's .data")
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    return root


def runtime_policy(options):
    root = scratch_parent(options)
    return {"timeout_s": options.timeout, "rss_limit_mib": options.rss_limit_mib,
            "scratch_root": str(root), "scratch_device": root.stat().st_dev}


def bind_phase_instructions(options, evidence, label):
    if not options.supervisor_log or options.instructions is not None:
        return options.instructions
    if os.environ.get("UOS_GATEWAY_INSTRUCTION_SNAPSHOT"):
        expected = os.environ.get("UOS_GATEWAY_NOTES_SHA256")
        if not expected:
            raise RuntimeError("Inherited instruction snapshot has no bound hash")
        instruction = supervisor(options.supervisor_log, expected)
    else:
        instruction = bind_instruction_snapshot(options.supervisor_log, evidence, label)
    options.instructions = instruction
    options.notes_hash = instruction["notes_sha256"]
    os.environ.update(UOS_GATEWAY_INSTRUCTION_SNAPSHOT=instruction["snapshot"],
                      UOS_GATEWAY_NOTES_SHA256=options.notes_hash, UOS_GATEWAY_BUILD_EVIDENCE=str(evidence),
                      UOS_GATEWAY_RUN_ID=str(label))
    return instruction


def command_for(binary, owner, scratch):
    info = scratch.stat()
    return ["/usr/bin/unshare", "--user", "--map-root-user", "--net", "--mount", "--pid", "--fork",
            "--mount-proc", "--propagation", "private", "--kill-child=SIGKILL", "--", "/bin/sh", "-eu", "-c",
            '/usr/bin/mount --bind "$1" /tmp; shift; /usr/bin/ip link set dev lo up; cd /tmp/w; exec "$@"',
            "--", str(scratch), "/usr/bin/python3", str(GATEWAY / "tools/ctest-network.py"),
            os.readlink("/proc/self/ns/net"), str(info.st_dev) + ":" + str(info.st_ino)] + owner["runtime_prefix"] + [str(binary)]


def test_environment(scratch, selected, sanitized):
    environment = {"PATH": "/usr/bin:/bin", "HOME": "/tmp/h", "TMPDIR": "/tmp/t",
                   "TZ": "UTC", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8"}
    for assignment in selected["environment"]:
        name, value = assignment.split("=", 1)
        if name in ("HOME", "TMPDIR", "CODEX_HOME", "SSH_AUTH_SOCK"):
            raise RuntimeError("Registry environment would escape fixture isolation: " + name)
        environment[name] = value.replace("${SCRATCH}", "/tmp")
    if sanitized:
        environment["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        environment["TSAN_OPTIONS"] = "halt_on_error=1"
    return environment


def run_test(selected, binary, owner_path, registry_path, evidence, options):
    evidence.mkdir(parents=True, exist_ok=False)
    instruction = bind_phase_instructions(options, evidence, selected["target"] + "-" + str(time.time_ns()))
    owner = profile_identity(owner_path)
    if binary.resolve().parent != owner_path.resolve().parent or binary.name != selected["target"]:
        raise RuntimeError("Test binary does not match its registered target and profile directory")
    admission("driver", options.supervisor_log, options.notes_hash)
    profile_hash = digest(owner_path)
    configuration = configuration_for(owner_path)
    record = {"schema": 1, "test": selected["test"], "target": selected["target"], "profile": owner,
              "profile_owner_sha256": profile_hash, "registry_sha256": digest(registry_path),
              "configuration_sha256": configuration,
              "registration": selected, "receipt": str(evidence / "terminal.json"),
              "runtime_policy": runtime_policy(options),
              "reviewed_instructions": instruction,
              "execution": "fresh", "started": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "cases_run": 0, "cases_passed": 0, "cases_failed": 0, "settled": True}
    if selected["skip"]:
        record.update(status="SKIP", reason=selected["skip"])
        atomic_json(evidence / "terminal.json", record)
        return record
    live = generator().resolve_test(selected["test"])
    if live != selected:
        raise RuntimeError("Links registration changed; reconfigure before running " + selected["test"])
    if not binary.is_file() or binary.resolve().parent != owner_path.resolve().parent:
        raise RuntimeError("Missing test binary or binary outside its owned profile: " + str(binary))
    accepted = supervisor(options.supervisor_log, options.notes_hash) if options.supervisor_log else None
    admitted = admission("driver", options.supervisor_log, accepted["notes_sha256"] if accepted else None)
    if admitted["mem_available_bytes"] < 2 * GIB:
        raise RuntimeError("Test runtime requires 2 GiB MemAvailable before its sampled 2 GiB RSS guard")
    scratch = Path(tempfile.mkdtemp(prefix="uos-test-", dir=scratch_parent(options)))
    for name in ("h", "t", "w"):
        (scratch / name).mkdir(mode=0o700)
    sanitized = bool(owner.get("sanitizer")) or owner["profile"].startswith(("asan-", "tsan-"))
    environment = test_environment(scratch, selected, sanitized)
    before_inputs = inputs_for(selected)
    before_binary = digest(binary)
    command = command_for(binary, owner, scratch)
    record.update(binary=str(binary), binary_sha256=before_binary, inputs=before_inputs,
                  command=command, environment_keys=sorted(environment), registry_environment=selected["environment"],
                  synthetic_state=str(scratch), supervisor=accepted)
    atomic_json(evidence / "inputs.json", record)
    try:
        with runtime_lock():
            process = execute(command, scratch / "w", environment, evidence, timeout=options.timeout,
                              rss_limit=options.rss_limit_mib * 1024 ** 2, address_limit=None if sanitized else GIB)
        text = (evidence / "run.log").read_text(errors="replace")
        matches = SUMMARY.findall(text)
        network = NETWORK.findall(text)
        record.update(process=process, settled=process["settled"],
                      binary_unchanged=binary.is_file() and digest(binary) == before_binary)
        if matches:
            record.update(zip(("cases_run", "cases_passed", "cases_failed"), map(int, matches[-1])))
        if network:
            record["network"] = json.loads(network[0])
        record["status"] = "PASS" if process["code"] == 0 and not process["failure"] and len(network) == 1 and matches and \
            record["cases_run"] > 0 and record["cases_failed"] == 0 and record["cases_run"] == record["cases_passed"] else "FAIL"
        after_inputs = inputs_for(selected)
        changed = sorted(path for path in before_inputs.keys() | after_inputs.keys() if before_inputs.get(path) != after_inputs.get(path))
        if changed or digest(owner_path) != profile_hash or not record["binary_unchanged"] or configuration_for(owner_path) != configuration:
            record.update(status="INPUT_CHANGED", changed_inputs=changed,
                          profile_unchanged=digest(owner_path) == profile_hash,
                          configuration_unchanged=configuration_for(owner_path) == configuration)
        if len(network) != 1:
            record["infrastructure_failure"] = "Network namespace control did not complete exactly once"
        if "ThreadSanitizer: unexpected memory mapping" in text or "FATAL: ThreadSanitizer:" in text:
            record["infrastructure_failure"] = "ThreadSanitizer runtime could not initialize; inspect run.log"
        record["log_sha256"] = digest(evidence / "run.log")
        record["finished"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        atomic_json(evidence / "terminal.json", record)
        print(json.dumps({"status": record["status"], "test": selected["test"], "profile": owner["profile"],
                          "cases_run": record["cases_run"], "cases_passed": record["cases_passed"],
                          "cases_failed": record["cases_failed"], "receipt": str(evidence / "terminal.json")}), flush=True)
        if record["status"] != "PASS":
            print("\n".join(text.splitlines()[-35:]), file=sys.stderr, flush=True)
        return record
    finally:
        if not options.keep_scratch:
            shutil.rmtree(scratch)


def suite(options, registry):
    owner_path = options.profile_owner.resolve()
    owner = profile_identity(owner_path)
    build_directory = owner_path.parent
    selected = [item for item in registry["tests"] if not options.target or item["target"] in options.target]
    if options.target and len(selected) != len(set(options.target)):
        raise RuntimeError("Suite selection contains an unknown test target")
    if options.incremental_scope and (not options.target or len(selected) >= len(registry["tests"])):
        raise RuntimeError("Incremental scope requires a strict explicit subset; a full suite must hold the release lock")
    evidence = options.evidence or WORKSPACE / ".data/cpp-gateway-evidence" / ("suite-" + owner["profile"] + "-" + str(time.time_ns()))
    evidence.mkdir(parents=True, exist_ok=False)
    instruction = bind_phase_instructions(options, evidence, "suite-" + str(time.time_ns()))
    record = {"schema": 1, "profile": owner, "selected": len(selected), "registry_sha256": digest(options.registry),
              "selected_targets": [item["target"] for item in selected], "execution": "fresh", "records": [],
              "scope": "focused incremental subset" if options.incremental_scope else "full-release lock",
              "supervisor_notes_sha256": options.notes_hash, "started": time.time(), "settled": False}
    record["reviewed_instructions"] = instruction
    atomic_json(evidence / "suite.json", record)
    previous = {}
    if options.reuse_suite:
        prior = json.loads(options.reuse_suite.read_text())
        previous = {item["target"]: item for item in prior["records"] if item["status"] == "PASS"}
    scope = contextlib.nullcontext() if options.incremental_scope else lock(options.release_lock, "gateway-tests-" + owner["profile"])
    with scope:
        for number, item in enumerate(selected, 1):
            admission("driver", options.supervisor_log, options.notes_hash)
            print(json.dumps({"suite": owner["profile"], "index": number, "selected": len(selected), "test": item["test"]}), flush=True)
            prior = previous.get(item["target"])
            if prior and prior.get("settled") and prior.get("binary_unchanged") and prior.get("registration") == item and \
                    prior.get("profile_owner_sha256") == digest(owner_path) and prior.get("inputs") == inputs_for(item) and \
                    prior.get("configuration_sha256") == configuration_for(owner_path) and \
                    prior.get("runtime_policy") == runtime_policy(options):
                original_log = Path(prior["process"]["log"])
                if original_log.is_file() and digest(original_log) == prior.get("log_sha256"):
                    result = dict(prior)
                    result.update(execution="reused", reused_from=str(options.reuse_suite),
                                  reuse_verified_at=datetime.datetime.now(datetime.timezone.utc).isoformat())
                    record["records"].append(result)
                    atomic_json(evidence / "suite.json", record)
                    print(json.dumps({"status": "PASS", "test": item["test"], "execution": "reused",
                                      "original_receipt": result["receipt"]}), flush=True)
                    continue
            target_evidence = evidence / item["target"]
            binary = build_directory / item["target"]
            build_capture = None
            if not options.no_build and not item["skip"]:
                prefix = ["native"] if owner["profile"] != "sdk-release" else [owner["sdk"]["path"]]
                command = ["/usr/bin/python3", str(GATEWAY / "tools/build.py")] + prefix + [owner["profile"], "build", "--incremental",
                           "--target", item["target"], "--jobs", "2"]
                if owner.get("lane"):
                    command += ["--lane", owner["lane"]]
                if options.supervisor_log:
                    command += ["--supervisor-log", str(options.supervisor_log)]
                target_evidence.mkdir(parents=True)
                environment = {"PATH": "/usr/bin:/bin", "HOME": str(evidence), "TMPDIR": str(evidence),
                               "TZ": "UTC", "LANG": "C.UTF-8", "PYTHONDONTWRITEBYTECODE": "1"}
                if instruction:
                    environment.update(UOS_GATEWAY_INSTRUCTION_SNAPSHOT=instruction["snapshot"],
                                       UOS_GATEWAY_NOTES_SHA256=instruction["notes_sha256"],
                                       UOS_GATEWAY_BUILD_EVIDENCE=str(target_evidence),
                                       UOS_GATEWAY_RUN_ID=item["target"] + "-" + str(time.time_ns()))
                with (target_evidence / "build.log").open("w") as output:
                    code, receipt = measured(command, GATEWAY, environment, target_evidence, "build-test", 7200, output=output)
                build_capture = {"command": command, "code": code, "receipt": str(receipt),
                                 "log": str(target_evidence / "build.log"), "log_sha256": digest(target_evidence / "build.log")}
                if code:
                    result = {"test": item["test"], "target": item["target"], "status": "BUILD_FAIL", "code": code,
                              "build_capture": build_capture, "cases_run": 0, "cases_passed": 0, "cases_failed": 0, "settled": True}
                    atomic_json(target_evidence / "terminal.json", result)
                    record["records"].append(result)
                    atomic_json(evidence / "suite.json", record)
                    print(json.dumps({"status": "BUILD_FAIL", "test": item["test"], "receipt": str(target_evidence / "terminal.json")}), flush=True)
                    print("\n".join((target_evidence / "build.log").read_text(errors="replace").splitlines()[-35:]), file=sys.stderr, flush=True)
                    continue
                run_evidence = target_evidence / "runtime"
            else:
                run_evidence = target_evidence
            result = run_test(item, binary, owner_path, options.registry, run_evidence, options)
            if build_capture:
                result["build_capture"] = build_capture
                atomic_json(run_evidence / "terminal.json", result)
            if options.prune and result["status"] == "PASS" and result["settled"] and result["binary_unchanged"]:
                if digest(binary) != result["binary_sha256"]:
                    raise RuntimeError("Binary changed between execution and pruning")
                result["pruned_bytes"] = binary.stat().st_size
                binary.unlink()
                result["binary_pruned_after_hash"] = True
                atomic_json(run_evidence / "terminal.json", result)
            record["records"].append(result)
            atomic_json(evidence / "suite.json", record)
        # Keep the release lock through reconciliation, retaining original runtime receipts.
        current_profile = digest(owner_path)
        current_configuration = configuration_for(owner_path)
        by_target = {item["target"]: item for item in selected}
        for result in record["records"]:
            if result["status"] != "PASS":
                continue
            current_inputs = inputs_for(by_target[result["target"]])
            changed = sorted(path for path in result["inputs"].keys() | current_inputs.keys()
                             if result["inputs"].get(path) != current_inputs.get(path))
            if changed or result["profile_owner_sha256"] != current_profile or result["configuration_sha256"] != current_configuration:
                result.update(status="STALE_AT_SUITE_END", original_runtime_status="PASS", changed_inputs_at_end=changed)
        scope_drift = []
        if len(selected) == len(registry["tests"]):
            live_targets = {item["target"] for item in generator().inventory()["tests"]}
            scope_drift = sorted(live_targets ^ {item["target"] for item in selected})
        record["scope_drift"] = scope_drift
        record["reconciled_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    counts = collections.Counter(item["status"] for item in record["records"])
    record.update(status_counts=dict(counts), executed=sum(item["status"] not in ("SKIP", "BUILD_FAIL") and item.get("execution") != "reused" for item in record["records"]),
                  fresh_executed=sum(item["status"] not in ("SKIP", "BUILD_FAIL") and item.get("execution") != "reused" for item in record["records"]),
                  reused=sum(item.get("execution") == "reused" for item in record["records"]),
                  cases_run=sum(item["cases_run"] for item in record["records"]), cases_passed=sum(item["cases_passed"] for item in record["records"]),
                  fresh_cases_run=sum(item["cases_run"] for item in record["records"] if item.get("execution") != "reused"),
                  cases_failed=sum(item["cases_failed"] for item in record["records"]), duration_s=round(time.time() - record["started"], 3), settled=True)
    record["accepted_cases_passed"] = sum(item["cases_passed"] for item in record["records"] if item["status"] == "PASS")
    atomic_json(evidence / "suite.json", record)
    print(json.dumps({key: value for key, value in record.items() if key != "records"}), flush=True)
    return 0 if counts.get("PASS", 0) == len(selected) and not scope_drift else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("run", "suite"))
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--profile-owner", type=Path, required=True)
    parser.add_argument("--target", action="append", default=[])
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--supervisor-log", type=Path,
                        default=Path(os.environ["UOS_GATEWAY_RESOURCE_LOG"]) if os.environ.get("UOS_GATEWAY_RESOURCE_LOG") else None)
    parser.add_argument("--release-lock", type=Path, default=WORKSPACE / ".data/cpp-gateway-release.lock")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--rss-limit-mib", type=int, default=2048)
    parser.add_argument("--scratch-root", type=Path, default=Path("/tmp"), help="Physical fixture backing; /tmp retains the original Linux tmpfs semantics")
    parser.add_argument("--prune", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--incremental-scope", action="store_true", help="Explicit focused subset only; retain per-profile/global compiler locks without claiming full-release lock")
    parser.add_argument("--reuse-suite", type=Path, help="Reuse only prior passing receipts with identical consumed inputs/profile and a verified original log")
    parser.add_argument("--keep-scratch", action="store_true")
    options = parser.parse_args()
    if options.timeout <= 0 or options.rss_limit_mib <= 0:
        raise RuntimeError("Runtime and RSS bounds must be positive")
    options.notes_hash = None
    options.instructions = None
    registry = load_registry(options.registry)
    if options.mode == "suite":
        return suite(options, registry)
    if len(options.target) != 1 or not options.binary:
        raise RuntimeError("A CTest run requires exactly one target and binary")
    selected = next((item for item in registry["tests"] if item["target"] == options.target[0]), None)
    if selected is None:
        raise RuntimeError("Unknown registered target")
    evidence = options.evidence or WORKSPACE / ".data/cpp-gateway-evidence" / ("ctest-" + options.target[0] + "-" + str(time.time_ns()))
    result = run_test(selected, options.binary.resolve(), options.profile_owner.resolve(), options.registry, evidence, options)
    return 0 if result["status"] == "PASS" else 77 if result["status"] == "SKIP" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except InterruptedError as error:
        print("ctest-run: " + str(error), file=sys.stderr)
        raise SystemExit(130)
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print("ctest-run: " + str(error), file=sys.stderr)
        raise SystemExit(3)
