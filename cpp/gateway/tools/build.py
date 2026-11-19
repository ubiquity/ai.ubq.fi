#!/usr/bin/python3
"""Offline qualified-SDK CMake/Ninja gateway build; never installs or launches it."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import re
import sys
import time

sys.dont_write_bytecode = True
from build_support import GATEWAY, SDK_MANIFEST_SHA256, WORKSPACE, admission, atomic_json, bind_instruction_snapshot, digest, local_inputs, measured, supervisor
import vendor_sources


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("preset", choices=("release", "sdk-release", "clang-release", "asan-ubsan-clang", "asan-ubsan-gcc", "tsan-threaded", "fuzz-clang", "clang-libcxx"))
    parser.add_argument("phase", choices=("configure", "build", "checkpoint", "trial", "test", "package"))
    parser.add_argument("--source", help="One cpp/gateway-relative source for an optimized compile trial")
    parser.add_argument("--target", action="append", default=[])
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=2)
    parser.add_argument("--tests", action="store_true", help="Enable the generated CTest extension once integrated")
    parser.add_argument("--focus-test", help="Build one unchanged existing Links closure for source-fix verification")
    parser.add_argument("--keep-going", action="store_true", help="Finish independent compile targets after a strict diagnostic")
    parser.add_argument("--ccache", choices=("on", "off"), default="on", help="Native task-local cache; qualification records cold and warm controls")
    parser.add_argument("--frame-recycling", choices=("auto", "enabled", "disabled"), default="auto", help="Explicit profile qualification; auto uses the recorded preset policy")
    parser.add_argument("--lane", help="Isolated incremental graph/view with the same qualified profile and shared content cache")
    parser.add_argument("--incremental", action="store_true", help="Build named targets without claiming the full-release lock")
    parser.add_argument("--release-lock", type=Path, default=WORKSPACE / ".data/cpp-gateway-release.lock")
    parser.add_argument("--supervisor-log", type=Path)
    return parser.parse_args()


def put_executable(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_text(text)
        temporary.chmod(0o755)
        temporary.replace(path)


def sdk_identity(sdk):
    if digest(sdk / "manifest.json") != SDK_MANIFEST_SHA256:
        raise RuntimeError("Qualified SDK manifest identity mismatch")
    required = ["root/usr/bin/aarch64-linux-gnu-g++-12", "root/usr/bin/aarch64-linux-gnu-gcc-12",
                "root/usr/bin/cmake", "root/usr/bin/ninja", "root/usr/bin/ctest",
                "root/lib/ld-linux-aarch64.so.1", "host-tools/bin/ld", "host-tools/bin/ar", "host-tools/bin/ranlib",
                "root/usr/lib/aarch64-linux-gnu/libboost_json.a", "root/usr/lib/aarch64-linux-gnu/libboost_container.a",
                "root/usr/lib/aarch64-linux-gnu/libboost_system.a", "root/usr/lib/aarch64-linux-gnu/libssl.so",
                "root/usr/lib/aarch64-linux-gnu/libcrypto.so"]
    result = {"manifest_sha256": SDK_MANIFEST_SHA256, "path": str(sdk), "artifacts": {}}
    for relative in required:
        path = sdk / relative
        if not path.is_file():
            raise RuntimeError("Missing offline SDK artifact: " + relative)
        result["artifacts"][relative] = digest(path)
    return result


def tool_view(sdk, view):
    loader = sdk / "root/lib/ld-linux-aarch64.so.1"
    libraries = str(sdk / "root/lib/aarch64-linux-gnu") + ":" + str(sdk / "root/usr/lib/aarch64-linux-gnu")
    (view / "share").mkdir(parents=True, exist_ok=True)
    resource = view / "share/cmake-3.25"
    expected = sdk / "root/usr/share/cmake-3.25"
    if resource.is_symlink() and resource.resolve() != expected.resolve():
        raise RuntimeError("CMake executable view belongs to a different SDK")
    if not resource.exists():
        resource.symlink_to(expected)
    for tool in ("cmake", "ctest", "cpack", "ninja"):
        command = shlex.join([str(loader), "--library-path", libraries])
        text = '#!/bin/sh\nset -eu\nexec ' + command + ' --argv0 "$0" ' + shlex.quote(str(sdk / "root/usr/bin" / tool)) + ' "$@"\n'
        put_executable(view / "bin" / tool, text)
    for tool in ("ar", "ranlib"):
        command = ["/usr/bin/python3", str(GATEWAY / "tools/compiler-limit.py"), "--archive", str(sdk / "host-tools/bin" / tool)]
        put_executable(view / "bin" / ("uos-" + tool), '#!/bin/sh\nset -eu\nexec ' + shlex.join(command) + ' "$@"\n')
    # CMake's compiler identification does not use COMPILER_LAUNCHER. Make the
    # compiler executable itself bounded, including identification/try_compile.
    for wrapper, driver in (("uos-cc", "gcc"), ("uos-cxx", "g++")):
        compiler = ["/usr/bin/python3", str(GATEWAY / "tools/compiler-limit.py"),
                    str(sdk / "root/usr/bin" / ("aarch64-linux-gnu-" + driver + "-12")),
                    "--sysroot=" + str(sdk / "root"), "-B" + str(sdk / "host-tools/bin") + "/"]
        put_executable(view / "bin" / wrapper, '#!/bin/sh\nset -eu\nexec ' + shlex.join(compiler) + ' "$@"\n')
    return [str(loader), "--library-path", libraries]


def input_identity(targets):
    vendors = tuple(vendor_sources.selected())
    vendor_targets = {library.target for library in vendors}
    paths = [GATEWAY / "PORT-MANIFEST.json", GATEWAY / "PORTED.txt", GATEWAY / "CMakeLists.txt", GATEWAY / "CMakePresets.json"]
    sources = [path for path in (GATEWAY / "src").rglob("*.cpp") if not path.name.startswith(".")]
    if not targets or "uos_gateway" in targets:
        sources += list((GATEWAY / "runtime").glob("*.cpp"))
        sources += [GATEWAY / "entrypoints/gateway.cpp"]
    if targets and set(targets) <= vendor_targets:
        sources = []
    elif targets and all(target.startswith("uos_obj_") for target in targets):
        sources += list((GATEWAY / "entrypoints").glob("*.cpp")) + list((GATEWAY / "runtime").glob("*.cpp"))
        for directory in ("tests", "bench", "fuzz"):
            sources += list((GATEWAY / directory).rglob("*.cpp"))
        selected = {"uos_obj_" + hashlib.sha256(path.relative_to(GATEWAY).as_posix().encode()).hexdigest()[:16]: path for path in sources}
        sources = [selected[target] for target in targets]
    elif targets and all(target.startswith("uos_focus_") for target in targets):
        from cmake_focus import resolve_target
        sources = [GATEWAY / source for target in targets for source in resolve_target(target)["sources"]]
        paths += [GATEWAY / "tests/LINKS.txt", GATEWAY / "tools/links.py"]
    elif targets and all(target.startswith("uos_test_") for target in targets):
        registry = Path(os.environ["UOS_GATEWAY_ACTIVE_BUILD"]) / "tests-registry.json"
        tests = {item["target"]: item for item in json.loads(registry.read_text())["tests"]}
        sources = [GATEWAY / source for target in targets for source in tests[target]["sources"]]
        paths += [registry, GATEWAY / "tests/LINKS.txt", GATEWAY / "tools/links.py", GATEWAY / "tools/generate-tests.py", GATEWAY / "tools/ctest-run.py"]
        for target in targets:
            paths += [GATEWAY / name for name in tests[target].get("inputs", [])]
    elif targets and any(target not in ("uos_gateway", "uos_gateway_modules") for target in targets):
        registry = GATEWAY / "cmake/extra-targets.json"
        extra = {item["target"]: item for item in json.loads(registry.read_text())["targets"]}
        if any(target not in extra for target in targets):
            raise RuntimeError("Target has no consumed-input contract yet: " + ", ".join(targets))
        sources = [GATEWAY / source for target in targets for source in extra[target]["sources"]]
        if any(extra[target].get("archive") for target in targets):
            sources += [path for path in (GATEWAY / "src").rglob("*.cpp") if not path.name.startswith(".")]
        paths += [registry]
    used = vendor_sources.required(vendors, [path.relative_to(GATEWAY).as_posix() for path in sources], targets)
    for library in used:
        paths.extend(GATEWAY / relative for relative in library.inputs)
    paths.extend((GATEWAY / "cmake").glob("*.cmake"))
    paths.extend((GATEWAY / "cmake").glob("*.json"))
    paths.extend(GATEWAY / "tools" / name for name in ("build.py", "build_support.py", "build_profiles.py", "compiler-limit.py", "cmake-sources.py", "cmake_focus.py", "vendor_sources.py"))
    result = {str(path.relative_to(GATEWAY)) if GATEWAY in path.parents else str(path): digest(path) for path in sorted(set(paths))}
    for path in sources:
        result.update(local_inputs(path))
    return result


def main():
    options = arguments()
    inherited_snapshot = os.environ.get("UOS_GATEWAY_INSTRUCTION_SNAPSHOT")
    inherited_notes = os.environ.get("UOS_GATEWAY_NOTES_SHA256")
    inherited_phase = os.environ.get("UOS_GATEWAY_RUN_ID")
    if options.lane and not re.fullmatch(r"[a-z][a-z0-9-]{0,23}", options.lane):
        raise RuntimeError("Lane must be a bounded lowercase identifier")
    if options.focus_test:
        from cmake_focus import resolve
        focus = resolve(options.focus_test)
        options.target = [focus["target"]]
    if options.incremental and not options.target:
        raise RuntimeError("An incremental invocation must name bounded targets")
    if options.phase == "trial" and not options.source:
        raise RuntimeError("trial requires --source")
    # Admission precedes SDK reads or process creation. No SDK download/repair exists.
    before = admission("driver", options.supervisor_log)
    vendors = tuple(vendor_sources.selected())
    native = str(options.sdk) == "native"
    if native:
        from build_profiles import native as native_profile
        profile = native_profile(options.preset, options.ccache == "on", options.lane)
        identity = profile["identity"]
        build, view, temporary, evidence = (profile[key] for key in ("build", "view", "temporary", "evidence"))
        runtime_prefix = []
    else:
        if options.lane:
            raise RuntimeError("SDK comparison does not admit native isolated lanes")
        if options.preset == "release":
            print("SDK release is an explicit transition alias for sdk-release; compiler remains SDK12.2", file=sys.stderr)
            options.preset = "sdk-release"
        if options.preset != "sdk-release":
            raise RuntimeError("The SDK selector requires sdk-release; native profiles require the explicit native selector")
        sdk = options.sdk.resolve()
        identity = sdk_identity(sdk)
        build = WORKSPACE / ".data/cpp-gateway-release"
        view = WORKSPACE / ".data/cpp-gateway-tool-view"
        temporary = WORKSPACE / ".data/cpp-gateway-tmp/sdk-release"
        evidence = WORKSPACE / ".data/cpp-gateway-evidence/sdk-release"
        for directory in (build, temporary / "h", temporary / "t", evidence):
            directory.mkdir(parents=True, exist_ok=True)
        runtime_prefix = tool_view(sdk, view)
    stamp = str(time.time_ns())
    env = dict(profile["env"]) if native else {"PATH": "/usr/bin:/bin", "HOME": str(temporary / "h"), "TMPDIR": str(temporary / "t"), "TZ": "UTC",
           "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8", "UOS_CPP_TOOLCHAIN_DIR": str(sdk), "UOS_GATEWAY_TOOL_VIEW": str(view),
           "UOS_GATEWAY_BUILD_EVIDENCE": str(evidence), "UOS_GATEWAY_RUN_ID": stamp,
           "PYTHONPYCACHEPREFIX": str(WORKSPACE / ".data/cpp-gateway-pycache")}
    env["UOS_GATEWAY_RUN_ID"] = stamp
    env["UOS_GATEWAY_ACTIVE_BUILD"] = str(build)
    os.environ["UOS_GATEWAY_ACTIVE_BUILD"] = str(build)
    if options.supervisor_log:
        accepted = supervisor(options.supervisor_log)
        env["UOS_GATEWAY_RESOURCE_LOG"] = str(options.supervisor_log.resolve())
        env["UOS_GATEWAY_NOTES_SHA256"] = accepted["notes_sha256"]
    atomic_json(build / "profile-environment.json", {"environment": env, "source_root": str(GATEWAY),
                "profile": options.preset, "compiler_wrapper": str(view / "bin/uos-cxx"), "qualification": "stage checks; final flags live in compile_commands.json"})
    cmake = [str(view / "bin/cmake")]
    configure = cmake + ["--preset", options.preset, "-B", str(build), "-DUOS_GATEWAY_TESTS=" + ("ON" if options.tests else "OFF"),
                         "-DUOS_GATEWAY_FOCUS_TEST=" + (options.focus_test or "")]
    if native and options.frame_recycling != "auto":
        configure += ["-DUOS_GATEWAY_FRAME_RECYCLING_DISABLED=" + ("ON" if options.frame_recycling == "disabled" else "OFF")]
    selected = list(options.target)
    if options.phase == "trial":
        source = Path(options.source)
        if source.is_absolute() or ".." in source.parts or not (GATEWAY / source).is_file():
            raise RuntimeError("Trial source must exist under cpp/gateway")
        selected = ["uos_obj_" + hashlib.sha256(source.as_posix().encode()).hexdigest()[:16]]
    build_command = cmake + ["--build", "--preset", options.preset, "--parallel", str(options.jobs)]
    if selected:
        build_command += ["--target"] + selected
    if options.keep_going:
        build_command += ["--", "-k0"]
    phases = [("configure", configure), ("build", build_command)] if options.phase in ("checkpoint", "trial") else \
             [("configure", configure)] if options.phase == "configure" else \
             [("test", [str(view / "bin/ctest"), "--preset", options.preset])] if options.phase == "test" else \
             [("package", cmake + ["--build", "--preset", options.preset, "--target", "package"])] if options.phase == "package" else [("build", build_command)]
    full_lock = None
    if options.phase in ("build", "checkpoint") and not options.incremental:
        options.release_lock.parent.mkdir(parents=True, exist_ok=True)
        full_lock = options.release_lock.open("a+")
        fcntl.flock(full_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        full_lock.seek(0)
        full_lock.truncate()
        full_lock.write(json.dumps({"owner": "gateway-build", "pid": os.getpid(), "started": time.time(), "run_id": stamp}) + "\n")
        full_lock.flush()
    # Ninja owns one graph/cache per profile. Global compiler locks alone cannot
    # prevent two independent Ninja processes from writing the same object.
    profile_lock = (build / ".invocation.lock").open("a+")
    fcntl.flock(profile_lock, fcntl.LOCK_EX)
    inputs = input_identity(selected)
    atomic_json(evidence / ("inputs-" + stamp + ".json"), {"sdk": identity, "inputs": inputs, "before": before, "runtime_prefix": runtime_prefix})
    try:
        for phase, command in phases:
            instruction = None
            if options.supervisor_log:
                if inherited_snapshot and inherited_notes:
                    # A serial suite owns one bounded reviewed phase. Descendant
                    # target builds preserve it instead of auto-binding new text.
                    instruction = supervisor(options.supervisor_log, inherited_notes)
                    instruction.update(snapshot=inherited_snapshot, inherited_phase=inherited_phase)
                else:
                    instruction = bind_instruction_snapshot(options.supervisor_log, evidence, stamp + "-" + phase)
                env["UOS_GATEWAY_NOTES_SHA256"] = instruction["notes_sha256"]
                env["UOS_GATEWAY_INSTRUCTION_SNAPSHOT"] = instruction["snapshot"]
                os.environ["UOS_GATEWAY_INSTRUCTION_SNAPSHOT"] = instruction["snapshot"]
                os.environ["UOS_GATEWAY_BUILD_EVIDENCE"] = str(evidence)
                os.environ["UOS_GATEWAY_RUN_ID"] = stamp
            admission("driver", options.supervisor_log, env.get("UOS_GATEWAY_NOTES_SHA256"))
            log = evidence / (phase + "-" + stamp + ".log")
            print(json.dumps({"phase": phase, "command": command, "log": str(log), "run_id": stamp}), flush=True)
            with log.open("w") as output:
                code, receipt = measured(command, GATEWAY, env, evidence, phase, timeout=7200 if phase == "build" else 600,
                                         output=output, footprint_paths=(build, temporary, view, evidence))
            if instruction:
                phase_record = json.loads(receipt.read_text())
                phase_record["reviewed_instructions"] = instruction
                atomic_json(receipt, phase_record)
            print(json.dumps({"phase": phase, "code": code, "receipt": str(receipt), "log": str(log)}), flush=True)
            if code:
                print("\n".join(log.read_text().splitlines()[-25:]), file=sys.stderr)
                return code
        after = input_identity(selected)
        changed = sorted(path for path in inputs.keys() | after.keys() if inputs.get(path) != after.get(path))
        artifact_paths = [build / name for name in ("libuos_gateway.a", "uos_gateway")]
        artifact_paths += [build / target for target in selected]
        artifact_paths += [build / library.archive for library in vendors if all(source in after for source in library.sources)]
        artifacts = {path.name: {"sha256": digest(path), "bytes": path.stat().st_size}
                     for path in artifact_paths if path.is_file()}
        atomic_json(evidence / ("result-" + stamp + ".json"), {"run_id": stamp, "phase": options.phase, "artifacts": artifacts,
                    "changed_inputs": changed, "object_count": len(list(build.glob("CMakeFiles/uos_obj_*.dir/src/**/*.o"))),
                    "selected_targets": selected, "consumed_inputs_sha256": after,
                    "profile_owner_sha256": digest(build / "profile-owner.json") if (build / "profile-owner.json").is_file() else None,
                    "compile_database_sha256": digest(build / "compile_commands.json") if (build / "compile_commands.json").is_file() else None,
                    "after": before if not phases else admission("driver", options.supervisor_log)})
        if changed:
            raise RuntimeError("Inputs changed during build; affected receipts need revalidation: " + ", ".join(changed))
        return 0
    finally:
        profile_lock.close()
        if full_lock:
            full_lock.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print("cpp/gateway build: " + str(error), file=sys.stderr)
        raise SystemExit(3)
