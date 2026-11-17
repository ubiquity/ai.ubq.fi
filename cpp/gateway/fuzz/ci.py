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


def compile_one(compiler, flags, source, objects):
    output = objects / (source.replace("/", "_") + ".o")
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
    args = parser.parse_args()
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
    sources = policy["common_sources"] + [target["harness"]]
    failed = []
    built = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for source, output, code, stderr in pool.map(
                lambda source: compile_one(args.compiler, flags, source, objects), sources):
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
    tail = (out / "campaign.log").read_text(errors="replace")[-3000:]
    summary = {"target": args.target, "seconds": args.seconds, "jobs": args.jobs, "exit": code,
               "corpus_count": len(list(corpus.iterdir())), "findings": findings}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    shutil.rmtree(objects)
    print(tail)
    print(json.dumps(summary, indent=2))
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
