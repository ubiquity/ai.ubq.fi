#!/usr/bin/env python3
"""Disposable-runner Codex shard build and isolated plain/ASan/UBSan execution.

Generated CMake, objects, binaries, logs and receipts must stay outside the repo.
The existing Links resolver determines each binary's exact module object list.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import tempfile
import time


GATEWAY = Path(__file__).resolve().parents[2]
REPOSITORY = GATEWAY.parent.parent
BASE_FLAGS = [
    "-std=c++20", "-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror",
    "-DBOOST_JSON_NO_LIB", "-DBOOST_CONTAINER_NO_LIB", "-DBOOST_SYSTEM_NO_LIB",
    "-DUOS_GATEWAY_NATIVE_LIBRARIES", "-pthread", "-O1", "-g",
]
SAN_FLAGS = [
    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    "-fno-sanitize-recover=undefined",
]


def clean_env():
    # Never inherit a provider, gateway, credential, dotenv or loader variable.
    return {"PATH": "/usr/local/bin:/usr/bin:/bin", "LANG": "C.UTF-8",
            "LC_ALL": "C.UTF-8", "TZ": "UTC", "PYTHONDONTWRITEBYTECODE": "1"}


def quote(path):
    return '"' + str(path).replace("\\", "/").replace('"', '\\"') + '"'


def network_isolation():
    namespace_path = Path("/proc/self/ns/net")
    if not namespace_path.exists():
        raise SystemExit("Test execution requires a Linux network namespace")
    interfaces = sorted(name for _, name in socket.if_nameindex())
    if interfaces != ["lo"]:
        raise SystemExit("Test execution requires an isolated network namespace with only loopback; "
                         "use sudo -n unshare --net, enable lo, and drop to the runner UID/GID")
    return dict(network_namespace=os.readlink(namespace_path), network_interfaces=interfaces)


def identity():
    revision = subprocess.check_output(
        ["git", "-C", str(REPOSITORY), "rev-parse", "HEAD", "HEAD^{tree}"],
        env=clean_env(), text=True).splitlines()
    status = subprocess.check_output(
        ["git", "-C", str(REPOSITORY), "status", "--porcelain"],
        env=clean_env(), text=True)
    return dict(head_commit=revision[0], head_tree=revision[1], worktree_clean=not status,
                commit=None if status else revision[0], tree=None if status else revision[1])


def inputs(tests):
    files = set()
    for directory in ["src", "include", "tests/helpers"]:
        files.update(path for path in (GATEWAY / directory).rglob("*") if path.is_file())
    for test in tests:
        files.add(test)
        files.update(test.parent.glob(test.stem.removesuffix("_test") + "*.inc"))
    files.update([GATEWAY / "tools/links.py", GATEWAY / "tests/LINKS.txt"])
    # All vector inputs are fingerprinted, including differently named includes.
    files.update((GATEWAY / "tests").glob("*.inc"))
    return {str(path.relative_to(GATEWAY)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(files)}


def command(argv, log, env):
    with log.open("a") as output:
        output.write("\n$ " + shlex.join(map(str, argv)) + "\n")
        output.flush()
        result = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT, env=env, check=False)
    if result.returncode:
        print(log.read_text()[-14000:], flush=True)
        raise SystemExit(result.returncode)


def closures(tests):
    result = {}
    for test in tests:
        sources = subprocess.check_output(
            ["python3", str(GATEWAY / "tools/links.py"), str(test)],
            env=clean_env(), text=True).split()
        result[test.stem] = sources
    return result


def build(args, profile, tests, links):
    root = args.build_root / profile
    driver = root / "driver"
    driver.mkdir(parents=True, exist_ok=True)
    for name in ["home", "tmp"]:
        (root / name).mkdir(exist_ok=True)
    sources = sorted({source for values in links.values() for source in values})
    flags = BASE_FLAGS + (SAN_FLAGS if profile == "asan" else [])
    lines = [
        "cmake_minimum_required(VERSION 3.25)",
        "project(uos_codex_checks LANGUAGES CXX)",
        "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)",
        "set_property(GLOBAL PROPERTY JOB_POOLS link_pool=1)",
        "set(CMAKE_JOB_POOL_LINK link_pool)",
        "add_compile_options(" + " ".join(flags) + ")",
        "include_directories(" + quote(GATEWAY / "include") + " " + quote(GATEWAY) + ")",
        "add_library(runtime OBJECT",
        *[quote(path) for path in sorted((GATEWAY / "src/rt").glob("*.cpp"))],
        ")",
    ]
    if profile == "asan":
        lines.append("add_link_options(-fsanitize=address,undefined)")
    names = {source: "unit_" + hashlib.sha256(source.encode()).hexdigest()[:16] for source in sources}
    for source in sources:
        lines.append(f"add_library({names[source]} OBJECT {quote(GATEWAY / source)})")
    for test in tests:
        lines.append(f"add_executable({test.stem} {quote(test)} $<TARGET_OBJECTS:runtime>")
        lines.extend(f"$<TARGET_OBJECTS:{names[source]}>" for source in links[test.stem])
        lines += [")", f"target_link_libraries({test.stem} PRIVATE boost_json boost_container ssl crypto pthread dl sqlite3 z brotlidec zstd icuuc icui18n)"]
    content = "\n".join(lines) + "\n"
    cmake_file = driver / "CMakeLists.txt"
    if not cmake_file.exists() or cmake_file.read_text() != content:
        cmake_file.write_text(content)
    env = clean_env() | {"HOME": str(root / "home"), "TMPDIR": str(root / "tmp")}
    command(["cmake", "-S", str(driver), "-B", str(root), "-G", "Ninja",
             "-DCMAKE_CXX_COMPILER=" + args.compiler], root / "build.log", env)
    targets = [test.stem for test in tests if args.filter in test.stem]
    if not targets:
        raise SystemExit("No matching tests")
    print(f"{profile}: {len(targets)} targets, {len(sources)} module/helper sources, 44 runtime sources", flush=True)
    command(["cmake", "--build", str(root), "-j", str(args.jobs), "--target", *targets], root / "build.log", env)
    return root, targets


def run(args, profile, root, targets, tests, before, source_identity):
    rows = []
    run_root = root / "runs" / str(time.time_ns())
    logs = run_root / "logs"
    logs.mkdir(parents=True)
    symbolizer = subprocess.check_output(
        [args.compiler, "-print-prog-name=llvm-symbolizer"], env=clean_env(), text=True).strip()
    if profile == "asan" and not Path(symbolizer).is_file():
        raise SystemExit("Install the compiler's llvm-symbolizer before running sanitizers")
    provenance = source_identity | dict(profile=profile, inputs=before,
        compiler=subprocess.check_output([args.compiler, "--version"], env=clean_env(), text=True),
        flags=BASE_FLAGS + (SAN_FLAGS if profile == "asan" else []), timeout=args.timeout,
        symbolizer=symbolizer, **network_isolation())
    (run_root / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    for target in targets:
        started = time.monotonic()
        with tempfile.TemporaryDirectory(prefix="cx-", dir="/tmp") as scratch:
            scratch = Path(scratch)
            for name in ["h", "t", "w"]:
                (scratch / name).mkdir()
            env = clean_env() | {"HOME": str(scratch / "h"), "TMPDIR": str(scratch / "t")}
            if profile == "asan":
                env |= {"ASAN_OPTIONS": "detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1",
                        "UBSAN_OPTIONS": "print_stacktrace=1", "ASAN_SYMBOLIZER_PATH": symbolizer}
                if args.suppressions:
                    env["LSAN_OPTIONS"] = "suppressions=" + str(args.suppressions.resolve())
            argv = [str(root / target)] + ([args.case] if args.case else [])
            log = logs / (target + ".log")
            with log.open("w") as output:
                output.write("$ " + shlex.join(argv) + "\n")
                output.flush()
                process = subprocess.Popen(argv, cwd=scratch / "w", env=env, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    code = process.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                    code = 124
            output_text = log.read_text(errors="replace")
            summaries = re.findall(r"(\d+) run, (\d+) passed, (\d+) failed", output_text)
            counts = list(map(int, summaries[-1])) if summaries else None
            diagnostics = bool(re.search(r"ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:", output_text))
            row = dict(test=target, exit=code, counts=counts, sanitizer_diagnostic=diagnostics,
                       seconds=round(time.monotonic() - started, 3), log=str(log))
            rows.append(row)
            print(json.dumps(row), flush=True)
            result_text = json.dumps(rows, indent=2) + "\n"
            (run_root / "results.json").write_text(result_text)
            (root / "results.json").write_text(result_text)
    provenance["inputs_unchanged"] = before == inputs(tests)
    provenance["identity_unchanged"] = source_identity == identity()
    (run_root / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    ok = provenance["inputs_unchanged"] and provenance["identity_unchanged"] and all(
        row["exit"] == 0 and row["counts"] and row["counts"][0] > 0 and row["counts"][2] == 0
        and not row["sanitizer_diagnostic"] for row in rows)
    print(json.dumps(dict(profile=profile, binaries=len(rows), passed=sum(
        row["exit"] == 0 and bool(row["counts"]) and row["counts"][2] == 0
        and not row["sanitizer_diagnostic"] for row in rows),
        cases=[sum(row["counts"][index] for row in rows if row["counts"]) for index in range(3)],
        inputs_unchanged=provenance["inputs_unchanged"], identity_unchanged=provenance["identity_unchanged"],
        receipt=str(run_root), ok=ok)), flush=True)
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, default=Path("/tmp/cppgw-asan-codex-20261117"))
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--profile", choices=["asan", "plain", "both"], default="both")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--filter", default="", help="Test binary name substring")
    parser.add_argument("--case", default="", help="Harness case name substring")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build-only", action="store_true")
    mode.add_argument("--run-only", action="store_true")
    parser.add_argument("--require-clean", action="store_true")
    parser.add_argument("--suppressions", type=Path, help="Explicit documented model limits only")
    args = parser.parse_args()
    args.build_root = args.build_root.resolve()
    if args.build_root == REPOSITORY or REPOSITORY in args.build_root.parents:
        parser.error("Build root must be outside the repository")
    if not args.build_only:
        network_isolation()
    tests = sorted(path for path in (GATEWAY / "tests").glob("*_test.cpp")
                   if path.name.startswith(("codex_", "banked_reset", "cluster_")))
    links = closures(tests)
    ok = True
    for profile in (["plain", "asan"] if args.profile == "both" else [args.profile]):
        before = inputs(tests)
        source_identity = identity()
        if args.require_clean and not source_identity["worktree_clean"]:
            parser.error("Acceptance requires a clean committed tree")
        if args.run_only:
            root = args.build_root / profile
            targets = [test.stem for test in tests if args.filter in test.stem]
            if not targets:
                parser.error("No matching tests")
            # Ninja detects stale objects without compiling in a run-only invocation.
            probe = subprocess.run(["ninja", "-C", str(root), "-n", *targets],
                                   env=clean_env(), text=True, stdout=subprocess.PIPE, check=True)
            if "ninja: no work to do." not in probe.stdout:
                raise SystemExit("Stale build: run --build-only before --run-only\n" + probe.stdout[:4000])
        else:
            root, targets = build(args, profile, tests, links)
        if not args.build_only:
            ok = run(args, profile, root, targets, tests, before, source_identity) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
