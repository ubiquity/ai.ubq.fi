#!/usr/bin/python3
"""Build and run selected tests under ASan + UBSan + LSan on a disposable CI runner.

Reads test paths (relative to cpp/gateway) from a list file, resolves each test's
sources with tools/links.py, compiles every distinct source once with the runner's
Clang, links one binary per test and runs it with an empty environment, a fresh
HOME/TMPDIR and a time limit. Nothing is written inside the checkout.

Usage: ci_sanitize.py --list tests/ci-sanitize.txt --out DIR [--jobs N] [--timeout S]
Exit status: 0 all clean, 1 a test failed or a sanitizer reported, 3 build or usage error.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.dont_write_bytecode = True
import vendor_sources

GATEWAY = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c++20", "-pthread", "-O1", "-g", "-fno-omit-frame-pointer",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
         "-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror",
         "-DBOOST_JSON_NO_LIB", "-DBOOST_CONTAINER_NO_LIB", "-DBOOST_SYSTEM_NO_LIB",
         "-DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING", "-DUOS_GATEWAY_NATIVE_LIBRARIES=1",
         "-I" + str(GATEWAY / "include"), "-I" + str(GATEWAY)]
LIBRARY_FLAGS = []
LIBRARIES = ["boost_json", "boost_container", "ssl", "crypto", "z", "sqlite3", "brotlidec", "zstd",
             "icuuc", "icui18n", "dl", "pthread"]


def third_party():
    """Vendored libraries that first-party code includes: (include flags, sources)."""
    root = GATEWAY / "third_party"
    if not root.is_dir():
        return [], []
    used = subprocess.run(["grep", "-rhoE", r"#include [<\"][^>\"]+[>\"]", str(GATEWAY / "src"), str(GATEWAY / "include")],
                          capture_output=True, text=True).stdout
    flags, sources = [], []
    for library in sorted(path for path in root.iterdir() if path.is_dir()):
        headers = [path.name for path in library.rglob("*.h")] + [path.name for path in library.rglob("*.hpp")]
        if not any(name in used for name in headers):
            continue
        flags.append("-isystem" + str(library))
        sources += sorted(str(path.relative_to(GATEWAY)) for path in library.rglob("*")
                          if path.suffix in (".cpp", ".c", ".cc"))
    if flags:
        # First-party code also includes vendored headers by their tree path ("third_party/..."):
        # Clang then treats them as system headers too, so the port's -Werror set does not apply to them.
        flags.append("--system-header-prefix=third_party/")
    return flags, sources


def compile_one(compiler, c_compiler, source, objects):
    output = objects / (hashlib.sha256(source.encode()).hexdigest()[:16] + ".o")
    flags = FLAGS + LIBRARY_FLAGS
    if source.startswith("third_party/"):
        # Vendored code is not held to the port's warning set; C sources use the C driver.
        flags = [flag for flag in flags if not flag.startswith("-W")]
        if source.endswith(".c"):
            compiler = c_compiler
            flags = ["-std=c11", *vendor_sources.c_dialect_flags(source),
                     *[flag for flag in flags if not flag.startswith("-std=c++")]]
    done = subprocess.run([compiler, *flags, "-c", str(GATEWAY / source), "-o", str(output)],
                          capture_output=True, text=True)
    return source, output, done.returncode, done.stderr[-4000:]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--cc", default=os.environ.get("CC"), help="C compiler for vendor C sources")
    args = parser.parse_args()
    cxx = Path(args.compiler)
    c_name = cxx.name.replace("clang++", "clang").replace("g++", "gcc")
    c_compiler = args.cc or (str(cxx.with_name(c_name)) if c_name != cxx.name else "cc")
    tests = [line.strip() for line in args.list.read_text().splitlines()
             if line.strip() and not line.startswith("#")]
    out = args.out.resolve()
    objects, binaries, logs = out / "obj", out / "bin", out / "logs"
    for directory in (objects, binaries, logs):
        directory.mkdir(parents=True, exist_ok=True)
    runtime = sorted(str(path.relative_to(GATEWAY)) for path in (GATEWAY / "src/rt").glob("*.cpp"))
    library_flags, library_sources = third_party()
    LIBRARY_FLAGS.extend(library_flags)
    runtime += library_sources
    closures = {}
    for test in tests:
        listed = subprocess.run([sys.executable, str(GATEWAY / "tools/links.py"), "--with-test", test],
                                cwd=GATEWAY, capture_output=True, text=True)
        if listed.returncode:
            print("LINK LIST FAILED " + test + "\n" + listed.stderr[-2000:], file=sys.stderr)
            return 3
        # links.py lists the application sources; every runtime source links into every test.
        closures[test] = list(dict.fromkeys([source for source in listed.stdout.split() if source.endswith(".cpp")] + runtime))
    sources = sorted({source for closure in closures.values() for source in closure})
    print("tests: %d, distinct translation units: %d" % (len(tests), len(sources)), flush=True)
    built, failed = {}, []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for source, output, code, stderr in pool.map(lambda item: compile_one(args.compiler, c_compiler, item, objects), sources):
            if code:
                failed.append(source)
                print("COMPILE FAILED " + source + "\n" + stderr, file=sys.stderr, flush=True)
            else:
                built[source] = str(output)
    results = []
    for test in tests:
        name = Path(test).stem
        record = {"test": test, "status": "BUILD_FAIL", "seconds": 0, "cases": None, "sanitizer": None}
        results.append(record)
        if any(source in failed for source in closures[test]):
            continue
        binary = binaries / name
        linked = subprocess.run([args.compiler, *[built[source] for source in closures[test]], "-o", str(binary),
                                 "-pthread", "-fsanitize=address,undefined", *["-l" + library for library in LIBRARIES]],
                                capture_output=True, text=True)
        if linked.returncode:
            print("LINK FAILED " + test + "\n" + linked.stderr[-4000:], file=sys.stderr, flush=True)
            continue
        scratch = out / "scratch" / name
        for directory in ("home", "tmp", "cwd"):
            (scratch / directory).mkdir(parents=True, exist_ok=True)
        environment = {"PATH": "/usr/bin:/bin", "HOME": str(scratch / "home"), "TMPDIR": str(scratch / "tmp"),
                       "LANG": "C.UTF-8", "TZ": "UTC",
                       "ASAN_OPTIONS": "detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1",
                       "UBSAN_OPTIONS": "print_stacktrace=1"}
        started = time.monotonic()
        log = logs / (name + ".log")
        with log.open("wb") as stream:
            try:
                code = subprocess.run(["/usr/bin/stdbuf", "-oL", str(binary)], cwd=scratch / "cwd", env=environment,
                                      stdout=stream, stderr=subprocess.STDOUT, timeout=args.timeout).returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        text = log.read_text(errors="replace")
        report = next((marker for marker in ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:")
                       if marker in text), None)
        record.update(status="PASS" if code == 0 and report is None else "FAIL", exit=code,
                      seconds=round(time.monotonic() - started, 1), sanitizer=report,
                      cases=next((line for line in reversed(text.splitlines()) if "passed" in line), None))
        binary.unlink()
        print("%-5s %s exit=%s sanitizer=%s %s" % (record["status"], test, code, report, record["cases"]), flush=True)
        if record["status"] != "PASS":
            print(text[-3000:], flush=True)
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    bad = [record for record in results if record["status"] != "PASS"]
    print("clean: %d of %d" % (len(results) - len(bad), len(results)))
    if any(record["status"] == "BUILD_FAIL" for record in bad):
        return 3
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
