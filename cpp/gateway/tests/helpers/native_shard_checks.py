#!/usr/bin/env python3
"""Disposable Linux runner for the authority/kernel/paid-fallback sanitizer shard.

Uses exact Links closures, shared per-source objects, credential-free environments
and private loopback-only network namespaces. Requires passwordless sudo/unshare.
All generated artifacts and logs stay outside the checkout.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


GATEWAY = Path(__file__).resolve().parents[2]
REPOSITORY = GATEWAY.parents[1]
PREFIXES = ("native_", "authority_", "kernel_", "paid_", "api_", "quota_", "worker_")
BASE_FLAGS = [
    "-std=c++20", "-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror",
    "-DBOOST_JSON_NO_LIB", "-DBOOST_CONTAINER_NO_LIB", "-DBOOST_SYSTEM_NO_LIB",
    "-DUOS_GATEWAY_NATIVE_LIBRARIES", "-pthread", "-O1", "-g",
]
SAN_FLAGS = [
    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=undefined",
]


def clean_env():
    # Never inherit a credential, including credentials with unfamiliar names.
    return {"PATH": "/usr/local/bin:/usr/bin:/bin", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8", "TZ": "UTC"}


def identity():
    revisions = subprocess.check_output(
        ["git", "-C", str(REPOSITORY), "rev-parse", "HEAD", "HEAD^{tree}"], env=clean_env(), text=True
    ).splitlines()
    dirty = subprocess.check_output(
        ["git", "-C", str(REPOSITORY), "status", "--porcelain"], env=clean_env(), text=True
    )
    return dict(head_commit=revisions[0], head_tree=revisions[1], worktree_clean=not dirty,
                commit=None if dirty else revisions[0], tree=None if dirty else revisions[1])


def manifest():
    # Import without leaving a __pycache__ in the read-only build-tools directory.
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("gateway_links", GATEWAY / "tools/links.py")
    links = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(links)
    registry = links.read_registry()
    closures = {}
    paths = {str(Path(__file__).relative_to(GATEWAY)), "tools/links.py", "tests/LINKS.txt"}
    for test in sorted((GATEWAY / "tests").glob("*_test.cpp")):
        if not test.name.startswith(PREFIXES):
            continue
        relative = str(test.relative_to(GATEWAY))
        resolved = links.resolve(relative, registry, check=True)
        if resolved.skip or resolved.env:
            raise SystemExit(f"Review unexpected Links skip/environment for {relative}")
        closures[relative] = resolved.sources
        paths.update(links.input_paths(relative, resolved.sources))
    hashes = {path: hashlib.sha256((GATEWAY / path).read_bytes()).hexdigest() for path in sorted(paths)}
    return closures, hashes


def quote(path):
    return '"' + str(path).replace("\\", "/").replace('"', '\\"') + '"'


def command(argv, log):
    print(shlex.join(map(str, argv)), flush=True)
    with log.open("a") as output:
        output.write("\n$ " + shlex.join(map(str, argv)) + "\n")
        output.flush()
        result = subprocess.run(argv, env=clean_env(), stdout=output, stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        print(log.read_text()[-6000:], flush=True)
    return result.returncode


def build(args, profile, closures, root):
    driver = root / "driver"
    driver.mkdir(parents=True, exist_ok=True)
    runtime = [str(path.relative_to(GATEWAY)) for path in sorted((GATEWAY / "src/rt").glob("*.cpp"))]
    sources = sorted(set(runtime).union(*(set(extra) for extra in closures.values())))
    units = {source: "unit_" + str(index) for index, source in enumerate(sources)}
    lines = [
        "cmake_minimum_required(VERSION 3.25)", "project(uos_native_shard LANGUAGES CXX)",
        "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)",
        "add_compile_options(" + " ".join(BASE_FLAGS + (SAN_FLAGS if profile == "asan" else [])) + ")",
        "include_directories(" + quote(GATEWAY / "include") + " " + quote(GATEWAY) + ")",
    ]
    if profile == "asan":
        lines.append("add_link_options(-fsanitize=address,undefined)")
    sqlite = "sqlite3"
    if args.sqlite_prefix:
        header = args.sqlite_prefix / "include/sqlite3.h"
        library = args.sqlite_prefix / "lib/libsqlite3.so"
        if not header.is_file() or not library.is_file():
            raise SystemExit("SQLite prefix must contain include/sqlite3.h and lib/libsqlite3.so")
        sqlite = quote(library)
        lines.append("include_directories(BEFORE " + quote(header.parent) + ")")
    for source, unit in units.items():
        lines.append(f"add_library({unit} OBJECT {quote(GATEWAY / source)})")
    targets = []
    for test, extra in closures.items():
        target = Path(test).stem
        objects = " ".join(f"$<TARGET_OBJECTS:{units[source]}>" for source in dict.fromkeys(runtime + extra))
        lines += [
            f"add_executable({target} {quote(GATEWAY / test)} {objects})",
            f"target_link_libraries({target} PRIVATE boost_json boost_container ssl crypto pthread dl {sqlite} z brotlidec zstd icuuc)",
        ]
        if not args.filter or args.filter in target:
            targets.append(target)
    if not targets:
        raise SystemExit("No matching tests")
    content = "\n".join(lines) + "\n"
    cmake = driver / "CMakeLists.txt"
    if not cmake.exists() or cmake.read_text() != content:
        cmake.write_text(content)
    (root / "closures.json").write_text(json.dumps(closures, indent=2) + "\n")
    code = command(["cmake", "-S", str(driver), "-B", str(root), "-G", "Ninja",
                    "-DCMAKE_CXX_COMPILER=" + args.compiler], root / "build.log")
    if not code:
        code = command(["cmake", "--build", str(root), "-j", str(args.jobs), "--target", *targets,
                        "--", "-k", "0"], root / "build.log")
    return code, targets


def run(args, profile, root, targets, run_root):
    rows = []
    logs = run_root / "logs"
    logs.mkdir()
    for target in targets:
        started = time.monotonic()
        with tempfile.TemporaryDirectory(prefix="au-", dir="/tmp") as directory:
            scratch = Path(directory)
            for name in ["home", "tmp", "work"]:
                (scratch / name).mkdir()
            env = clean_env() | {"HOME": str(scratch / "home"), "TMPDIR": str(scratch / "tmp")}
            if profile == "asan":
                env |= {"ASAN_OPTIONS": "detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1",
                        "UBSAN_OPTIONS": "print_stacktrace=1"}
                if args.symbolizer:
                    env["ASAN_SYMBOLIZER_PATH"] = str(args.symbolizer)
                if args.suppressions:
                    env["LSAN_OPTIONS"] = "suppressions=" + str(args.suppressions)
            # Create an actual loopback-only namespace, then drop root and all capabilities.
            # An unexpected external route is a failed preflight, never an unrestricted fallback.
            boundary = ('ip link set lo up || exit 97; '
                        'if ip route get 192.0.2.1 >/dev/null 2>&1; then exit 98; fi; '
                        'exec "$@"')
            argv = ["sudo", "-n", "unshare", "--net", "--", "/bin/sh", "-c", boundary, "cppgw-isolated",
                    "setpriv", f"--reuid={os.getuid()}", f"--regid={os.getgid()}", "--clear-groups",
                    "--bounding-set=-all", "--inh-caps=-all", "--ambient-caps=-all", "env", "-i",
                    *[key + "=" + value for key, value in env.items()], str(root / target)]
            if args.case:
                argv.append(args.case)
            log = logs / (target + ".log")
            with log.open("w") as output:
                output.write("$ " + shlex.join(argv) + "\n")
                output.flush()
                process = subprocess.Popen(argv, env=clean_env(), cwd=scratch / "work", stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    code = process.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    # sudo and the test share the owned process group; sudo may require root to kill.
                    subprocess.run(["sudo", "-n", "kill", "-KILL", "--", "-" + str(process.pid)],
                                   env=clean_env(), check=False, stdout=output, stderr=subprocess.STDOUT)
                    process.wait()
                    code = 124
            output_text = log.read_text(errors="replace")
            summaries = re.findall(r"(\d+) run, (\d+) passed, (\d+) failed", output_text)
            counts = list(map(int, summaries[-1])) if summaries else None
            diagnostics = bool(re.search(r"ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:", output_text))
            row = dict(test=target, exit=code, counts=counts, sanitizer_report=diagnostics,
                       seconds=round(time.monotonic() - started, 3), log=str(log))
            rows.append(row)
            print(json.dumps(row), flush=True)
            for result in [run_root / "results.json", root / "results.json"]:
                result.write_text(json.dumps(rows, indent=2) + "\n")
    return all(row["exit"] == 0 and row["counts"] and row["counts"][0] > 0
               and row["counts"][2] == 0 and not row["sanitizer_report"] for row in rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, default=Path("/tmp/cppgw-asan-authority-20261117"))
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--profile", choices=["plain", "asan", "both"], default="both")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--filter", default="")
    parser.add_argument("--case", default="")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--require-clean", action="store_true")
    parser.add_argument("--sqlite-prefix", type=Path)
    parser.add_argument("--suppressions", type=Path)
    parser.add_argument("--symbolizer", type=Path)
    args = parser.parse_args()
    args.build_root = args.build_root.resolve()
    if args.build_root == REPOSITORY or REPOSITORY in args.build_root.parents:
        parser.error("Build root must be outside the checkout")
    for name in ["sqlite_prefix", "suppressions", "symbolizer"]:
        if getattr(args, name):
            setattr(args, name, getattr(args, name).resolve())
    if not args.symbolizer:
        compiler = Path(shutil.which(args.compiler) or args.compiler).resolve()
        sibling = compiler.parent / "llvm-symbolizer"
        if sibling.is_file():
            args.symbolizer = sibling
    ok = True
    for profile in (["plain", "asan"] if args.profile == "both" else [args.profile]):
        closures, before = manifest()
        source_identity = identity()
        if args.require_clean and not source_identity["worktree_clean"]:
            parser.error("Acceptance requires a clean committed tree")
        root = args.build_root / profile
        run_root = root / "runs" / str(time.time_ns())
        run_root.mkdir(parents=True)
        provenance = source_identity | dict(profile=profile, inputs=before, timeout=args.timeout)
        receipt = run_root / "provenance.json"
        receipt.write_text(json.dumps(provenance, indent=2) + "\n")
        code, targets = build(args, profile, closures, root)
        provenance["build_exit"] = code
        passed = code == 0
        if passed and not args.build_only:
            passed = run(args, profile, root, targets, run_root)
        provenance["inputs_unchanged"] = before == manifest()[1]
        provenance["identity_unchanged"] = source_identity == identity()
        receipt.write_text(json.dumps(provenance, indent=2) + "\n")
        valid = provenance["inputs_unchanged"] and provenance["identity_unchanged"]
        ok = passed and valid and ok
        print(f"{profile}: tests {'PASS' if passed else 'FAIL'}, provenance {'VALID' if valid else 'STALE'}; receipt {receipt}", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
