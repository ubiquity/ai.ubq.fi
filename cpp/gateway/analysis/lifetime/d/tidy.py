#!/usr/bin/env python3
"""Run the five lifetime checks on area D with staged native headers."""

import argparse
import collections
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time

from audit import GATEWAY, LEDGER

CHECKS = (
    "cppcoreguidelines-avoid-capturing-lambda-coroutines",
    "cppcoreguidelines-avoid-reference-coroutine-parameters",
    "misc-coroutine-hostile-raii",
    "bugprone-dangling-handle",
    "bugprone-use-after-move",
)
DIAGNOSTIC = re.compile(r"^(.*?):(\d+):(\d+): (?:warning|error): (.*?) \[([^]]+)\]$", re.M)


def run_unit(source, settings, directory, tool, seconds):
    logfile = directory / (source + ".log")
    logfile.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(dir=directory, prefix="unit-") as scratch:
        work = Path(scratch)
        for name in ("home", "temp"):
            (work / name).mkdir()
        env = {"PATH": "/usr/local/bin:/usr/bin:/bin", "LANG": "C.UTF-8", "HOME": str(work / "home"), "TMPDIR": str(work / "temp")}
        status = "ok"
        with logfile.open("w") as out:
            try:
                completed = subprocess.run([tool, str(GATEWAY / source), *settings], env=env, cwd=work, stderr=subprocess.STDOUT, stdout=out, timeout=seconds)
                if completed.returncode:
                    status = f"exit {completed.returncode}"
            except subprocess.TimeoutExpired:
                status = "timeout"
    diagnostics = []
    for filename, line, column, message, check in DIAGNOSTIC.findall(logfile.read_text()):
        file = Path(filename)
        if GATEWAY in file.parents:
            diagnostics.append((file.relative_to(GATEWAY).as_posix(), int(line), int(column), check, message))
    result = {"source": source, "status": status, "seconds": round(time.monotonic() - started, 3), "diagnostics": diagnostics}
    logfile.with_suffix(".json").write_text(json.dumps(result) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boost", type=Path, required=True)
    parser.add_argument("--icu", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--tool", default="/usr/bin/clang-tidy-21")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--source", action="append")
    args = parser.parse_args()
    output = args.out.resolve()
    if GATEWAY.parents[1] == output or GATEWAY.parents[1] in output.parents:
        parser.error("use an output path outside the checkout")
    output.mkdir(parents=True, exist_ok=False)
    ledger = json.loads(LEDGER.read_text())
    source = args.source or [path for path in ledger["files"] if path.endswith(".cpp")]
    extra = ["-isystem", str(args.boost.resolve())]
    if args.icu:
        extra += ["-isystem", str(args.icu.resolve())]
    settings = [
        f"--config-file={GATEWAY / '.clang-tidy'}", "--checks=-*," + ",".join(CHECKS), "--quiet",
        "--", "-std=c++20", "-DUOS_GATEWAY_NATIVE_LIBRARIES=1", "-DBOOST_JSON_NO_LIB", "-DBOOST_CONTAINER_NO_LIB",
        "-DBOOST_SYSTEM_NO_LIB", "-pthread", "-I" + str(GATEWAY / "include"), "-I" + str(GATEWAY), *extra,
    ]
    (output / "command.json").write_text(json.dumps({"tool": args.tool, "settings": settings, "jobs": args.jobs, "timeout": args.timeout, "source_commit": ledger["source_commit"]}, indent=2) + "\n")
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        results = list(executor.map(lambda path: run_unit(path, settings, output, args.tool, args.timeout), source))
    diagnostics = sorted({tuple(row) for result in results for row in result["diagnostics"]})
    summary = {
        "wall_seconds": round(time.monotonic() - started, 3),
        "statuses": collections.Counter(result["status"] for result in results),
        "diagnostic_counts": collections.Counter(row[3] for row in diagnostics),
        "unique_diagnostics": diagnostics,
        "translation_units": [{"source": result["source"], "status": result["status"], "seconds": result["seconds"]} for result in results],
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{len(results)} translation units in {summary['wall_seconds']} s, {dict(summary['statuses'])}")
    print(f"{len(diagnostics)} unique diagnostics, {dict(summary['diagnostic_counts'])}")
    return int(any(result["status"] != "ok" for result in results))


if __name__ == "__main__":
    raise SystemExit(main())
