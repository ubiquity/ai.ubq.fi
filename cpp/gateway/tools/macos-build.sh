#!/bin/bash
# Disposable Darwin build/test driver. Dependencies must already be prepared.
set -euo pipefail
UOS_MACOS_GATEWAY=$(cd "$(dirname "$0")/.." && pwd)
export UOS_MACOS_GATEWAY
exec python3 - "$@" <<'PYTHON'
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
parser = argparse.ArgumentParser(description="Build native gateway executables; run isolated Darwin tests, runtime first")
parser.add_argument("--build-root", type=Path, default=Path(tempfile.gettempdir()) / "cppgw-macos-native")
parser.add_argument("--jobs", type=int, default=4)
parser.add_argument("--suite", choices=["all", "rt", "none"], default="all")
parser.add_argument("--filter", default="", help="Optional comma-separated test path substrings")
parser.add_argument("--case", default="", help="Optional harness case substring")
parser.add_argument("--timeout", type=int, default=180)
parser.add_argument("--sqlite-prefix", type=Path, help="Prepared SQLite 3.54.0 prefix for the versioned native fixtures")
parser.add_argument("--legacy-loaders", action="store_true", help="Compile the three C-library loaders through their Darwin dlopen branches")
parser.add_argument("--require-clean", action="store_true")
parser.add_argument("--release-commit", help="Full lowercase Git commit bound into a packaged release")
args = parser.parse_args()
if platform.system() != "Darwin" or platform.machine() != "arm64":
    parser.error("This driver requires native macOS Apple Silicon")
if args.jobs < 1 or args.timeout < 1:
    parser.error("jobs and timeout must be positive")
if args.release_commit and not re.fullmatch(r"[0-9a-f]{40}", args.release_commit):
    parser.error("release-commit must be the full 40-character lowercase Git commit")
gateway = Path(os.environ["UOS_MACOS_GATEWAY"]).resolve()
repo = gateway.parent.parent
sys.path.insert(0, str(gateway / "tools"))
import compiler_cache
import vendor_sources

vendors = tuple(vendor_sources.selected(gateway))
simdjson_sources = vendor_sources.simdjson_consumers(vendors)
build = args.build_root.resolve()
if build == repo or repo in build.parents:
    parser.error("Build output must be outside the checkout")
build.mkdir(parents=True, exist_ok=True)
build_lock = (build / ".build.lock").open("w")
try:
    fcntl.flock(build_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    parser.error("Another invocation owns this build root")
run = build / "runs" / str(time.time_ns())
run.mkdir(parents=True)
(build / "build-home").mkdir(exist_ok=True)
env = {"PATH": "/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin", "LC_ALL": "C", "TZ": "UTC", "HOME": str(build / "build-home"),
       "HOMEBREW_NO_AUTO_UPDATE": "1", "HOMEBREW_NO_ANALYTICS": "1"}

def capture(command):
    return subprocess.check_output(list(map(str, command)), env=env, text=True).strip()

def identity():
    commit, tree = capture(["git", "-C", repo, "rev-parse", "HEAD", "HEAD^{tree}"]).splitlines()
    clean = not capture(["git", "-C", repo, "status", "--porcelain"])
    return dict(head_commit=commit, head_tree=tree, worktree_clean=clean,
                commit=commit if clean else None, tree=tree if clean else None)

source_identity = identity()
if args.require_clean and not source_identity["worktree_clean"]:
    parser.error("--require-clean requires a committed, clean checkout")
inputs = {str(p.relative_to(gateway)): hashlib.sha256(p.read_bytes()).hexdigest()
          for directory in ["entrypoints", "include", "runtime", "src", "tests", "tools"]
          for p in sorted((gateway / directory).rglob("*")) if p.is_file()}
for relative in [vendor_sources.REGISTRY, *(relative for library in vendors for relative in library.inputs)]:
    inputs[str(relative)] = hashlib.sha256((gateway / relative).read_bytes()).hexdigest()
brew = shutil.which("brew")
if not brew:
    parser.error("Homebrew is required; see docs/MACOS.md")
prefixes = {name: Path(capture([brew, "--prefix", name])) for name in
            ["boost", "openssl@3", "sqlite", "zlib", "icu4c@78", "brotli", "zstd"]}
if args.sqlite_prefix:
    prefixes["sqlite"] = args.sqlite_prefix.resolve()
env["PKG_CONFIG_LIBDIR"] = ":".join(str(p / "lib/pkgconfig") for p in prefixes.values())
compiler = shutil.which(os.environ["CXX"]) if os.environ.get("CXX") else capture(["xcrun", "--find", "clang++"])
c_compiler = shutil.which(os.environ["CC"]) if os.environ.get("CC") else capture(["xcrun", "--find", "clang"])
if not compiler or not c_compiler:
    parser.error("CC and CXX must name available compilers")
compiler, c_compiler = map(os.path.abspath, (compiler, c_compiler))
env["SDKROOT"] = capture(["xcrun", "--show-sdk-path"])
try:
    launcher = compiler_cache.launcher()
except ValueError as error:
    parser.error(str(error))
checkout_flags = [f"-fdebug-prefix-map={repo}=."]
packages = ["openssl", "zlib", "libbrotlidec", "libzstd", "icu-uc", "icu-i18n"]
cflags = shlex.split(capture(["pkg-config", "--cflags-only-other", *packages]))
for include in shlex.split(capture(["pkg-config", "--cflags-only-I", *packages])):
    cflags.extend(["-isystem", str(Path(include.removeprefix("-I")).resolve())])
libs = shlex.split(capture(["pkg-config", "--libs", *packages]))
sqlite = prefixes["sqlite"]
flags = [compiler, "-std=c++20", "-stdlib=libc++", "-O0", "-g0", "-Wall", "-Wextra", "-Wshadow",
         "-Wconversion", "-Werror", "-DBOOST_JSON_NO_LIB", "-DBOOST_CONTAINER_NO_LIB", "-DBOOST_SYSTEM_NO_LIB",
         "-I", str(gateway / "include"), "-I", str(gateway), "-isystem", str(prefixes["boost"] / "include"),
         "--system-header-prefix=third_party/",
         "-isystem", str(sqlite / "include"), "-pthread", *cflags]
flags.append("-DUOS_GATEWAY_NATIVE_LIBRARIES")
release_identity_sources = {Path("src/release.cpp"), Path("tests/release_identity_test.cpp"), Path("tests/health_test.cpp")}
vendor_c_flags = [c_compiler, "-std=c11", "-O0", "-g0"]
vendor_cxx_flags = vendor_sources.vendor_flags(flags)
libs += [str(sqlite / "lib/libsqlite3.dylib"), "-L" + str(prefixes["boost"] / "lib"), "-lboost_json", "-lboost_container"]
# dyld fallback search is explicit and credential-free; native links use the exact dylib above.
env["DYLD_LIBRARY_PATH"] = ":".join(str(p / "lib") for p in prefixes.values())
build_env = compiler_cache.environment(env, repo, "macos") if launcher else env
test_build_env = (compiler_cache.environment(env, repo, "macos-tests-" + hashlib.sha256(str(repo).encode()).hexdigest())
                  if launcher else env)
provenance = dict(source_identity=source_identity, platform=platform.platform(), compiler=capture([compiler, "--version"]),
                  c_compiler=capture([c_compiler, "--version"]),
                  compiler_launcher=launcher,
                  sdk=env["SDKROOT"], dependencies=capture([brew, "list", "--versions", *prefixes]),
                  package_versions=capture(["pkg-config", "--modversion", *packages]),
                  flags=flags, libraries=libs, inputs=inputs, arguments=vars(args))
provenance["dependency_artifacts"] = {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest()
                                      for prefix in prefixes.values() for p in sorted((prefix / "lib").glob("*.dylib")) if p.is_file()}
(run / "provenance.json").write_text(json.dumps(provenance, indent=2, default=str) + "\n")
print("output:", build, "run:", run.name, flush=True)

def invoke(command, log, timeout=300, *, environment=env, cwd=None):
    start = time.monotonic()
    with log.open("w") as output:
        output.write(shlex.join(list(map(str, command))) + "\n")
        output.flush()
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, env=environment, cwd=cwd, start_new_session=True)
        try:
            status = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            status = 124
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    return dict(status=status, seconds=round(time.monotonic() - start, 3), command=list(map(str, command)), log=str(log))

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

compilers = {command: digest(command) for command in (compiler, c_compiler)}

def obj(source):
    return build / "obj" / source.with_suffix(".o")

def compile_one(source, selected_flags=None):
    target = obj(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    source_flags = flags if selected_flags is None else selected_flags
    if args.release_commit and source in release_identity_sources:
        # Only the identity consumers vary by package commit; all other
        # translation units keep the same compiler-cache key.
        source_flags = source_flags + ['-DUOS_PACKAGE_GIT_COMMIT="' + args.release_commit + '"']
    if str(source) in simdjson_sources:
        source_flags = source_flags + vendor_sources.simdjson_flags(gateway)
    if args.legacy_loaders and str(source) in {"src/rt/gzip.cpp", "src/rt/sqlite.cpp", "src/rt/fetch_transport_codec.cpp"}:
        source_flags = [f for f in flags if f != "-DUOS_GATEWAY_NATIVE_LIBRARIES"]
    for library in vendor_sources.required(vendors, [source]):
        source_flags = source_flags + vendor_sources.include_flags(library, gateway)
    path_flags = checkout_flags
    compile_env = build_env
    if source.parts[0] == "tests":
        # ccache makes checkout paths relative. Keep test fixture lookups bound
        # to the actual checkout used by the Darwin sandbox.
        path_flags = [*checkout_flags, f"-fmacro-prefix-map={gateway.relative_to(repo)}={gateway}"]
        compile_env = test_build_env
    command = launcher + source_flags + path_flags + ["-MMD", "-MF", str(target.with_suffix(".d")),
                                                      "-c", str(gateway / source), "-o", str(target)]
    compiler_hash = compilers[source_flags[0]]
    receipt = target.with_suffix(".json")
    previous = json.loads(receipt.read_text()) if receipt.exists() else {}
    valid = (previous.get("status") == 0 and target.exists() and previous.get("command") == command
             and previous.get("compiler_sha256") == compiler_hash)
    if valid:
        valid = all(Path(p).is_file() and digest(p) == h for p, h in previous["inputs"].items())
    if valid:
        return dict(previous, cached=True)
    result = invoke(command, target.with_suffix(".log"), environment=compile_env, cwd=repo)
    result["source"] = str(source)
    result["compiler_sha256"] = compiler_hash
    if result["status"] == 0:
        deps = shlex.split(target.with_suffix(".d").read_text().replace("\\\n", " ").split(":", 1)[1])
        # -MMD omits system headers, including nested headers from the registry.
        for library in vendors:
            if str(source) in library.consumers or str(source) in library.sources:
                deps.extend(str(gateway / header) for header in library.headers)
        result["inputs"] = {str(repo / p): digest(repo / p) for p in deps}
    receipt.write_text(json.dumps(result, indent=2) + "\n")
    return result

def compile_phase(name, sources):
    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        pending = {pool.submit(compile_one, p): p for p in sources}
        for future in as_completed(pending):
            result = future.result()
            results.append(result)
            if result["status"] or len(results) % 25 == 0:
                print(name, len(results), "/", len(sources),
                      ("FAIL " + result["source"] + " (" + result["log"] + ")") if result["status"] else "compiled", flush=True)
    (run / (name + "-compile.json")).write_text(json.dumps(results, indent=2) + "\n")
    print(name, "compile", sum(r["status"] == 0 for r in results), "passed", sum(r["status"] != 0 for r in results), "failed", flush=True)
    return results

runtime = sorted(p.relative_to(gateway) for p in (gateway / "src/rt").glob("*.cpp"))
modules = [p.relative_to(gateway) for p in sorted((gateway / "src").rglob("*.cpp")) if p.relative_to(gateway) not in runtime]
startup = Path("runtime/startup.cpp")
entrypoints = {
    "uos-gateway": Path("entrypoints/gateway.cpp"),
}
production_sources = runtime + modules + [startup, *entrypoints.values()]
production = compile_phase("production", production_sources)
if any(r["status"] for r in production):
    raise SystemExit(1)
archive_path = run / "libuos_gateway.a"
archive = invoke(["/usr/bin/ar", "rcs", str(archive_path), *map(obj, runtime + modules)], run / "archive.log")
if archive["status"]:
    raise SystemExit("Archive failed: " + archive["log"])
members = [name for name in capture(["/usr/bin/ar", "-t", archive_path]).splitlines() if not name.startswith("__.SYMDEF")]
if len(members) != len(runtime + modules):
    raise SystemExit("Archive member census does not match production sources")
gateway_archive = build / archive_path.name
archive_path.replace(gateway_archive)
vendor_archives = {}
for library in vendors:
    members = []
    for source in library.sources:
        relative = Path(source)
        compiler_flags = vendor_c_flags if relative.suffix == ".c" else vendor_cxx_flags
        compiled = compile_one(relative, compiler_flags + vendor_sources.c_dialect_flags(source) +
                               vendor_sources.include_flags(library, gateway))
        if compiled["status"]:
            raise SystemExit("Vendor compile failed: " + compiled["log"])
        members.append(obj(relative))
    archive_path = run / library.archive
    archived = invoke(["/usr/bin/ar", "rcs", archive_path, *members], run / (library.archive + ".log"))
    if archived["status"]:
        raise SystemExit("Vendor archive failed: " + archived["log"])
    target = build / library.archive
    archive_path.replace(target)
    vendor_archives[library.archive] = target
provenance["vendor_archives"] = {name: digest(path) for name, path in vendor_archives.items()}
print("vendor archives:", ", ".join(vendor_archives), flush=True)
link_flags = [compiler, "-stdlib=libc++", "-pthread"]
production_vendors = [vendor_archives[library.archive] for library in vendor_sources.required(vendors, production_sources)]
executables = {}
for name, source in entrypoints.items():
    binary = run / name
    command = [*link_flags, obj(source), obj(startup), "-Xlinker", "-force_load", "-Xlinker", gateway_archive,
               *production_vendors, *libs, "-o", binary]
    result = invoke(command, run / (name + ".link.log"))
    if result["status"] == 0:
        result["sha256"] = digest(binary)
    else:
        print("LINK_FAIL", name, "log:", result["log"], flush=True)
    executables[name] = result
provenance["executables"] = executables
print("executables:", sum(r["status"] == 0 for r in executables.values()), "/", len(executables), "linked", flush=True)

def finalize():
    provenance["identity_unchanged"] = identity() == source_identity
    provenance["inputs_unchanged"] = all((gateway / p).is_file() and digest(gateway / p) == h for p, h in inputs.items())
    provenance["archive_sha256"] = digest(gateway_archive)
    (run / "provenance.json").write_text(json.dumps(provenance, indent=2, default=str) + "\n")
    print("receipt:", run, flush=True)
    return provenance["inputs_unchanged"] and provenance["identity_unchanged"] and all(
        r["status"] == 0 for r in executables.values())

if args.suite == "none":
    print("tests intentionally not selected", flush=True)
    raise SystemExit(not finalize())

spec = importlib.util.spec_from_file_location("uos_links", gateway / "tools/links.py")
links = importlib.util.module_from_spec(spec)
spec.loader.exec_module(links)
registry = links.read_registry()
all_tests = [Path(t) for t in links.all_tests()]
tests = [t for t in all_tests if any(f in str(t) for f in args.filter.split(",")) and (args.suite == "all" or t.parent == Path("tests/rt"))]
if not tests:
    raise SystemExit("No tests selected")
sandbox = ('(version 1)(allow default)(deny network*)'
           '(allow network-inbound (local ip "localhost:*"))'
           '(allow network-outbound (remote ip "localhost:*"))'
           '(allow network-bind (local ip "localhost:*"))'
           '(allow network* (local unix-socket) (remote unix-socket))')
preflight = ('import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); s.listen(); '
             'c=socket.socket(); c.connect(s.getsockname()); a,_=s.accept(); a.close(); c.close(); s.close(); '
             't=socket.socket(); t.settimeout(2)\n'
             'try: t.connect(("192.0.2.1",443))\n'
             'except PermissionError: print("loopback accepted; external network denied"); raise SystemExit(0)\n'
             'raise SystemExit("network sandbox did not deny external connection")')
boundary = invoke(["/usr/bin/sandbox-exec", "-p", sandbox, sys.executable, "-c", preflight], run / "sandbox.log", 10)
if boundary["status"]:
    raise SystemExit("Darwin fixture sandbox unavailable: " + boundary["log"])

results = []
test_compile_failed = False
for phase, selected in [("runtime", [t for t in tests if t.parent == Path("tests/rt")]),
                        ("gateway", [t for t in tests if t.parent != Path("tests/rt")])]:
    resolutions = {t: links.resolve(str(t), registry) for t in selected}
    sources = sorted({t for t in selected} | {Path(s) for r in resolutions.values() for s in r.sources
                                             if s not in {str(p) for p in runtime + modules}})
    if phase == "gateway" and args.suite == "all" and not args.filter:
        # Include the standalone production-smoke helper's compile gate as well.
        sources = sorted(set(sources) | {p.relative_to(gateway) for p in (gateway / "tests").rglob("*.cpp")
                                        if p.relative_to(gateway) not in all_tests})
    compiled = compile_phase(phase, sources)
    failures = {r["source"]: r["log"] for r in compiled if r["status"]}
    test_compile_failed |= bool(failures)
    for test in selected:
        label = str(test.relative_to("tests").with_suffix("")).replace("/", "__")
        log = run / (label + ".log")
        binary = build / (label + ".bin")
        resolution = resolutions[test]
        source_list = list(dict.fromkeys([test, *map(Path, resolution.sources), *runtime]))
        if failed_logs := [failures[str(s)] for s in source_list if str(s) in failures]:
            row = dict(test=str(test), status="BUILD_FAIL", log=failed_logs[0])
        else:
            selected_archives = [vendor_archives[library.archive] for library in vendor_sources.required(vendors, source_list)]
            linked = invoke([*link_flags, *map(obj, source_list), *selected_archives, *libs, "-o", binary], log)
            if linked["status"]:
                row = dict(test=str(test), status="LINK_FAIL", **{k: v for k, v in linked.items() if k != "status"})
            else:
                with tempfile.TemporaryDirectory(prefix="um.", dir="/tmp") as scratch_name:
                    scratch = Path(scratch_name).resolve()
                    for name in ["h", "t", "w"]:
                        (scratch / name).mkdir()
                    test_env = dict(env, HOME=str(scratch / "h"), TMPDIR=str(scratch / "t"))
                    for setting in resolution.env:
                        name, value = setting.replace("${SCRATCH}", str(scratch)).split("=", 1)
                        test_env[name] = value
                    with log.open("a") as output:
                        # sandbox-exec is a platform binary: set dyld's search path after it,
                        # otherwise macOS removes DYLD_* before the fixture starts.
                        command = ["/usr/bin/sandbox-exec", "-p", sandbox, "/usr/bin/env",
                                   "DYLD_LIBRARY_PATH=" + env["DYLD_LIBRARY_PATH"], str(binary)] + ([args.case] if args.case else [])
                        output.write("\n" + shlex.join(command) + "\n")
                        output.flush()
                        start = time.monotonic()
                        child = subprocess.Popen(command, cwd=scratch / "w", env=test_env, stdout=output,
                                                 stderr=subprocess.STDOUT, start_new_session=True)
                        try:
                            status = child.wait(timeout=args.timeout)
                        except subprocess.TimeoutExpired:
                            status = 124
                        finally:
                            try:
                                os.killpg(child.pid, signal.SIGKILL)
                            except ProcessLookupError:
                                pass
                            child.wait()
                    summaries = re.findall(r"(\d+) run, (\d+) passed, (\d+) failed", log.read_text(errors="replace"))
                    counts = list(map(int, summaries[-1])) if summaries else [0, 0, 0]
                    row = dict(test=str(test), status="PASS" if status == 0 and counts[0] and counts[2] == 0 else
                               "TIMEOUT" if status == 124 else "FAIL", exit=status, cases=counts,
                               seconds=round(time.monotonic() - start, 3), log=str(log))
            binary.unlink(missing_ok=True)
        results.append(row)
        if row["status"] != "PASS" or len(results) % 25 == 0:
            print(row["status"], test, row.get("cases", []), "log:", row["log"], flush=True)
        (run / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    subset = [r for r in results if (Path(r["test"]).parent == Path("tests/rt")) == (phase == "runtime")]
    print(phase, "binaries:", len(subset), "passed:", sum(r["status"] == "PASS" for r in subset),
          "cases:", [sum(r.get("cases", [0, 0, 0])[i] for r in subset) for i in range(3)], flush=True)

verified = finalize()
raise SystemExit(not verified or test_compile_failed or any(r["status"] != "PASS" for r in results))
PYTHON
