#!/bin/sh
# Python 3 + PyYAML, matching Clang/clang-tidy and the documented native headers.
# No host presets, dotenv, installs, application execution or in-checkout artifacts.
set -eu
exec python3 -B - "$0" "$@" <<'PYTHON'
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

try:
    import yaml
except ImportError:
    sys.exit("Install PyYAML (e.g. python3-yaml) before running tidy.sh")

GATEWAY = Path(sys.argv[1]).resolve().parents[1]
REPO = GATEWAY.parents[1]
CONFIG = GATEWAY / ".clang-tidy"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def inputs():
    return {str(p.relative_to(GATEWAY)): digest(p)
            for p in sorted(GATEWAY.rglob("*"))
            if p.is_file() and p.suffix in {".cpp", ".hpp", ".h", ".inc"}}


def executable(name):
    path = shutil.which(name)
    if not path:
        raise RuntimeError("Executable not found: " + name)
    return str(Path(path).absolute())


def main():
    parser = argparse.ArgumentParser(description="Complete, isolated gateway clang-tidy audit; no source fixes.")
    parser.add_argument("--compiler", default="clang++", help="Clang C++ driver; e.g. clang++-24")
    parser.add_argument("--clang-tidy", help="Defaults to clang-tidy with the compiler's version suffix")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--build-dir", type=Path, help="External evidence directory; default: a fresh /tmp directory")
    parser.add_argument("--baseline", type=Path, help="Optional JSON destination; default: BUILD_DIR/tidy-baseline.json")
    parser.add_argument("--config", type=Path, help="Explicit configuration; default: gateway .clang-tidy")
    parser.add_argument("--audit", action="store_true", help="Replay the broad configuration recorded in analysis/tidy-baseline.json")
    parser.add_argument("--timeout", type=int, default=600, help="Per-TU process-group timeout in seconds")
    parser.add_argument("--source", action="append", help="Gateway-relative pilot TU; repeatable; full database is still generated")
    parser.add_argument("--checks", help="Explicit discovery override; retained verbatim in the receipt")
    parser.add_argument("--resume", action="store_true", help="Reuse successful receipts only when all input/tool/config identities match")
    parser.add_argument("--checkpoint-every", type=int, default=100, help="Atomically publish partial baseline and diagnostics every N units")
    parser.add_argument("--fail-on-findings", action="store_true")
    args = parser.parse_args(sys.argv[2:])
    if args.jobs < 1 or args.timeout < 1 or args.checkpoint_every < 1:
        parser.error("jobs, timeout and checkpoint-every must be positive")
    if args.audit and args.config:
        parser.error("select --audit or --config, not both")
    root = args.build_dir.resolve() if args.build_dir else Path(tempfile.mkdtemp(prefix="cppgw-tidy-"))
    if root == REPO or REPO in root.parents:
        parser.error("build-dir must be outside the checkout")
    root.mkdir(parents=True, exist_ok=True)
    if (root / "manifest.json").exists() and not args.resume:
        parser.error("existing evidence directory: select a fresh build-dir or use --resume")
    config = args.config.resolve() if args.config else CONFIG
    if args.audit:
        recorded = json.loads((GATEWAY / "analysis/tidy-baseline.json").read_text())
        config_text = recorded["configuration_yaml"]
        if hashlib.sha256(config_text.encode()).hexdigest() != recorded["identity"]["config_sha256"]:
            raise RuntimeError("Recorded discovery configuration hash does not match")
        config = root / "audit.clang-tidy"
        config.write_text(config_text)
    config_text = config.read_text()
    compiler = executable(args.compiler)
    suffix = re.search(r"(-\d+)$", compiler)
    tidy = executable(args.clang_tidy or ("clang-tidy" + (suffix.group(1) if suffix else "")))
    # An environment allowlist removes unknown provider/credential variables too.
    env = {"PATH": str(Path(compiler).parent) + ":/usr/bin:/bin", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8", "TZ": "UTC"}
    with tempfile.TemporaryDirectory(prefix="metadata-", dir=root) as scratch:
        for name in ("home", "tmp"):
            Path(scratch, name).mkdir()
        metadata_env = env | {"HOME": str(Path(scratch, "home")), "TMPDIR": str(Path(scratch, "tmp"))}

        def query(argv):
            return subprocess.check_output(argv, env=metadata_env, text=True, stderr=subprocess.STDOUT, timeout=60)

        versions = {"compiler": query([compiler, "--version"]).strip(), "clang_tidy": query([tidy, "--version"]).strip()}
        verified = query([tidy, "--config-file=" + str(config), "--verify-config"])
        if "warning:" in verified or "error:" in verified:
            raise RuntimeError("Invalid clang-tidy configuration: " + verified.strip())
        check_args = (["--checks=" + args.checks] if args.checks else [])
        inventory = query([tidy, "--config-file=" + str(config), "--list-checks", *check_args])
        checks = sorted(re.findall(r"^\s+([a-z][\w.*-]+)\s*$", inventory, re.M))
        (root / "checks.txt").write_text(inventory)
        before = inputs()
        git = query(["git", "-C", str(REPO), "rev-parse", "HEAD", "HEAD^{tree}"]).splitlines()
        status = query(["git", "-C", str(REPO), "status", "--porcelain"])
        try:
            packages = query(["dpkg-query", "-W", "-f=${Package}=${Version}\n", "libboost1.90-dev", "libssl-dev", "libsqlite3-dev", "zlib1g-dev", "libicu-dev", "libbrotli-dev", "libzstd-dev"]).splitlines()
        except (OSError, subprocess.CalledProcessError):
            packages = []
    tools = {"compiler_sha256": digest(Path(compiler).resolve()), "clang_tidy_sha256": digest(Path(tidy).resolve()), **versions}
    flags = ["-std=c++20", "-O0", "-g0", "-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror",
             "-DBOOST_JSON_NO_LIB", "-DBOOST_CONTAINER_NO_LIB", "-DBOOST_SYSTEM_NO_LIB",
             "-DUOS_GATEWAY_NATIVE_LIBRARIES", "-pthread", "-I" + str(GATEWAY / "include"), "-I" + str(GATEWAY)]
    sources = sorted(p for name in ("src", "tests") for p in (GATEWAY / name).rglob("*.cpp"))
    database = [{"directory": str(GATEWAY), "file": str(p),
                 "arguments": [compiler, *flags, "-c", str(p), "-o", str(root / "obj" / (str(p.relative_to(GATEWAY)) + ".o"))]}
                for p in sources]
    write_json(root / "compile_commands.json", database)
    selected = sources if not args.source else sorted({(GATEWAY / name).resolve() for name in args.source})
    if not selected or any(p not in sources and GATEWAY / "analysis/controls" not in p.parents for p in selected):
        parser.error("source must be a src/tests TU or an existing analysis/controls TU")
    # Controls use precisely the same compile arguments as ordinary units.
    for p in selected:
        if p not in sources:
            database.append({"directory": str(GATEWAY), "file": str(p), "arguments": [compiler, *flags, "-c", str(p)]})
    write_json(root / "compile_commands.json", database)
    identity = {"tools": tools, "config_sha256": digest(config), "checks": checks, "check_override": args.checks,
                "runner_sha256": digest(Path(sys.argv[1]).resolve()),
                "inputs_sha256": hashlib.sha256(encoded(before)).hexdigest(), "flags": flags}
    signature = hashlib.sha256(encoded(identity)).hexdigest()
    manifest = root / "manifest.json"
    if manifest.exists():
        shutil.copy2(manifest, root / ("manifest-" + str(time.time_ns()) + ".json"))
    write_json(manifest, {**identity, "configuration_yaml": config_text, "inputs": before, "head_commit": git[0], "head_tree": git[1], "initial_status": status})
    started = time.monotonic()
    rows = []

    def checkpoint(final=False):
        unique = {}
        occurrences = Counter()
        for row in rows:
            for d in row["diagnostics"]:
                key = (d["check"], d["file"], d["line"], d["column"], d["message"])
                unique[key] = d
                occurrences[(d["check"], d["file"])] += 1
        counts = defaultdict(Counter)
        for d in unique.values():
            counts[d["check"]][d["file"]] += 1
        unchanged = inputs() == before and digest(config) == identity["config_sha256"]
        complete = (final and len(rows) == len(selected) and selected == sources and unchanged
                    and all(row["status"] == "ok" for row in rows))
        baseline = {"schema_version": 1, "complete": complete, "head_commit": git[0], "head_tree": git[1],
                    "configuration_yaml": config_text,
                    "head_is_audit_input_identity": not bool(status), "identity": identity, "input_sha256": before,
                    "counting": "Unique (check/alias-group, gateway-relative file, line, column, message); repeated header emissions deduplicated. TU emissions also counted separately.",
                    "coverage": {"database_units": len(sources), "selected": len(selected), "completed": len(rows),
                                 "source_units": sum("src" == p.relative_to(GATEWAY).parts[0] for p in sources),
                                 "test_support_units": sum("tests" == p.relative_to(GATEWAY).parts[0] for p in sources),
                                 "ok": sum(r["status"] == "ok" for r in rows), "failed": sum(r["status"] == "failed" for r in rows),
                                 "timeouts": sum(r["status"] == "timeout" for r in rows), "inputs_unchanged": unchanged,
                                 "units_with_findings": sum(bool(r["diagnostics"]) for r in rows)},
                    "resources": {"wall_seconds": round(time.monotonic() - started, 3), "jobs": args.jobs,
                                  "sum_unit_seconds": round(sum(r["seconds"] for r in rows), 3),
                                  "sum_user_seconds": round(sum(r.get("user_s", 0) for r in rows), 3),
                                  "sum_system_seconds": round(sum(r.get("system_s", 0) for r in rows), 3),
                                  "largest_process_rss_kib": max((r.get("peak_rss_kib", 0) for r in rows), default=0)},
                    "packages": packages, "unique_diagnostics": len(unique), "tu_diagnostic_emissions": sum(occurrences.values()),
                    "checks": {c: {"unique": sum(files.values()), "files": dict(sorted(files.items())),
                                   "tu_emissions": sum(v for (check, _), v in occurrences.items() if check == c)}
                               for c, files in sorted(counts.items())},
                    "failures": [{k: r[k] for k in ("source", "status", "exit")} for r in rows if r["status"] != "ok"],
                    "completed_units": sorted(row["source"] for row in rows)}
        write_json(root / "diagnostics.json", sorted(unique.values(), key=lambda d: (d["check"], d["file"], d["line"], d["column"], d["message"])))
        write_json(args.baseline.resolve() if args.baseline else root / "tidy-baseline.json", baseline)
        return baseline

    def run(source):
        relative = str(source.relative_to(GATEWAY))
        base = root / "units" / relative
        base.parent.mkdir(parents=True, exist_ok=True)
        receipt = base.with_suffix(".json")
        log = base.with_suffix(".log")
        fixes = base.with_suffix(".yaml")
        usage = base.with_suffix(".usage")
        if args.resume and receipt.exists():
            cached = json.loads(receipt.read_text())
            if (cached.get("signature") == signature and cached.get("status") == "ok"
                    and log.is_file() and fixes.is_file() and digest(log) == cached.get("log_sha256")
                    and digest(fixes) == cached.get("fixes_sha256")):
                return cached | {"reused": True}
        if receipt.exists():
            archive = root / "attempts" / relative / str(time.time_ns())
            archive.mkdir(parents=True)
            for output in (receipt, log, fixes, usage):
                if output.exists():
                    shutil.copy2(output, archive / output.name)
        for output in (fixes, usage):
            output.unlink(missing_ok=True)
        argv = [tidy, str(source), "-p=" + str(root), "--config-file=" + str(config), "--quiet",
                "--export-fixes=" + str(fixes), *check_args]
        unit_started = time.monotonic()
        with tempfile.TemporaryDirectory(prefix="unit-", dir=root) as scratch:
            for name in ("home", "tmp", "work"):
                Path(scratch, name).mkdir()
            child_env = env | {"HOME": str(Path(scratch, "home")), "TMPDIR": str(Path(scratch, "tmp"))}
            command = ["/usr/bin/time", "-f", "%U %S %M", "-o", str(usage), *argv]
            with log.open("wb") as output:
                process = subprocess.Popen(command, cwd=Path(scratch, "work"), env=child_env,
                                           stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    code = process.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                    code = 124
        elapsed = round(time.monotonic() - unit_started, 3)
        raw = yaml.safe_load(fixes.read_text()) if fixes.exists() else {}
        diagnostics = []
        for diagnostic in (raw or {}).get("Diagnostics", []):
            message = diagnostic["DiagnosticMessage"]
            path = Path(message.get("FilePath", ""))
            if not path.is_absolute():
                path = GATEWAY / path
            path = path.resolve()
            # Enforce first-party scope on exported diagnostics as well as console output.
            if GATEWAY not in path.parents or not path.is_file():
                continue
            offset = message.get("FileOffset", 0)
            prefix = path.read_bytes()[:offset]
            diagnostics.append({"file": str(path.relative_to(GATEWAY)), "line": prefix.count(b"\n") + 1,
                                "column": len(prefix.rsplit(b"\n", 1)[-1]) + 1, "check": diagnostic["DiagnosticName"],
                                "message": message["Message"], "level": diagnostic.get("Level", "Warning")})
        # Compiler errors invalidate a unit even if it also has ordinary tidy warnings.
        failed = code != 0 or any(d["level"] in {"Error", "Fatal"} for d in diagnostics)
        row = {"source": relative, "status": "timeout" if code == 124 else "failed" if failed else "ok",
               "exit": code, "seconds": elapsed, "signature": signature, "command": argv, "diagnostics": diagnostics,
               "log_sha256": digest(log), "fixes_sha256": digest(fixes) if fixes.exists() else None}
        if usage.exists():
            match = re.search(r"^([\d.]+) ([\d.]+) (\d+)$", usage.read_text(), re.M)
            if match:
                row.update(user_s=float(match[1]), system_s=float(match[2]), peak_rss_kib=int(match[3]))
        write_json(receipt, row)
        return row

    print(json.dumps({"event": "start", "units": len(selected), "database_units": len(sources), "checks": len(checks), "build_dir": str(root)}), flush=True)
    checkpoint()
    last_progress = 0.0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for future in as_completed([pool.submit(run, p) for p in selected]):
            row = future.result()
            rows.append(row)
            now = time.monotonic()
            if row["status"] != "ok" or now - last_progress >= 30 or len(rows) == len(selected):
                print(json.dumps({"done": len(rows), "total": len(selected), "failed": sum(r["status"] != "ok" for r in rows),
                                  "last": row["source"], "last_seconds": row["seconds"], "elapsed_s": round(now - started, 1)}), flush=True)
                last_progress = now
            write_json(root / "progress.json", {"done": len(rows), "total": len(selected), "failures": [r["source"] for r in rows if r["status"] != "ok"]})
            if len(rows) % args.checkpoint_every == 0:
                checkpoint()
    baseline = checkpoint(final=True)
    print(json.dumps({"event": "finish", "complete": baseline["complete"], "coverage": baseline["coverage"], "unique_diagnostics": baseline["unique_diagnostics"], "resources": baseline["resources"]}), flush=True)
    return int(not baseline["coverage"]["inputs_unchanged"] or any(row["status"] != "ok" for row in rows)
               or (args.fail_on_findings and baseline["unique_diagnostics"] != 0))


try:
    sys.exit(main())
except (OSError, RuntimeError, ValueError, subprocess.SubprocessError, yaml.YAMLError) as error:
    sys.exit("gateway tidy: " + str(error))
PYTHON
