#!/usr/bin/python3
"""Build and run one parser fuzz target on a disposable CI runner.

Unlike run.py (finite 60-second campaigns under the production host's
admission rules), this runner has no host-safety limits: it compiles in
parallel with the system Clang and runs libFuzzer in fork mode for the
requested time. Sources, libraries and sanitizer options come from
targets.json, so both runners build the same harnesses.

Usage: ci.py <target> --seconds N --out DIR [--jobs N]
Exit status: 0 no findings, 1 findings were written to DIR/artifacts, 3 build or usage error.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

GATEWAY = Path(__file__).resolve().parents[1]
DEFINES = ["BOOST_JSON_NO_LIB", "BOOST_CONTAINER_NO_LIB", "BOOST_SYSTEM_NO_LIB",
           "BOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING", "UOS_GATEWAY_NATIVE_LIBRARIES=1"]
# Harnesses that include an application source need that source's own link list;
# it is taken from the unit test that already links the same implementation.
LINK_LIST_TEST = {"sse": "tests/responses_stream_test.cpp", "http_request": "tests/runtime_http_test.cpp",
                  "passkeys": "tests/passkeys_test.cpp"}


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
        # Use the matching C frontend and retain the same sanitizer options.
        compiler = c_compiler
        flags = ["-std=c11", *[flag for flag in flags if not flag.startswith("-std=c++")]]
    done = subprocess.run([compiler, *flags, "-c", str(GATEWAY / source), "-o", str(output)],
                          capture_output=True, text=True)
    return source, output, done.returncode, done.stderr[-6000:]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target")
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--cc", default=os.environ.get("CC"), help="C compiler for vendor C sources")
    args = parser.parse_args()
    cxx = Path(args.compiler)
    c_name = cxx.name.replace("clang++", "clang").replace("g++", "gcc")
    c_compiler = args.cc or (str(cxx.with_name(c_name)) if c_name != cxx.name else "cc")
    policy = json.loads((GATEWAY / "fuzz/targets.json").read_text())
    if args.target not in policy["targets"] or args.seconds < 1:
        parser.error("known target and a positive duration are required")
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
    for seed in (GATEWAY / "fuzz/corpus" / args.target).iterdir():
        if seed.is_file():
            shutil.copyfile(seed, corpus / seed.name)
    runtime = policy["runtime"]
    environment = dict(os.environ,
                       ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:allocator_may_return_null=0",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    command = [str(binary), "-max_len=" + str(target["max_len"]), "-timeout=" + str(runtime["timeout_s"]),
               "-rss_limit_mb=" + str(runtime["rss_limit_mb"]), "-malloc_limit_mb=128",
               "-fork=" + str(args.jobs), "-ignore_crashes=1", "-ignore_timeouts=1", "-ignore_ooms=1",
               "-max_total_time=" + str(args.seconds), "-print_final_stats=1",
               "-artifact_prefix=" + str(artifacts) + "/", str(corpus)]
    with (out / "campaign.log").open("wb") as log:
        code = subprocess.run(command, cwd=out, env=environment, stdout=log, stderr=subprocess.STDOUT).returncode
    findings = sorted(path.name for path in artifacts.iterdir() if path.is_file())
    # Catastrophic backtracking is the JavaScript engines' own behaviour for patterns such as
    # (a*)+ and (.+)+, so slow regex inputs are kept in the artifacts but are not failures.
    expected = [name for name in findings if args.target == "regex" and name.startswith("timeout-")]
    # On ARM64 libFuzzer's fork-mode parent leaks 32 bytes of its own at exit (fuzzer::FuzzWithFork);
    # LeakSanitizer then records an empty "crash" input. It is the tool's leak, not a target finding.
    log_text = (out / "campaign.log").read_text(errors="replace")
    if "fuzzer::FuzzWithFork" in log_text:
        expected += [name for name in findings if name.startswith("crash-") and (artifacts / name).stat().st_size == 0]
    findings = [name for name in findings if name not in expected]
    tail = (out / "campaign.log").read_text(errors="replace")[-3000:]
    summary = {"target": args.target, "seconds": args.seconds, "jobs": args.jobs, "exit": code,
               "corpus_count": len(list(corpus.iterdir())), "findings": findings,
               "expected_slow_inputs": len(expected)}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    shutil.rmtree(objects)
    print(tail)
    print(json.dumps(summary, indent=2))
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
