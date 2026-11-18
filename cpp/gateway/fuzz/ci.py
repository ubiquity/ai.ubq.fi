#!/usr/bin/python3
"""Build and run one parser fuzz target on a disposable CI runner.

Unlike run.py (finite 60-second campaigns under the production host's
admission rules), this runner has no host-safety limits: it compiles in
parallel with the system Clang and runs libFuzzer in fork mode for the
requested time. Sources, libraries and sanitizer options come from
targets.json, so both runners build the same harnesses.

Usage: ci.py <target> --seconds N --out DIR [--jobs N]
Exit status: 0 completed without findings, 1 findings were written to DIR/artifacts,
3 build failure or incomplete campaign.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time

GATEWAY = Path(__file__).resolve().parents[1]
DEFINES = ["BOOST_JSON_NO_LIB", "BOOST_CONTAINER_NO_LIB", "BOOST_SYSTEM_NO_LIB",
           "BOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING", "UOS_GATEWAY_NATIVE_LIBRARIES=1"]
# Harnesses that include an application source need that source's own link list;
# it is taken from the unit test that already links the same implementation.
LINK_LIST_TEST = {"sse": "tests/responses_stream_test.cpp", "http_request": "tests/runtime_http_test.cpp",
                  "passkeys": "tests/passkeys_test.cpp"}
FORK_EXIT = re.compile(r"^INFO: exiting: (\d+) time: (\d+)s$", re.MULTILINE)
TIMEOUT_EXIT = 70


def extra_sources(name, target, common):
    if name not in LINK_LIST_TEST:
        return []
    listed = subprocess.run([sys.executable, str(GATEWAY / "tools/links.py"), LINK_LIST_TEST[name]],
                            cwd=GATEWAY, capture_output=True, text=True, check=True).stdout.split()
    skip = set(common) | {target.get("included_implementation")}
    return [source for source in listed if source.startswith("src/") and source not in skip]


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


def compile_one(compiler, c_compiler, flags, source, objects):
    output = objects / (source.replace("/", "_") + ".o")
    if source.endswith(".c"):
        compiler = c_compiler
        flags = ["-std=c11"] + [flag for flag in flags if not flag.startswith("-std=c++")]
    done = subprocess.run([compiler, *flags, "-c", str(GATEWAY / source), "-o", str(output)],
                          capture_output=True, text=True)
    return source, output, done.returncode, done.stderr[-6000:]


def classify_campaign(target, seconds, artifacts, log, code):
    inputs = {path for path in artifacts.iterdir() if path.is_file()}
    slow = {path for path in inputs if target == "regex" and path.name.startswith("timeout-")}
    fork_exit = FORK_EXIT.search(log)
    fork_exit_code = int(fork_exit[1]) if fork_exit else None
    fuzzed_seconds = int(fork_exit[2]) if fork_exit else 0
    expected_codes = (0, TIMEOUT_EXIT) if target == "regex" else (0,)

    # The ARM64 fork parent reports its own exit leak after its last child settles.
    # Require that specific diagnostic and an empty crash artifact.
    fork_leaks = set()
    if platform.machine() in {"aarch64", "arm64"} and fork_exit and code != fork_exit_code:
        after_exit = log[fork_exit.end():]
        if "ERROR: LeakSanitizer: detected memory leaks" in after_exit and "fuzzer::FuzzWithFork" in after_exit:
            fork_leaks = {path for path in inputs if path.name.startswith("crash-") and path.stat().st_size == 0}

    findings = sorted(path.name for path in inputs - slow - fork_leaks)
    completed = fuzzed_seconds >= seconds and fork_exit_code in expected_codes and (code == fork_exit_code or bool(fork_leaks))
    outcome = "findings" if findings else "passed" if completed else "incomplete"
    return {"outcome": outcome, "fuzzed_seconds": fuzzed_seconds, "findings": findings,
            "expected_slow_inputs": len(slow), "expected_fork_exit_leaks": len(fork_leaks)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target")
    parser.add_argument("--seconds", type=int, default=300)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--cc", default=os.environ.get("CC"), help="C compiler for vendor sources")
    args = parser.parse_args()
    cxx = Path(args.compiler)
    c_name = cxx.name.replace("clang++", "clang").replace("g++", "gcc")
    c_compiler = args.cc or (str(cxx.with_name(c_name)) if c_name != cxx.name else "cc")
    policy = json.loads((GATEWAY / "fuzz/targets.json").read_text())
    if args.target not in policy["targets"] or args.seconds < 1 or args.jobs < 1:
        parser.error("known target, positive duration and positive job count are required")
    target = policy["targets"][args.target]
    out = args.out.resolve()
    objects, corpus, artifacts = out / "obj", out / "corpus", out / "artifacts"
    for directory in (objects, corpus, artifacts):
        directory.mkdir(parents=True, exist_ok=True)
    flags = ["-std=c++20", "-pthread", "-I" + str(GATEWAY / "include"), "-I" + str(GATEWAY)]
    flags += ["-D" + define for define in DEFINES] + policy["compile_options"]
    sources = policy["common_sources"] + extra_sources(args.target, target, policy["common_sources"])
    # A target may list further sources itself (for example vendored third_party libraries).
    sources += [source for source in target.get("extra_sources", []) if source not in sources]
    library_flags, library_sources = third_party()
    flags += library_flags
    sources += [source for source in library_sources if source not in sources]
    sources.append(target["harness"])
    failed = []
    built = []
    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for source, output, code, stderr in pool.map(
                lambda source: compile_one(args.compiler, c_compiler, flags, source, objects), sources):
            if code:
                failed.append(source)
                print("COMPILE FAILED " + source + "\n" + stderr, file=sys.stderr)
            else:
                built.append(str(output))
    if failed:
        print("gateway fuzz ci: " + str(len(failed)) + " translation units failed", file=sys.stderr)
        return 3
    binary = out / ("uos_fuzz_" + args.target)
    # The host policy pins mold and its thread count; a CI runner uses its default linker.
    link = [option for option in policy["link_options"] if "mold" not in option and "--threads" not in option]
    linked = subprocess.run([args.compiler, *built, "-o", str(binary), "-pthread", *link,
                             *["-l" + library for library in policy["libraries"]]], capture_output=True, text=True)
    if linked.returncode:
        print("LINK FAILED\n" + linked.stderr[-12000:], file=sys.stderr)
        return 3
    build_seconds = round(time.monotonic() - started, 3)
    for seed in (GATEWAY / "fuzz/corpus" / args.target).iterdir():
        if seed.is_file():
            shutil.copyfile(seed, corpus / seed.name)
    runtime = policy["runtime"]
    environment = dict(os.environ,
                       ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:allocator_may_return_null=0",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    command = [str(binary), "-max_len=" + str(target["max_len"]), "-timeout=" + str(runtime["timeout_s"]),
               "-rss_limit_mb=" + str(runtime["rss_limit_mb"]), "-malloc_limit_mb=128",
               "-fork=" + str(args.jobs), "-ignore_crashes=0", "-ignore_ooms=0",
               "-timeout_exitcode=" + str(TIMEOUT_EXIT), "-ignore_timeouts=" + str(int(args.target == "regex")),
               "-max_total_time=" + str(args.seconds), "-print_final_stats=1",
               "-artifact_prefix=" + str(artifacts) + "/", str(corpus)]
    campaign_log = out / "campaign.log"
    started = time.monotonic()
    with campaign_log.open("wb") as log:
        code = subprocess.run(command, cwd=out, env=environment, stdout=log, stderr=subprocess.STDOUT).returncode
    campaign_seconds = round(time.monotonic() - started, 3)
    log_text = campaign_log.read_text(errors="replace")
    result = classify_campaign(args.target, args.seconds, artifacts, log_text, code)
    summary = {"target": args.target, "seconds": args.seconds, "jobs": args.jobs, "exit": code,
               "build_seconds": build_seconds, "campaign_seconds": campaign_seconds,
               "corpus_count": len(list(corpus.iterdir())), **result}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    shutil.rmtree(objects)
    print(log_text[-3000:])
    print(json.dumps(summary, indent=2))
    return {"passed": 0, "findings": 1, "incomplete": 3}[result["outcome"]]


if __name__ == "__main__":
    raise SystemExit(main())
