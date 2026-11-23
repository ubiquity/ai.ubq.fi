#!/usr/bin/env python3
"""Portable Ninja object build and isolated, bounded standalone test runner."""
import argparse
import concurrent.futures
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True
GATEWAY = Path(__file__).resolve().parents[2]
ROOT = GATEWAY.parents[1]
SOURCE_PREFIX = GATEWAY.relative_to(ROOT)
FIXTURE_CHECKOUT = Path("/tmp/source")
sys.path.insert(0, str(GATEWAY / "tools"))
import compiler_cache
import links
import vendor_sources

CONFIGS = {
    "debug": ["-O0"],
    # Standard coroutine frame allocation avoids GCC 15's mismatched-new-delete
    # diagnostic in Boost.Asio's inlined recycling allocator. Warnings stay fatal.
    "release": ["-O2", "-DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING"],
    "asan": ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING"],
}
SUMMARY = re.compile(r"^(\d+) run, (\d+) passed, (\d+) failed$", re.M)
PRINT_LOCK = threading.Lock()
CHILD_LOCK = threading.RLock()
CHILDREN = set()
NINJA = None


def announce(message):
    with PRINT_LOCK:
        print(message, flush=True)


def stop_group(pid):
    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    except PermissionError:
        # The sudo namespace supervisor retains uid 0 until its child settles.
        # Signal only the process group created by this exact Popen call.
        stopped = subprocess.run(["sudo", "-n", "kill", "-KILL", "--", str(-pid)],
                                 env={"PATH": os.defpath, "LC_ALL": "C"}, capture_output=True, text=True)
        # The last root supervisor can exit between the unprivileged signal and
        # sudo's signal. Only ESRCH is harmless; an actual signal failure is fatal.
        if stopped.returncode and "No such process" not in stopped.stderr:
            raise RuntimeError("could not terminate owned process group %d: %s" % (pid, stopped.stderr.strip()))


def positive(value):
    number = int(value)
    if number < 1:
        raise ValueError("must be a positive integer")
    return number


def clean_environment():
    # An allowlist also excludes unknown providers and future credential names.
    allowed = ("PATH", "LANG", "LC_ALL", "TZ", "SYSTEMROOT", "DYLD_LIBRARY_PATH", "LD_LIBRARY_PATH")
    return {name: os.environ[name] for name in allowed if name in os.environ}


def boundary_command(prefix, scratch, build, binary, environment):
    if sys.platform != "linux":
        raise RuntimeError("OS fixture network boundary is currently qualified only on Linux")
    # A user namespace maps our host user to root. The sudo fallback uses the
    # real host uid/gid and the helper drops privileges after namespace setup.
    mapped = "--map-root-user" in prefix
    return prefix + [sys.executable, str(Path(__file__).with_name("isolate.py")), str(scratch),
                     str(build / "bin"), binary, str(ROOT), FIXTURE_CHECKOUT.name, os.readlink("/proc/self/ns/net"),
                     str(0 if mapped else os.getuid()), str(0 if mapped else os.getgid()),
                     json.dumps(environment)]


def find_boundary(build, environment):
    if sys.platform != "linux":
        raise RuntimeError("OS fixture network boundary is currently qualified only on Linux")
    unshare = shutil.which("unshare")
    if not unshare:
        raise RuntimeError("unshare is required for the Linux fixture network boundary")
    common = ["--net", "--mount", "--pid", "--fork", "--mount-proc", "--propagation", "private", "--kill-child=SIGKILL", "--"]
    candidates = [[unshare, "--user", "--map-root-user"] + common]
    if os.geteuid() == 0:
        candidates.append([unshare] + common)
    elif shutil.which("sudo"):
        candidates.append([shutil.which("sudo"), "-n", unshare] + common)
    for index, prefix in enumerate(candidates):
        with tempfile.TemporaryDirectory(prefix="uos-ci-", dir="/tmp") as directory:
            scratch = Path(directory)
            for name in ("h", "t", "w"):
                (scratch / name).mkdir()
            command = boundary_command(prefix, scratch, build, ":probe:", environment)
            log = build / ("boundary-probe-%d.log" % index)
            code, expired, _ = execute(command, log, cwd=build, environment=environment, timeout=15)
            if code == 0 and not expired and "UOS_CI_NETWORK " in log.read_text():
                announce("Fixture boundary: fresh Linux network/mount/pid namespaces" + (" via sudo; test uid=%d" % os.getuid() if "-n" in prefix else ""))
                return prefix
    raise RuntimeError("OS fixture network boundary unavailable; see boundary-probe logs (no unrestricted fallback)")


def execute(command, log, *, cwd, environment, timeout):
    """One owned process group; timeouts/cancellation also terminate descendants."""
    started = time.monotonic()
    with log.open("w") as output:
        output.write("command: " + shlex.join(map(str, command)) + "\n")
        output.flush()
        child = subprocess.Popen(command, cwd=cwd, env=environment, stdin=subprocess.DEVNULL,
                                 stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        with CHILD_LOCK:
            CHILDREN.add(child.pid)
        timed_out = False
        try:
            try:
                code = child.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                stop_group(child.pid)
                code = child.wait()
        finally:
            stop_group(child.pid)
            with CHILD_LOCK:
                CHILDREN.discard(child.pid)
    return code, timed_out, round(time.monotonic() - started, 3)


def interrupted(number, _frame):
    with CHILD_LOCK:
        for pid in CHILDREN:
            if NINJA is not None and pid == NINJA.pid:
                # Ninja forwards termination to the separate process groups it
                # creates for compiler commands; killing only Ninja would leak them.
                os.kill(pid, signal.SIGTERM)
                try:
                    NINJA.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    stop_group(pid)
            else:
                stop_group(pid)
    raise SystemExit(128 + number)


def ninja_escape(value):
    return str(value).replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def object_path(build, source):
    return build / "obj" / (source + ".o")


def cpp_inventory(directory, *, recursive=False):
    root = GATEWAY / directory
    paths = root.rglob("*.cpp") if recursive else root.glob("*.cpp")
    return sorted(path.relative_to(GATEWAY).as_posix() for path in paths
                  if not any(part.startswith(".") for part in path.relative_to(GATEWAY).parts))


def ninja_compile_rule(name, argv, description):
    command = shlex.join(argv).replace("$", "$$")
    return ["rule " + name, "  command = rm -f $out && " + command +
            " -MMD -MF $out.d -MT $out -c $in -o $out > $out.log 2>&1 || { cat $out.log; exit 1; }",
            "  depfile = $out.d", "  deps = gcc", "  description = " + description + " $in"]


def write_ninja(build, sources, compiler, flags, c_compiler, c_flags, vendors):
    # Ninja's depfiles track transitive headers; a failed rebuild removes stale output.
    # Run from the checkout so relative inputs (including cached depfiles) resolve
    # identically at any checkout depth. Keep all Ninja state in the output tree.
    lines = ["ninja_required_version = 1.10", "builddir = " + ninja_escape(build),
             *ninja_compile_rule("cxx", compiler + flags, "CXX")]
    json_sources = vendor_sources.simdjson_consumers(vendors)
    vendor_headers = {}
    for library in vendors:
        headers = [ninja_escape(SOURCE_PREFIX / path) for path in library.headers]
        for source in (*library.consumers, *library.sources):
            vendor_headers.setdefault(source, []).extend(headers)
    if json_sources:
        lines += ninja_compile_rule("selected_json", compiler + flags + vendor_sources.simdjson_flags(SOURCE_PREFIX), "CXX")
    includes = {library.name: vendor_sources.include_flags(library, SOURCE_PREFIX) for library in vendors}
    vendor_c_options = {library.name: includes[library.name] + vendor_sources.c_dialect_flags(library.sources[0])
                        for library in vendors}
    include_consumers = {source: library for library in vendors if includes[library.name] for source in library.consumers}
    for library in vendors:
        if includes[library.name]:
            lines += ninja_compile_rule("cxx_" + library.name, compiler + flags + includes[library.name], "CXX")
        if vendor_c_options[library.name]:
            lines += ninja_compile_rule("vendor_c_" + library.name, c_compiler + c_flags + vendor_c_options[library.name], "Vendor C")
    if vendors:
        archive = shutil.which("ar")
        if not archive:
            raise RuntimeError("ar is required for the selected vendors")
        lines += ninja_compile_rule("vendor_cxx", compiler + vendor_sources.vendor_flags(flags), "Vendor CXX")
        lines += ninja_compile_rule("vendor_c", c_compiler + c_flags, "Vendor C")
        lines += ["rule vendor_archive", "  command = rm -f $out && " + shlex.quote(archive).replace("$", "$$") + " rcs $out $in"]
    for source in sources:
        target = object_path(build, source)
        target.parent.mkdir(parents=True, exist_ok=True)
        rule = "selected_json" if source in json_sources else "cxx"
        if library := include_consumers.get(source):
            rule = "cxx_" + library.name
        # -MMD omits system headers, including QCBOR's nested includes.
        headers = vendor_headers.get(source)
        dependency = " | " + " ".join(headers) if headers else ""
        lines.append("build " + ninja_escape(target) + ": " + rule + " " + ninja_escape(SOURCE_PREFIX / source) + dependency)
    for library in vendors:
        members = []
        for source in library.sources:
            target = object_path(build, source)
            target.parent.mkdir(parents=True, exist_ok=True)
            rule = "vendor_c" if source.endswith(".c") else "vendor_cxx"
            if source.endswith(".c") and vendor_c_options[library.name]:
                rule += "_" + library.name
            members.append(ninja_escape(target))
            headers = " | " + " ".join(vendor_headers[source])
            lines.append("build " + members[-1] + ": " + rule + " " + ninja_escape(SOURCE_PREFIX / source) + headers)
        lines.append("build " + ninja_escape(build / library.archive) + ": vendor_archive " + " ".join(members))
    (build / "build.ninja").write_text("\n".join(lines) + "\n")


def result(test, status, note="", *, built=False, **extra):
    return dict(test=test, status=status, built=built, run=0, passed=0, failed=0, note=note, **extra)


def input_digest(vendors=()):
    digest = hashlib.sha256()
    paths = [GATEWAY / "tools" / name for name in ("ci-build.sh", "links.py")]
    paths.extend([GATEWAY / "tools/compiler_cache.py", GATEWAY / "tools/vendor_sources.py", GATEWAY / vendor_sources.REGISTRY])
    paths.extend(GATEWAY / relative for library in vendors for relative in library.inputs)
    for directory in ("src", "include", "tests", "cmake/ci"):
        paths.extend(path for path in (GATEWAY / directory).rglob("*") if path.is_file())
    for path in sorted(paths):
        digest.update(path.relative_to(GATEWAY).as_posix().encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()


def run_test(selected, runtime, build, compiler, flags, libraries, environment, timeout, boundary, vendors=()):
    test = selected.test
    identity = test.removeprefix("tests/").removesuffix(".cpp").replace("/", "__")
    log_dir = build / "logs" / identity
    log_dir.mkdir(parents=True, exist_ok=True)
    sources = list(dict.fromkeys([test] + selected.sources + runtime))
    missing = [source for source in sources if not object_path(build, source).is_file()]
    if missing:
        return result(test, "BUILD_FAIL", "compile failed: " + ", ".join(missing), log=str(log_dir))
    binary = build / "bin" / identity
    archives = [str(build / library.archive) for library in vendor_sources.required(vendors, sources)]
    command = compiler + flags + [str(object_path(build, source)) for source in sources] + archives + libraries + ["-o", str(binary)]
    code, expired, link_s = execute(command, log_dir / "build.log", cwd=build,
                                    environment=environment, timeout=900)
    if code:
        binary.unlink(missing_ok=True)
        return result(test, "TIMEOUT" if expired else "BUILD_FAIL", "link failed; see build.log",
                      build_seconds=link_s, log=str(log_dir))
    try:
        # Short paths keep AF_UNIX socket names within the OS limit. Tests can change
        # cwd without ever opening the checkout's default production-state paths.
        with tempfile.TemporaryDirectory(prefix="uos-ci-", dir="/tmp") as scratch:
            scratch = Path(scratch)
            for directory in ("h", "t", "w"):
                (scratch / directory).mkdir()
            isolated = dict(environment, HOME=str(scratch / "h"), TMPDIR=str(scratch / "t"))
            for assignment in selected.env:
                name, value = assignment.split("=", 1)
                if name in ("HOME", "TMPDIR", "PATH") or re.search(r"(?:KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|AUTH|PROVIDER)", name, re.I):
                    raise RuntimeError("credential/provider environment entry refused for " + test)
                isolated[name] = value.replace("${SCRATCH}", "/tmp")
            if "-fsanitize=address,undefined" in flags:
                isolated["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
                isolated["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            command = boundary_command(boundary, scratch, build, binary.name, isolated)
            code, expired, run_s = execute(command, log_dir / "run.log", cwd=scratch / "w",
                                           environment=isolated, timeout=timeout)
        output = (log_dir / "run.log").read_text(errors="replace")
        summaries = SUMMARY.findall(output)
        counts = tuple(map(int, summaries[-1])) if summaries else (0, 0, 0)
        ran, passed, failed = counts
        complete = ran > 0 and ran == passed + failed
        controlled = output.count("UOS_CI_NETWORK ") == 1
        status = "TIMEOUT" if expired else "PASS" if code == 0 and controlled and complete and failed == 0 else "FAIL"
        row = result(test, status, "" if status == "PASS" else "exit %d; see run.log" % code,
                     built=True, build_seconds=link_s, seconds=run_s, log=str(log_dir))
        row.update(run=ran, passed=passed, failed=failed)
        return row
    except Exception as error:
        return result(test, "FAIL", str(error), built=True, build_seconds=link_s, log=str(log_dir))
    finally:
        # At most jobs binaries exist at once, including on small CI disks.
        binary.unlink(missing_ok=True)


def verify_sqlite(build, launcher, compiler, flags, path_flags, libraries, build_env, environment, boundary):
    source = Path(__file__).with_name("sqlite-version.cpp")
    binary = build / "bin" / "sqlite-version"
    obj = build / "sqlite-version.o"
    try:
        log = build / "sqlite-version-compile.log"
        command = launcher + compiler + flags + path_flags + ["-c", str(source.relative_to(ROOT)), "-o", str(obj)]
        code, _, _ = execute(command, log, cwd=ROOT, environment=build_env, timeout=60)
        if code:
            raise RuntimeError("SQLite version probe failed to compile; see " + str(log))
        log = build / "sqlite-version-build.log"
        command = compiler + flags + [str(obj)] + libraries + ["-o", str(binary)]
        code, _, _ = execute(command, log, cwd=build, environment=environment, timeout=60)
        if code:
            raise RuntimeError("SQLite version probe failed to build; see " + str(log))
        log = build / "sqlite-version-run.log"
        with tempfile.TemporaryDirectory(prefix="uos-ci-", dir="/tmp") as directory:
            scratch = Path(directory)
            command = boundary_command(boundary, scratch, build, binary.name, environment)
            code, _, _ = execute(command, log, cwd=build, environment=environment, timeout=15)
        for line in log.read_text().splitlines():
            if line.startswith("sqlite3_libversion(): "):
                announce(line)
                if code == 0:
                    return line.removeprefix("sqlite3_libversion(): ")
        raise RuntimeError("SQLite version probe failed; see " + str(log))
    finally:
        binary.unlink(missing_ok=True)
        obj.unlink(missing_ok=True)


def main():
    global NINJA
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("filter", nargs="?", default="", help="substring of the test path")
    parser.add_argument("--config", choices=CONFIGS, default="debug")
    parser.add_argument("--shard", metavar="i/n", help="one-based round-robin split of sorted filtered tests")
    parser.add_argument("--list", action="store_true", help="list selected paths without compiling")
    options = parser.parse_args()
    tests = [test for test in links.all_tests() if options.filter in test]
    if options.shard:
        try:
            index, total = map(positive, options.shard.split("/"))
            if index > total:
                raise ValueError("shard index exceeds total")
        except ValueError as error:
            parser.error("invalid --shard: " + str(error))
        tests = tests[index - 1::total]
    if not tests:
        parser.error("no tests selected")
    if options.list:
        print("\n".join(tests))
        return 0
    default_jobs = subprocess.check_output(["nproc"], text=True).strip() if shutil.which("nproc") else str(os.cpu_count() or 1)
    try:
        jobs = positive(os.environ.get("UOS_CI_JOBS", default_jobs))
        timeout = positive(os.environ.get("UOS_CI_TIMEOUT", "300"))
    except ValueError as error:
        parser.error("invalid UOS_CI_JOBS/UOS_CI_TIMEOUT: " + str(error))
    try:
        launcher = compiler_cache.launcher()
    except ValueError as error:
        parser.error(str(error))
    compiler = shlex.split(os.environ.get("CXX", "g++"))
    if not compiler or not shutil.which(compiler[0]):
        parser.error("CXX compiler not found")
    compiler[0] = shutil.which(compiler[0])
    version = subprocess.check_output(compiler + ["--version"], text=True)
    c_compiler = shlex.split(os.environ.get("CC", "cc"))
    if not c_compiler or not shutil.which(c_compiler[0]):
        parser.error("CC compiler not found")
    c_compiler[0] = shutil.which(c_compiler[0])
    c_version = subprocess.check_output(c_compiler + ["--version"], text=True)
    clang = "clang" in version.lower()
    if not clang:
        major = int(subprocess.check_output(compiler + ["-dumpversion"], text=True).split(".")[0])
        if major < 14:
            parser.error("GCC >=14 required: GCC 12 has a known coroutine miscompile")
    if not shutil.which("ninja"):
        parser.error("Ninja is required")
    sqlite_prefix = Path(os.environ.get("UOS_CI_SQLITE_PREFIX", "/usr/local")).resolve()
    sqlite_include = sqlite_prefix / "include"
    sqlite_library = sqlite_prefix / "lib" / "libsqlite3.so"
    if not (sqlite_include / "sqlite3.h").is_file() or not sqlite_library.is_file():
        parser.error("prepare SQLite 3.54.0 at UOS_CI_SQLITE_PREFIX; see cpp/gateway/docs/CI.md")
    packages = ["openssl", "zlib", "icu-uc", "icu-i18n", "libbrotlidec", "libzstd"]
    package_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs"] + packages, text=True))
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror",
             "-DBOOST_JSON_NO_LIB", "-DBOOST_CONTAINER_NO_LIB", "-DBOOST_SYSTEM_NO_LIB",
             "-DUOS_GATEWAY_NATIVE_LIBRARIES=1", "-I" + str(SOURCE_PREFIX / "include"), "-I" + str(SOURCE_PREFIX), "-I" + str(sqlite_include)]
    if clang:
        # Some consumers include vendor headers relative to the gateway root.
        flags.append("--system-header-prefix=third_party/")
    extra_cxx_flags = shlex.split(os.environ.get("UOS_CI_CXXFLAGS", ""))
    extra_c_flags = vendor_sources.vendor_flags(shlex.split(os.environ.get("UOS_CI_CFLAGS", "")))
    flags += CONFIGS[options.config] + [flag for flag in package_flags if flag.startswith(("-I", "-D"))]
    flags += extra_cxx_flags
    c_flags = ["-std=c11"] + [flag for flag in CONFIGS[options.config] if not flag.startswith("-D")]
    c_flags += extra_c_flags
    # Some fixtures use __FILE__ to read repository assets. Give those paths a
    # stable absolute name that the fixture namespace binds to this checkout.
    path_flags = [f"-fdebug-prefix-map={ROOT}=.",
                  f"-fmacro-prefix-map={ROOT}={FIXTURE_CHECKOUT}",
                  f"-fmacro-prefix-map={SOURCE_PREFIX}={FIXTURE_CHECKOUT / SOURCE_PREFIX}"]
    libraries = ["-lboost_json", "-lboost_container", str(sqlite_library)]
    libraries += [flag for flag in package_flags if not flag.startswith(("-I", "-D"))] + ["-pthread"]
    if sys.platform.startswith("linux"):
        # Use DT_RPATH so a distribution search path cannot override the pinned suite library.
        libraries += ["-ldl", "-Wl,--disable-new-dtags", "-Xlinker", "-rpath", "-Xlinker", str(sqlite_library.parent)]
        if shutil.which("mold"):
            libraries += ["-fuse-ld=mold", "-Wl,--threads=1"]
    libraries += shlex.split(os.environ.get("UOS_CI_LDFLAGS", ""))
    compiler_hash, c_compiler_hash = (hashlib.sha256(Path(command[0]).read_bytes()).hexdigest()
                                      for command in (compiler, c_compiler))
    identity = hashlib.sha256(json.dumps([str(ROOT), compiler, compiler_hash, version, flags, c_compiler,
                                         c_compiler_hash, c_version, c_flags, path_flags, libraries]).encode()).hexdigest()[:12]
    build_root = Path(os.environ.get("UOS_CI_BUILD_DIR", "/tmp/uos-gateway-build")).resolve()
    if build_root.is_relative_to(GATEWAY.parents[1]):
        parser.error("UOS_CI_BUILD_DIR must be outside the repository checkout")
    build = build_root / (Path(compiler[0]).name + "-" + identity) / options.config
    build.mkdir(parents=True, exist_ok=True)
    owner = (build / "run.lock").open("a")
    try:
        fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        parser.error("another driver owns " + str(build))
    started = time.monotonic()
    run_id = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + str(os.getpid())
    # Preserve old results/logs for failed runs rather than overwriting evidence.
    if (build / "logs").exists():
        archive = build / "history" / run_id
        archive.mkdir(parents=True)
        (build / "logs").rename(archive / "logs")
        for name in ("results.tsv", "receipt.json", "compile.log", "ccache.log"):
            if (build / name).exists():
                (build / name).rename(archive / name)
    (build / "bin").mkdir(exist_ok=True)
    runtime = cpp_inventory("src/rt")
    production = cpp_inventory("src", recursive=True)
    registry = links.read_registry()
    selected, rows = [], []
    sources = set(production)
    for test in tests:
        try:
            resolved = links.resolve(test, registry)
            for source in [test] + resolved.sources:
                if Path(source).is_absolute() or ".." in Path(source).parts or not source.endswith(".cpp"):
                    raise links.LinkError("nonlocal/non-C++ source: " + source)
            selected.append(resolved)
            sources.update([test] + resolved.sources)
        except links.LinkError as error:
            rows.append(result(test, "BUILD_FAIL", str(error)))
    vendors = vendor_sources.required(tuple(vendor_sources.selected()), sources)
    source_hash = input_digest(vendors)
    git_identity = subprocess.check_output(["git", "-C", str(GATEWAY), "rev-parse", "HEAD", "HEAD^{tree}"], text=True).splitlines()
    announce("Compiler: " + version.splitlines()[0])
    announce("C compiler: " + c_version.splitlines()[0])
    announce("Compiler launcher: " + (shlex.join(launcher) if launcher else "none"))
    announce("Build: %s; %d production sources (%d runtime), %d tests, jobs=%d" % (build, len(production), len(runtime), len(tests), jobs))
    write_ninja(build, sorted(sources), launcher + compiler, flags + path_flags,
                launcher + c_compiler, c_flags + path_flags, vendors)
    environment = clean_environment()
    build_env = dict(environment, **{name: os.environ[name] for name in ("HOME", "TMPDIR", "XDG_CACHE_HOME") if name in os.environ})
    cache_log = build / "ccache.log"
    if launcher:
        build_env = compiler_cache.environment(build_env, ROOT, options.config)
        # Ccache intentionally omits path-map options from its hash. Explicit
        # arguments can change their destinations, unlike our fixed path maps.
        extra_arguments = [compiler[1:], c_compiler[1:], extra_cxx_flags, extra_c_flags]
        if any(extra_arguments):
            build_env["CCACHE_NAMESPACE"] += "-" + hashlib.sha256(json.dumps(extra_arguments).encode()).hexdigest()
    if compiler_cache.is_ccache(launcher):
        cache_log.unlink(missing_ok=True)
        build_env.update(CCACHE_STATS="1", CCACHE_STATSLOG=str(cache_log))
    boundary = find_boundary(build, environment)
    sqlite_version = verify_sqlite(build, launcher, compiler, flags, path_flags, libraries, build_env, environment, boundary)
    build_env["NINJA_STATUS"] = "[%f/%t %es] "
    compile_started = time.monotonic()
    with (build / "compile.log").open("w") as log:
        process = subprocess.Popen(["ninja", "-f", str(build / "build.ninja"), "-k", "0", "-j", str(jobs)], cwd=ROOT,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                   env=build_env, start_new_session=True)
        NINJA = process
        with CHILD_LOCK:
            CHILDREN.add(process.pid)
        for line in process.stdout:
            log.write(line)
            log.flush()
            announce(line.rstrip())
        compile_code = process.wait()
        with CHILD_LOCK:
            CHILDREN.discard(process.pid)
        NINJA = None
    compile_s = round(time.monotonic() - compile_started, 3)
    missing_production = [source for source in production if not object_path(build, source).is_file()]
    announce("Object phase: %.3fs, %d/%d production objects, exit %d" % (compile_s, len(production) - len(missing_production), len(production), compile_code))
    test_started = time.monotonic()
    (build / "logs").mkdir(exist_ok=True)
    with (build / "results.tsv").open("w") as incremental:
        incremental.write("test\tstatus\trun\tpassed\tfailed\tbuild_seconds\tseconds\tnote\n")
        for row in rows:
            incremental.write("\t".join(str(row.get(key, "")) for key in ("test", "status", "run", "passed", "failed", "build_seconds", "seconds", "note")) + "\n")
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            pending = {pool.submit(run_test, item, runtime, build, compiler, flags, libraries, environment, timeout, boundary, vendors): item.test for item in selected}
            for future in concurrent.futures.as_completed(pending):
                try:
                    row = future.result()
                except Exception as error:
                    row = result(pending[future], "FAIL", str(error))
                rows.append(row)
                incremental.write("\t".join(str(row.get(key, "")) for key in ("test", "status", "run", "passed", "failed", "build_seconds", "seconds", "note")) + "\n")
                incremental.flush()
                announce("%s %s (%d/%d cases)" % (row["status"], row["test"], row["passed"], row["run"]))
    rows.sort(key=lambda row: row["test"])
    statuses = {name: sum(row["status"] == name for row in rows) for name in ("PASS", "FAIL", "BUILD_FAIL", "TIMEOUT")}
    after_hash = input_digest(vendors)
    vendor_archives = {library.archive: hashlib.sha256((build / library.archive).read_bytes()).hexdigest()
                       for library in vendors if (build / library.archive).is_file()}
    cache_stats = compiler_cache.stats(cache_log) if compiler_cache.is_ccache(launcher) else None
    receipt = dict(config=options.config, compiler=compiler, compiler_version=version, flags=flags, sqlite_version=sqlite_version,
                   c_compiler=c_compiler, c_compiler_version=c_version, c_flags=c_flags,
                   compiler_sha256=compiler_hash, c_compiler_sha256=c_compiler_hash, compile_path_flags=path_flags,
                   compiler_launcher=launcher, ccache=cache_stats,
                   libraries=libraries, vendor_archives=vendor_archives, source_sha256=source_hash, after_source_sha256=after_hash,
                   inputs_unchanged=source_hash == after_hash, git_commit=git_identity[0], git_tree=git_identity[1], selected=tests,
                   production_sources=len(production), runtime_sources=len(runtime),
                   production_compile_failures=missing_production, compile_exit=compile_code,
                   compile_seconds=compile_s, test_wall_seconds=round(time.monotonic() - test_started, 3),
                   elapsed_seconds=round(time.monotonic() - started, 3), jobs=jobs,
                   statuses=statuses, cases={key: sum(row[key] for row in rows) for key in ("run", "passed", "failed")}, results=rows)
    (build / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    announce("\nTEST\tSTATUS\tRUN\tPASSED\tFAILED")
    for row in rows:
        announce("%s\t%s\t%d\t%d\t%d" % tuple(row[key] for key in ("test", "status", "run", "passed", "failed")))
    if cache_stats is not None:
        announce("ccache --show-stats delta: %(hits)d hits, %(misses)d misses, %(uncacheable)d uncacheable" % cache_stats)
    announce("TOTAL " + json.dumps({"tests": statuses, "cases": receipt["cases"], "seconds": receipt["elapsed_seconds"]}))
    announce("Evidence: " + str(build / "receipt.json"))
    if source_hash != after_hash:
        announce("INPUTS CHANGED: result counts are preserved, but this run is not acceptance evidence")
    return int(compile_code != 0 or source_hash != after_hash or any(row["status"] != "PASS" for row in rows))


if __name__ == "__main__":
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, interrupted)
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, links.LinkError) as error:
        print("ci-build: " + str(error), file=sys.stderr)
        raise SystemExit(2)
