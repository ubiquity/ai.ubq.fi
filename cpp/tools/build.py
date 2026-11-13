#!/usr/bin/python3
"""Offline, serial, measured invocation of the root-qualified SDK; never installs."""
import json
import os
import resource
from pathlib import Path
import shutil
import subprocess
import sys
import time

sdk = Path(sys.argv[1]).resolve()
preset = sys.argv[2]
phase = sys.argv[3]
if preset not in ("pi-release", "native-asan") or phase not in ("configure", "build", "test", "checkpoint", "test-checkpoint", "complete", "tidy"):
    raise SystemExit("usage: build.py SDK {pi-release|native-asan} {configure|build|test|checkpoint|tidy}")
workspace = Path(__file__).resolve().parents[2]
source = workspace / "cpp"
build = workspace / ".data/cpp-build" / preset
temporary = workspace / ".data/cpp-tmp" / preset
evidence = workspace / ".data/cpp-evidence"
for path in (temporary, evidence):
    path.mkdir(parents=True, exist_ok=True)
roots = [workspace / (".data/" + name) for name in ("cpp-build", "cpp-tmp", "cpp-stage", "cpp-debug", "cpp-evidence", "cpp-tool-view")]

def allocated(paths):
    seen = set()
    total = 0
    for root in paths:
        if not root.exists():
            continue
        for directory, _, files in os.walk(root):
            for path in [Path(directory)] + [Path(directory) / value for value in files]:
                st = path.lstat()
                key = (st.st_dev, st.st_ino)
                if key not in seen:
                    seen.add(key)
                    total += st.st_blocks * 512
    return total

sdk_bytes = int(subprocess.check_output(["du", "-sx", "--block-size=1", str(sdk)], text=True).split()[0])
baseline = allocated(roots)
free = shutil.disk_usage(workspace).free
if free - 256 * 1024 * 1024 < 2 * 1024 ** 3 or sdk_bytes + baseline + 256 * 1024 * 1024 > 2000000000:
    raise SystemExit("Insufficient bounded disk reserve for compiler phase")
env = {"PATH": "/usr/bin:/bin", "HOME": os.environ.get("HOME", "/home/codex"), "TMPDIR": str(temporary), "TZ": "UTC", "UOS_CPP_TOOLCHAIN_DIR": str(sdk)}
if preset == "native-asan":
    env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
loader = str(sdk / "root/lib/ld-linux-aarch64.so.1")
libraries = str(sdk / "root/lib/aarch64-linux-gnu") + ":" + str(sdk / "root/usr/lib/aarch64-linux-gnu")
view = workspace / ".data/cpp-tool-view"
(view / "bin").mkdir(parents=True, exist_ok=True)
(view / "share").mkdir(exist_ok=True)
resource_link = view / "share/cmake-3.25"
if not resource_link.exists():
    resource_link.symlink_to(sdk / "root/usr/share/cmake-3.25")
for tool in ("cmake", "ctest", "cpack"):
    wrapper = view / "bin" / tool
    wrapper.write_text('#!/bin/sh\nset -eu\nexec "' + loader + '" --library-path "' + libraries + '" --argv0 "$0" "' + str(sdk / "root/usr/bin" / tool) + '" "$@"\n')
    wrapper.chmod(0o755)
cmake = [str(view / "bin/cmake")]
commands = {
    "configure": cmake + ["--preset", preset, "-DUOS_BUN_EXECUTABLE=/home/codex/repos/ubiquity/ai.ubq.fi/.data/runtimes/bun/linux-arm64/1.4.0-086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9/bun", "-DUOS_RUNTIME_PREFIX=" + json.dumps([loader, "--library-path", libraries] if preset == "pi-release" else [])],
    "build": cmake + ["--build", "--preset", preset, "--", "-k0"],
    "test": [str(view / "bin/ctest"), "--preset", preset],
}
phases = ["configure", "build", "test"] if phase == "complete" else ["configure", "build"] if phase == "checkpoint" else ["configure", "test"] if phase == "test-checkpoint" else [phase]
receipts = []
for current in phases:
    if current == "tidy":
        units = sorted((source / "src").glob("*.cpp"))
        argv = [loader, "--library-path", libraries, str(sdk / "root/usr/bin/clang-tidy-14"), *map(str, units), "-p", str(build), "--checks=-*,clang-analyzer-core.*,clang-analyzer-cplusplus.*,bugprone-dangling-handle,bugprone-use-after-move", "--warnings-as-errors=*", "--extra-arg=--gcc-toolchain=" + str(sdk / "root/usr"), "--extra-arg=-resource-dir=" + str(sdk / "root/usr/lib/llvm-14/lib/clang/14.0.6")]
        argv = ["/usr/bin/python3", str(source / "tools/compiler-limit.py"), *argv]
    else:
        argv = commands[current]
    stamp = str(time.time_ns())
    log = evidence / (preset + "-" + current + "-" + stamp + ".log")
    timing = log.with_suffix(".time")
    start = time.monotonic()
    peak = baseline
    samples = []
    with log.open("w") as output:
        child = subprocess.Popen(argv, cwd=source, env=env, stdout=output, stderr=subprocess.STDOUT)
        while child.poll() is None:
            size = allocated(roots)
            peak = max(peak, size)
            available = shutil.disk_usage(workspace).free
            samples.append({"elapsed": round(time.monotonic() - start, 3), "candidate_bytes": size, "free": available})
            if size - baseline > 768 * 1024 * 1024 or sdk_bytes + size > 2000000000 or available < 2 * 1024 ** 3 or time.monotonic() - start > 300:
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                raise RuntimeError("Owned compiler phase crossed disk or time budget")
            time.sleep(1)
        code = child.wait()
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    timing.write_text(json.dumps({"measurement": "Python RUSAGE_CHILDREN after wait; cumulative maximum across this invocation", "max_rss_kib": usage.ru_maxrss, "user_s": usage.ru_utime, "system_s": usage.ru_stime}) + "\n")
    receipt = {"preset": preset, "phase": current, "command": argv, "code": code, "duration_s": round(time.monotonic() - start, 3), "baseline_candidate_bytes": baseline, "peak_candidate_bytes_lower_bound": peak, "sample_interval_s": 1, "sdk_bytes": sdk_bytes, "samples": samples, "log": str(log), "timing": str(timing)}
    log.with_suffix(".json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({key: receipt[key] for key in ("preset", "phase", "code", "duration_s", "peak_candidate_bytes_lower_bound", "log", "timing")}), flush=True)
    receipts.append(receipt)
    if code:
        print("".join(log.read_text().splitlines(keepends=True)[-35:]), file=sys.stderr)
        raise SystemExit(code)
