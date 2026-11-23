#!/usr/bin/env python3
"""Disposable Linux runner: all runtime objects, strict warnings, isolated tests.

Requires Clang, CMake, Ninja, Boost.JSON/Container, OpenSSL, SQLite, zlib, ICU,
Brotli, and Zstd development packages. All generated files stay outside the repo.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time


GATEWAY = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(GATEWAY / "tools"))
import vendor_sources
BASE_FLAGS = [
    "-std=c++20", "-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror",
    "-DBOOST_JSON_NO_LIB", "-DBOOST_CONTAINER_NO_LIB", "-DBOOST_SYSTEM_NO_LIB",
    "-DUOS_GATEWAY_NATIVE_LIBRARIES", "-pthread",
]
SAN_FLAGS = [
    "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    "-fno-sanitize-recover=undefined",
]


def clean_env():
    # An allowlist also removes credentials whose names we have never seen.
    return {"PATH": "/usr/local/bin:/usr/bin:/bin", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8"}


def quote(path):
    return '"' + str(path).replace("\\", "/").replace('"', '\\"') + '"'


def inputs():
    files = sorted(path for directory in ["src/rt", "include/uos/rt", "tests/rt"]
                   for path in (GATEWAY / directory).rglob("*") if path.is_file())
    files.extend([GATEWAY / "tools/vendor_sources.py", GATEWAY / vendor_sources.REGISTRY])
    files.extend(GATEWAY / relative for library in vendor_sources.selected() for relative in library.inputs)
    return {path.relative_to(GATEWAY).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}


def identity():
    revision = subprocess.check_output(["git", "-C", str(GATEWAY), "rev-parse", "HEAD", "HEAD^{tree}"],
                                       env=clean_env(), text=True).splitlines()
    status = subprocess.check_output(["git", "-C", str(GATEWAY), "status", "--porcelain"],
                                     env=clean_env(), text=True)
    return dict(head_commit=revision[0], head_tree=revision[1], worktree_clean=not status,
                commit=None if status else revision[0], tree=None if status else revision[1])


def command(argv, log, env):
    print(shlex.join(map(str, argv)), flush=True)
    with log.open("a") as output:
        output.write("\n$ " + shlex.join(map(str, argv)) + "\n")
        output.flush()
        result = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT, env=env, check=False)
    if result.returncode:
        print(log.read_text()[-16000:], flush=True)
        raise SystemExit(result.returncode)


def build(args, profile, tests):
    root = args.build_root / profile
    source = root / "driver"
    source.mkdir(parents=True, exist_ok=True)
    flags = BASE_FLAGS + (SAN_FLAGS if profile == "asan" else ["-O2"])
    runtime = sorted((GATEWAY / "src/rt").glob("*.cpp"))
    vendors = vendor_sources.required(tuple(vendor_sources.selected()), (path.relative_to(GATEWAY) for path in runtime))
    json_sources = vendor_sources.simdjson_consumers(vendors)
    lines = [
        "cmake_minimum_required(VERSION 3.25)",
        "project(uos_rt_checks LANGUAGES CXX)",
        "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)",
        "include_directories(" + quote(GATEWAY / "include") + " " + quote(GATEWAY) + ")",
        "add_library(runtime OBJECT",
        *[quote(path) for path in runtime],
        ")",
        "target_compile_options(runtime PRIVATE " + " ".join(flags) + ")",
    ]
    for library in vendors:
        members = " ".join(quote(GATEWAY / member) for member in library.sources)
        lines += [f"add_library({library.target} STATIC {members})",
                  f"target_compile_options({library.target} PRIVATE " + " ".join(vendor_sources.vendor_flags(flags)) + ")"]
    for source_path in sorted(json_sources):
        source_path = quote(GATEWAY / source_path)
        options = " ".join(map(quote, vendor_sources.simdjson_flags(GATEWAY)))
        lines.append(f"set_property(SOURCE {source_path} APPEND PROPERTY COMPILE_OPTIONS {options})")
    if profile == "asan":
        lines.append("add_link_options(-fsanitize=address,undefined)")
    sqlite_library = "sqlite3"
    if args.sqlite_prefix:
        header = args.sqlite_prefix / "include/sqlite3.h"
        library = args.sqlite_prefix / "lib/libsqlite3.so"
        if not header.is_file() or not library.is_file():
            raise SystemExit("SQLite prefix must contain include/sqlite3.h and lib/libsqlite3.so")
        sqlite_library = quote(library)
        lines.append("set_source_files_properties(" + quote(GATEWAY / "src/rt/sqlite.cpp")
                     + " PROPERTIES INCLUDE_DIRECTORIES " + quote(header.parent) + ")")
    for test in tests:
        # Verify the existing Links resolver: runtime tests need no module extras.
        extras = subprocess.check_output(
            ["python3", str(GATEWAY / "tools/links.py"), str(test)], env=clean_env(), text=True
        ).strip()
        if extras:
            raise SystemExit(f"Unexpected non-runtime Links closure for {test}: {extras}")
        lines += [
            f"add_executable({test.stem} {quote(test)} $<TARGET_OBJECTS:runtime>)",
            f"target_compile_options({test.stem} PRIVATE " + " ".join(flags) + ")",
            f"target_link_libraries({test.stem} PRIVATE " + " ".join(library.target for library in vendors) + " boost_json boost_container ssl crypto pthread dl {sqlite_library} z brotlidec zstd icui18n icuuc)",
        ]
    content = "\n".join(lines) + "\n"
    cmake_file = source / "CMakeLists.txt"
    if not cmake_file.exists() or cmake_file.read_text() != content:
        cmake_file.write_text(content)
    env = clean_env()
    command(["cmake", "-S", str(source), "-B", str(root), "-G", "Ninja",
             "-DCMAKE_CXX_COMPILER=" + args.compiler], root / "build.log", env)
    targets = [test.stem for test in tests if args.filter in test.stem]
    if not targets:
        raise SystemExit("No matching tests")
    command(["cmake", "--build", str(root), "-j", str(args.jobs), "--target", *targets], root / "build.log", env)
    return root, targets


def run(args, profile, root, targets, before, source_identity):
    rows = []
    run_root = root / "runs" / str(time.time_ns())
    logs = run_root / "logs"
    logs.mkdir(parents=True)
    provenance = source_identity | dict(profile=profile, inputs=before)
    (run_root / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    for target in targets:
        started = time.monotonic()
        scratch_root = args.fs_scratch_root if target == "fs_test" and args.fs_scratch_root else Path("/tmp")
        with tempfile.TemporaryDirectory(prefix="rt-", dir=scratch_root) as scratch:
            scratch = Path(scratch)
            for name in ["home", "tmp", "work"]:
                (scratch / name).mkdir()
            env = clean_env() | {"HOME": str(scratch / "home"), "TMPDIR": str(scratch / "tmp")}
            if profile == "asan":
                env |= {
                    "ASAN_OPTIONS": "detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1",
                    "UBSAN_OPTIONS": "print_stacktrace=1",
                }
                if args.suppressions:
                    env["LSAN_OPTIONS"] = "suppressions=" + str(args.suppressions.resolve())
            argv = [str(root / target)] + ([args.case] if args.case else [])
            log = logs / (target + ".log")
            with log.open("w") as output:
                process = subprocess.Popen(argv, cwd=scratch / "work", env=env, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    code = process.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                    code = 124
            text = log.read_text(errors="replace")
            summaries = re.findall(r"(\d+) run, (\d+) passed, (\d+) failed", text)
            counts = list(map(int, summaries[-1])) if summaries else None
            row = dict(test=target, exit=code, counts=counts, seconds=round(time.monotonic() - started, 3), log=str(log))
            rows.append(row)
            print(json.dumps(row), flush=True)
            result_text = json.dumps(rows, indent=2) + "\n"
            (run_root / "results.json").write_text(result_text)
            (root / "results.json").write_text(result_text)
    provenance["inputs_unchanged"] = before == inputs()
    provenance["identity_unchanged"] = source_identity == identity()
    (run_root / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    if not provenance["inputs_unchanged"]:
        print("FAIL: runtime inputs changed during build or execution", flush=True)
    if not provenance["identity_unchanged"]:
        print("FAIL: checkout identity changed during build or execution", flush=True)
    return provenance["inputs_unchanged"] and provenance["identity_unchanged"] and all(
        row["exit"] == 0 and row["counts"] and row["counts"][0] > 0 and row["counts"][2] == 0 for row in rows
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, default=Path("/tmp/cppgw-asan-rt-20261117"))
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--profile", choices=["asan", "plain", "both"], default="both")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--filter", default="", help="Test binary name substring")
    parser.add_argument("--case", default="", help="Harness case name substring")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--require-clean", action="store_true", help="Require a clean committed tree for acceptance receipts")
    parser.add_argument("--suppressions", type=Path, help="Explicit, documented LSan suppressions only")
    parser.add_argument("--sqlite-prefix", type=Path, help="Optional exact SQLite library and header prefix")
    parser.add_argument("--fs-scratch-root", type=Path, help="Filesystem for fs_test (e.g. tmpfs for its extreme timestamps)")
    args = parser.parse_args()
    args.build_root = args.build_root.resolve()
    if args.sqlite_prefix:
        args.sqlite_prefix = args.sqlite_prefix.resolve()
    if args.fs_scratch_root:
        args.fs_scratch_root = args.fs_scratch_root.resolve()
    if args.build_root == GATEWAY.parent.parent or GATEWAY.parent.parent in args.build_root.parents:
        parser.error("Build root must be outside the repository")
    if args.fs_scratch_root and (args.fs_scratch_root == GATEWAY.parent.parent or GATEWAY.parent.parent in args.fs_scratch_root.parents):
        parser.error("Filesystem scratch root must be outside the repository")
    tests = sorted((GATEWAY / "tests/rt").glob("*_test.cpp"))
    ok = True
    for profile in (["asan", "plain"] if args.profile == "both" else [args.profile]):
        before = inputs()
        source_identity = identity()
        if args.require_clean and not source_identity["worktree_clean"]:
            parser.error("Acceptance requires a clean committed tree")
        root, targets = build(args, profile, tests)
        if not args.build_only:
            ok = run(args, profile, root, targets, before, source_identity) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
