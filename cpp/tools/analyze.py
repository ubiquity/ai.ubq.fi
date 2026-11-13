#!/usr/bin/python3
"""Bounded offline clang-tidy gate for the pilot's actual compilation model."""
import fnmatch
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shlex
import signal
import subprocess
import sys
import time


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def policy(path):
    # This owned policy is deliberately limited to flat JSON scalar values in YAML.
    value = {}
    for line in Path(path).read_text().splitlines():
        match = re.fullmatch(r"([A-Za-z]+): (.+)", line)
        if not match or match[1] in value:
            raise ValueError("Analysis policy requires unique flat scalar settings")
        value[match[1]] = json.loads(match[2])
    if set(value) != {"Checks", "WarningsAsErrors", "InheritParentConfig", "HeaderFilterRegex"}:
        raise ValueError("Analysis policy must contain only explicit check/failure settings")
    if value["WarningsAsErrors"] != "*" or value["InheritParentConfig"] is not False or value["HeaderFilterRegex"] != ".*":
        raise ValueError("Analysis must retain fatal diagnostics and isolated configuration")
    patterns = value["Checks"].split(",")
    if patterns[0] != "-*" or len(patterns) < 2 or any(not p or p.startswith("-") for p in patterns[1:]):
        raise ValueError("Analysis policy requires an explicit positive check list after -*")
    return value


def check_names(output):
    lines = output.splitlines()
    if not lines or lines[0] != "Enabled checks:":
        raise ValueError("Malformed clang-tidy checker inventory")
    names = [line.strip() for line in lines[1:] if line.strip()]
    if not names or len(set(names)) != len(names) or any(not re.fullmatch(r"[a-zA-Z0-9_.-]+", name) for name in names):
        raise ValueError("Empty, duplicate or malformed clang-tidy checker inventory")
    return set(names)


def supported_checks(configuration, available):
    selected = set()
    for pattern in configuration["Checks"].split(",")[1:]:
        matches = {name for name in available if fnmatch.fnmatchcase(name, pattern)}
        if not matches:
            raise ValueError("Unsupported required clang-tidy check: " + pattern)
        selected.update(matches)
    return selected


def compilation_units(path, workspace, build, sdk):
    entries = json.loads(Path(path).read_text())
    expected = set((workspace / "cpp/src").glob("*.cpp")) | {workspace / "cpp/tests/protocol_tests.cpp"}
    expected |= {workspace / "cpp/tests/analysis" / name for name in ("raw_defects.cpp", "clean_ownership.cpp", "clean_cancel.cpp")}
    if not isinstance(entries, list) or len(entries) != len(expected):
        raise ValueError("Compilation database must cover every production and protocol-test TU exactly once")
    found = set()
    for entry in entries:
        source = Path(entry["file"])
        if source not in expected or source in found or not source.is_file() or Path(entry["directory"]) != build:
            raise ValueError("Duplicate, unexpected, missing or foreign compilation unit")
        if ("arguments" in entry) == ("command" in entry):
            raise ValueError("Each compilation entry needs exactly one command representation")
        argv = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
        if not isinstance(argv, list) or not argv or not all(isinstance(value, str) for value in argv):
            raise ValueError("Invalid compilation command")
        compiler = sdk / "root/usr/bin/aarch64-linux-gnu-g++-12"
        if Path(argv[0]) != compiler or "--sysroot=" + str(sdk / "root") not in argv or "-std=c++20" not in argv:
            raise ValueError("Analysis requires the pinned Pi C++20 compiler/sysroot model")
        if "-c" not in argv or argv[argv.index("-c") + 1] != str(source):
            raise ValueError("Compilation command does not name its source")
        found.add(source)
    if found != expected:
        raise ValueError("Compilation database coverage differs from the first-party source set")
    return sorted(found)


def consumer_profile(directory, source):
    profiles = list(directory.glob("*.json"))
    if len(profiles) != 1:
        raise ValueError("Each TU requires exactly one fresh AST-consumer profile")
    value = json.loads(profiles[0].read_text())
    if not isinstance(value, dict) or Path(value["file"]).resolve() != source.resolve():
        raise ValueError("AST-consumer profile names a different source")
    if not isinstance(value.get("profile"), dict) or not isinstance(value.get("timestamp"), str):
        raise ValueError("Malformed AST-consumer profile")
    return {"path": str(profiles[0]), "sha256": digest(profiles[0]), "file": value["file"], "timestamp": value["timestamp"]}


def diagnostic_report(path, source, raw, workspace, output, label, timeout):
    exported = path.read_text()
    if not exported.startswith("---\n") or not exported.rstrip().endswith("\n..."):
        raise ValueError("Incomplete clang-tidy YAML document")
    common = Path(subprocess.check_output(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                         cwd=workspace, text=True).strip()).parent
    bun = common / ".data/runtimes/bun/linux-arm64/1.4.0-086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9/bun"
    if not bun.is_file():
        raise FileNotFoundError("The pinned Bun YAML parser is unavailable")
    script = ('if(Bun.version!=="1.4.0"||typeof Bun.YAML?.parse!=="function")throw new Error("Pinned YAML parser unavailable");'
              'console.log(JSON.stringify(Bun.YAML.parse(await Bun.file(process.argv[1]).text())));')
    parsed = execute([str(bun), "--no-env-file", "--no-install", "--eval", script, str(path)],
                     workspace, output, label + "-yaml", min(timeout, 15))
    if parsed["exit"] != 0 or Path(parsed["stderr"]).read_bytes():
        raise ValueError("Malformed clang-tidy YAML report; parser output retained")
    value = json.loads(Path(parsed["stdout"]).read_text())
    if not isinstance(value, dict) or set(value) != {"MainSourceFile", "Diagnostics"}:
        raise ValueError("Invalid clang-tidy report structure")
    if value["MainSourceFile"] != str(source) or not isinstance(value["Diagnostics"], list):
        raise ValueError("Source-mismatched report or invalid Diagnostics payload")
    names = []
    for entry in value["Diagnostics"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("DiagnosticName"), str):
            raise ValueError("Malformed diagnostic entry")
        message = entry.get("DiagnosticMessage")
        if (not isinstance(message, dict) or not isinstance(message.get("Message"), str)
                or not isinstance(message.get("FilePath"), str) or type(message.get("FileOffset")) is not int
                or message["FileOffset"] < 0 or not isinstance(message.get("Replacements"), list)):
            raise ValueError("Malformed diagnostic message")
        names.append(entry["DiagnosticName"])
    raw_names = [name for group in re.findall(r":\d+:\d+: (?:warning|error): [^\n]*\[([^\]]+)\]", raw)
                 for name in group.split(",") if not name.startswith("-")]
    if Counter(names) != Counter(raw_names):
        raise ValueError("Raw diagnostics and YAML diagnostic IDs disagree")
    return names, value["Diagnostics"]


def beast_baseline(path, sdk, workspace):
    baseline = json.loads(path.read_text())
    if baseline["schema"] != "uos-clang14-beast-read-v1" or baseline["source"] != "cpp/src/session.cpp" or len(baseline["diagnostics"]) != 4:
        raise ValueError("Invalid finite Beast diagnostic baseline")
    expected_sdk = {"root/lib/ld-linux-aarch64.so.1", "root/usr/bin/clang-tidy-14",
                    "root/usr/lib/aarch64-linux-gnu/libclang-cpp.so.14", "root/usr/lib/aarch64-linux-gnu/libLLVM-14.so.1",
                    "root/usr/include/boost/beast/http/impl/read.hpp", "root/usr/include/boost/asio/coroutine.hpp",
                    "root/usr/include/boost/asio/compose.hpp"}
    if set(baseline["sdk_sha256"]) != expected_sdk:
        raise ValueError("Incomplete Beast baseline identity binding")
    for relative, expected in baseline["sdk_sha256"].items():
        if digest(sdk / relative) != expected:
            raise ValueError("Stale Beast baseline identity: " + relative)
    if digest(workspace / "cpp/.clang-tidy") != baseline["policy_sha256"]:
        raise ValueError("Stale Beast baseline check policy")
    return baseline


def beast_fingerprint(diagnostic, sdk):
    if diagnostic.get("DiagnosticName") != "bugprone-use-after-move" or diagnostic.get("Level") != "Warning":
        return None
    def location(value):
        if set(value) != {"Message", "FilePath", "FileOffset", "Replacements"} or value["Replacements"] != []:
            raise ValueError("Changed Beast diagnostic location structure")
        path = Path(value["FilePath"])
        relative = path.relative_to(sdk / "root/usr/include")
        if str(relative) != "boost/beast/http/impl/read.hpp":
            raise ValueError("Finding is outside the reviewed Beast header")
        data, offset = path.read_bytes(), value["FileOffset"]
        if type(offset) is not int or offset < 0 or offset >= len(data):
            raise ValueError("Invalid Beast diagnostic offset")
        return {"path": str(relative), "offset": offset, "line": data[:offset].count(b"\n") + 1,
                "column": offset - data.rfind(b"\n", 0, offset), "message": value["Message"], "replacements": []}
    try:
        return {"check": diagnostic["DiagnosticName"], "primary": location(diagnostic["DiagnosticMessage"]),
                "notes": [location(note) for note in diagnostic["Notes"]], "level": diagnostic["Level"]}
    except (ValueError, KeyError, TypeError):
        return None


def classify_beast(results, baseline, sdk, workspace):
    expected = Counter(json.dumps(value, sort_keys=True) for value in baseline["diagnostics"])
    observed, classified, new = Counter(), [], []
    for result in results:
        for index, diagnostic in enumerate(result["diagnostics"]):
            fingerprint = beast_fingerprint(diagnostic, sdk) if result["source"] == str(workspace / baseline["source"]) else None
            key = json.dumps(fingerprint, sort_keys=True)
            item = {"source": result["source"], "diagnostic_index": index, "check": diagnostic["DiagnosticName"]}
            if fingerprint is not None and observed[key] < expected[key]:
                observed[key] += 1
                classified.append(item)
            else:
                new.append(item)
    missing = sum((expected - observed).values())
    failed_exits = sum(result["exit"] != 0 and (result["exit"] != 1 or not result["diagnostics"]) for result in results)
    return {"classified_count": len(classified), "new_count": len(new), "missing_count": missing,
            "unclassified_exit_count": failed_exits, "classified": classified, "new": new,
            "raw_child_exits": [result["exit"] for result in results]}


def execute(argv, cwd, output, label, timeout):
    """Retain raw output, propagate timeout/crash, and settle the exact child group."""
    if timeout <= 0:
        raise TimeoutError("Analysis deadline exhausted")
    stdout, stderr = output / (label + ".stdout"), output / (label + ".stderr")
    started = time.monotonic()
    timed_out = False
    with stdout.open("wb") as out, stderr.open("wb") as err:
        child = subprocess.Popen(argv, cwd=cwd, env={"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "TMPDIR": str(output)},
                                 stdout=out, stderr=err, start_new_session=True)
        try:
            child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait(timeout=3)
    result = {"argv": list(map(str, argv)), "exit": child.returncode, "timed_out": timed_out,
              "duration_s": round(time.monotonic() - started, 6), "stdout": str(stdout), "stderr": str(stderr)}
    (output / (label + ".json")).write_text(json.dumps(result, indent=2) + "\n")
    if timed_out:
        raise TimeoutError("Analysis command timed out; raw output: " + str(stderr))
    if max(stdout.stat().st_size, stderr.stat().st_size) > 16 * 1024 * 1024:
        raise ValueError("Analysis output exceeded its bound; raw files retained")
    return result


class Tool:
    def __init__(self, sdk, workspace, output):
        self.sdk, self.workspace, self.output = Path(sdk), Path(workspace), Path(output)
        self.binary = self.sdk / "root/usr/bin/clang-tidy-14"
        loader = self.sdk / "root/lib/ld-linux-aarch64.so.1"
        limit = self.workspace / "cpp/tools/compiler-limit.py"
        for path in (self.binary, loader, limit):
            if not path.is_file():
                raise FileNotFoundError("Required analyzer artifact is missing: " + str(path))
        libraries = str(self.sdk / "root/lib/aarch64-linux-gnu") + ":" + str(self.sdk / "root/usr/lib/aarch64-linux-gnu")
        self.prefix = ["/usr/bin/python3", str(limit), str(loader), "--library-path", libraries, str(self.binary)]

    def metadata(self, configuration, config_path):
        version = execute(self.prefix + ["--version"], self.workspace, self.output, "version", 15)
        text = Path(version["stdout"]).read_text()
        if version["exit"] != 0 or Path(version["stderr"]).read_bytes() or "LLVM version 14.0.6" not in text:
            raise ValueError("The qualified clang-tidy 14.0.6 tool is unavailable")
        catalog = execute(self.prefix + ["--config-file=" + str(config_path), "--checks=*", "--list-checks"],
                          self.workspace, self.output, "catalog", 15)
        if catalog["exit"] != 0 or Path(catalog["stderr"]).read_bytes():
            raise ValueError("clang-tidy checker inventory failed")
        available = check_names(Path(catalog["stdout"]).read_text())
        required = supported_checks(configuration, available)
        selected = execute(self.prefix + ["--config-file=" + str(config_path), "--list-checks"],
                           self.workspace, self.output, "selected", 15)
        if selected["exit"] != 0 or Path(selected["stderr"]).read_bytes():
            raise ValueError("clang-tidy selected checker inventory failed")
        enabled = check_names(Path(selected["stdout"]).read_text())
        if not required <= enabled:
            raise ValueError("clang-tidy omitted required checks")
        return {"version": text.strip(), "binary_sha256": digest(self.binary), "required_checks": sorted(required),
                "enabled_checks": sorted(enabled), "metadata_commands": [version, catalog, selected]}

    def analyze(self, source, database, config_path, label, timeout):
        started = time.monotonic()
        report = self.output / (label + ".yaml")
        profiles = self.output / (label + "-profiles")
        profiles.mkdir()
        argv = self.prefix + [str(source), "-p", str(database), "--config-file=" + str(config_path),
                              "--system-headers", "--enable-check-profile", "--store-check-profile=" + str(profiles),
                              "--extra-arg=--gcc-toolchain=" + str(self.sdk / "root/usr"),
                              "--extra-arg=-resource-dir=" + str(self.sdk / "root/usr/lib/llvm-14/lib/clang/14.0.6"),
                              "--export-fixes=" + str(report)]
        result = execute(argv, self.workspace, self.output, label, timeout)
        raw = Path(result["stdout"]).read_text() + Path(result["stderr"]).read_text()
        frontend_failed = bool(re.search(r"clang-diagnostic-error|Error while processing|LLVM ERROR:|fatal error:|"
                                        r"Error while trying to load a compilation database|Running without flags|Skipping .*file", raw))
        profile = consumer_profile(profiles, Path(source))
        names, diagnostics = [], []
        if report.is_file():
            names, diagnostics = diagnostic_report(report, source, raw, self.workspace, self.output, label,
                                                   timeout - (time.monotonic() - started))
        elif result["exit"] != 0 or frontend_failed or re.search(r":\d+:\d+: (?:warning|error):", raw):
            raise ValueError("Missing clang-tidy diagnostic report: " + str(report))
        # Pinned Clang14 omits export-fixes entirely when there are no selected diagnostics.
        result.update(source=str(source), report=str(report) if report.is_file() else None, diagnostic_ids=names,
                      diagnostics=diagnostics, consumer_profile=profile,
                      report_status="exported" if report.is_file() else "not-emitted-for-clean-input",
                      frontend_success=not frontend_failed and result["exit"] in (0, 1))
        if result["exit"] == 0 and (frontend_failed or names):
            raise ValueError("clang-tidy returned success without a clean frontend/report")
        return result


def controls(tool, units, build, config_path):
    started = time.monotonic()
    results = []
    for index, source in enumerate(units):
        result = tool.analyze(source, build, config_path, "control-" + str(index + 1), 300 - (time.monotonic() - started))
        results.append(result)
        if not result["frontend_success"]:
            raise ValueError("Control frontend failed: " + str(source))
        if source.name == "raw_defects.cpp":
            expected = {"clang-analyzer-unix.Malloc", "clang-analyzer-cplusplus.NewDeleteLeaks",
                        "clang-analyzer-cplusplus.NewDelete", "clang-analyzer-unix.MismatchedDeallocator",
                        "clang-analyzer-core.CallAndMessage", "bugprone-use-after-move", "clang-analyzer-cplusplus.Move"}
            if result["exit"] != 1 or len(result["diagnostic_ids"]) != 7 or set(result["diagnostic_ids"]) != expected:
                raise ValueError("Dirty control did not produce its exact seven required findings")
            raw = Path(result["stdout"]).read_text()
            markers = [(source, "expect-malloc", "clang-analyzer-unix.Malloc"),
                       (source, "expect-new-leak", "clang-analyzer-cplusplus.NewDeleteLeaks"),
                       (source, "expect-new-uaf", "clang-analyzer-cplusplus.NewDelete"),
                       (source, "expect-uninitialized", "clang-analyzer-core.CallAndMessage"),
                       (source, "expect-genuine-move", "bugprone-use-after-move"),
                       (source, "expect-genuine-move", "clang-analyzer-cplusplus.Move"),
                       (source.parent / "fixture_sdk/release.hpp", "expect-mismatch", "clang-analyzer-unix.MismatchedDeallocator")]
            for path, marker, check in markers:
                line = next(i + 1 for i, value in enumerate(path.read_text().splitlines()) if marker in value)
                pattern = re.escape(str(path)) + ":" + str(line) + r":\d+: error: [^\n]*\[" + re.escape(check) + r"(?:,|\])"
                if not re.search(pattern, raw):
                    raise ValueError("Control finding missing its independently marked location: " + marker)
            if "allocation-owned" not in source.read_text() or not re.search(re.escape(str(source)) + r":\d+:\d+: note: Memory is allocated", raw):
                raise ValueError("System-header mismatch lost its first-party allocation path")
        elif result["exit"] != 0 or result["diagnostic_ids"]:
            raise ValueError("Clean ownership/cancellation control produced diagnostics")
    return results


def main():
    if len(sys.argv) not in (3, 4) or sys.argv[2] != "pi-release" or (len(sys.argv) == 4 and sys.argv[3] not in ("all", "controls", "check")):
        raise ValueError("usage: analyze.py SDK pi-release [all|controls|check]")
    mode = sys.argv[3] if len(sys.argv) == 4 else "all"
    workspace = Path(__file__).resolve().parents[2]
    sdk = Path(sys.argv[1]).resolve()
    build = workspace / ".data/cpp-build/pi-release"
    config_path = workspace / "cpp/.clang-tidy"
    output = workspace / ".data/cpp-evidence" / ("analysis-" + str(time.time_ns()))
    output.mkdir(parents=True, mode=0o700)
    started = time.monotonic()
    receipt = {"workspace": str(workspace), "preset": "pi-release", "mode": mode, "units": [], "status": "failed",
               "runner_sha256": digest(__file__), "compiler_limit_sha256": digest(workspace / "cpp/tools/compiler-limit.py")}
    code = 1
    try:
        configuration = policy(config_path)
        units = compilation_units(build / "compile_commands.json", workspace, build, sdk)
        receipt.update(configuration=configuration, config_sha256=digest(config_path),
                       compile_database_sha256=digest(build / "compile_commands.json"), expected_units=list(map(str, units)),
                       source_sha256={str(path.relative_to(workspace)): digest(path) for path in units + sorted((workspace / "cpp/include").rglob("*.hpp"))})
        tool = Tool(sdk, workspace, output)
        receipt["tool"] = tool.metadata(configuration, config_path)
        baseline_path = workspace / "cpp/clang14-beast-read-baseline.json"
        baseline = beast_baseline(baseline_path, sdk, workspace)
        receipt["baseline_sha256"] = digest(baseline_path)
        fixtures = [source for source in units if source.parent == workspace / "cpp/tests/analysis"]
        real = [source for source in units if source not in fixtures]
        receipt.update(expected_real_units=list(map(str, real)), expected_control_units=list(map(str, fixtures)))
        if mode != "check":
            receipt["controls"] = controls(tool, fixtures, build, config_path)
        if mode != "controls":
            phase_start = time.monotonic()
            for index, source in enumerate(real):
                result = tool.analyze(source, build, config_path, "tu-" + str(index + 1), 300 - (time.monotonic() - phase_start))
                receipt["units"].append(result)
                if not result["frontend_success"]:
                    raise ValueError("Frontend failed for " + str(source))
        if mode != "controls":
            classification = classify_beast(receipt["units"], baseline, sdk, workspace)
            receipt["classification"] = classification
            code = int(classification["new_count"] != 0 or classification["missing_count"] != 0 or classification["unclassified_exit_count"] != 0)
            receipt["status"] = "passed_with_four_known_diagnostics" if code == 0 else "findings_or_stale_baseline"
        else:
            code, receipt["status"] = 0, "controls_passed"
    except (OSError, ValueError, KeyError, IndexError, TypeError, TimeoutError, subprocess.SubprocessError) as error:
        receipt["error"] = str(error)
    finally:
        receipt["duration_s"] = round(time.monotonic() - started, 6)
        receipt["child_max_rss_kib"] = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        receipt["rss_scope"] = "RUSAGE_CHILDREN cumulative maximum for this serial invocation, not cgroup peak"
        receipt["source_revision"] = subprocess.check_output(["git", "rev-parse", "HEAD", "HEAD^{tree}"], cwd=workspace, text=True).splitlines()
        receipt["git_status"] = subprocess.check_output(["git", "status", "--porcelain"], cwd=workspace, text=True)
        path = output / "receipt.json"
        path.write_text(json.dumps(receipt, indent=2) + "\n")
        classification = receipt.get("classification", {})
        print(json.dumps({"status": receipt["status"], "receipt": str(path), "units": len(receipt["units"]),
                          "classified": classification.get("classified_count"), "new": classification.get("new_count"),
                          "missing": classification.get("missing_count"), "error": receipt.get("error")}))
    return code


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
