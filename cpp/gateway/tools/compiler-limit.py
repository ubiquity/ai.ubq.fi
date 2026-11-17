#!/usr/bin/python3
"""CMake compiler/linker launcher using the existing two global gateway slots."""
import os
from pathlib import Path
import sys
import shlex

sys.dont_write_bytecode = True
from build_support import GIB, admission, local_inputs, measured


def measured_small_native_link(argv, kind, source):
    """v18/v19: measured no-fork Mold inputs; unknown/SDK links stay3/6."""
    if kind != "link" or source is not None or os.environ.get("UOS_GATEWAY_BUILD_PROFILE") != "release":
        return None
    args = []
    for arg in argv:
        if arg.startswith("@"):
            path = Path(arg[1:])
            if not path.is_file() or path.stat().st_size > 1024 ** 2:
                return None
            args.extend(shlex.split(path.read_text()))
        else:
            args.append(arg)
    if "-fuse-ld=mold" not in args or "-Wl,--no-fork" not in args or any("sanitize" in x or "flto" in x or x.startswith("@") for x in args):
        return None
    owner_path = Path(os.environ.get("UOS_GATEWAY_ACTIVE_BUILD", ".")) / "profile-owner.json"
    if not owner_path.is_file():
        return None
    import json
    from build_support import digest, WORKSPACE
    owner = json.loads(owner_path.read_text())
    if owner.get("profile") != "release" or owner.get("sanitizer") is not None or Path(argv[0]).resolve() != Path(owner["compiler"]).resolve() or digest(argv[0]) != owner["compiler_sha256"]:
        return None
    pinned = owner["artifacts_sha256"]
    archives = {"/usr/lib/libboost_json.a", "/usr/lib/libboost_container.a"}
    inputs = []
    objects = 0
    native_archive = False
    skip = False
    fixed_options = {"-O2", "-g0", "-fuse-ld=mold", "-Wl,--threads=2", "-Wl,--no-fork", "-pthread",
                     "-Wl,--push-state,--whole-archive", "-Wl,--pop-state"}
    fixed_libraries = {"ssl", "crypto", "sqlite3", "z", "brotlidec", "zstd", "icuuc", "icui18n", "pthread", "dl", "m"}
    for arg in args[1:]:
        if skip:
            skip = False
            continue
        if arg == "-o":
            skip = True
            continue
        if arg.startswith("-"):
            if arg not in fixed_options and not arg.startswith(("-Wl,--dependency-file,", "-Wl,--dependency-file=")) and \
                    not (arg.startswith("-l") and arg[2:] in fixed_libraries):
                return None
            continue
        path = Path(arg)
        if not path.is_file():
            return None
        if arg.endswith(".o"):
            if WORKSPACE / ".data" not in path.resolve().parents:
                return None
            objects += 1
        elif arg.endswith(".a"):
            if path.resolve() == (owner_path.parent / "libuos_gateway.a").resolve() and not path.is_symlink():
                native_archive = True
            elif str(path) not in archives or pinned.get(str(path)) != digest(path):
                return None
        else:
            return None
        inputs.append({"path": str(path.resolve()), "bytes": path.stat().st_size})
    total = sum(row["bytes"] for row in inputs)
    if not 1 <= objects <= 384 or total > 256 * 1024 ** 2:
        return None
    small = objects <= 64 and total <= 64 * 1024 ** 2 and not native_archive
    return {"policy": "BUILD-RESOURCES-v18-MOLD" if small else "BUILD-RESOURCES-v19-MOLD-LARGE", "objects": objects, "input_bytes": total,
            "inputs": inputs, "address_limit": GIB if small else 2 * GIB,
            "admission_bytes": 3 * GIB if small else 17 * GIB // 4}


def main():
    argv = sys.argv[1:]
    kind = "compile" if any(arg in argv for arg in ("-c", "-E", "-S", "-fsyntax-only")) else "link"
    if argv and argv[0] == "--archive":
        kind = "archive"
        argv = argv[1:]
    if not argv:
        raise RuntimeError("Missing qualified compiler/tool command")
    source = next((arg for arg in argv if arg.endswith((".cpp", ".cxx", ".cc", ".c", ".C", ".c++"))), None)
    if source is None and any(arg == "--version" or arg.startswith(("-dump", "-print")) for arg in argv):
        kind = "query"
    from build_support import acquire_slots
    locks = acquire_slots(kind)
    try:
        small_link = measured_small_native_link(argv, kind, source)
        admitted = admission(kind, os.environ.get("UOS_GATEWAY_RESOURCE_LOG"), os.environ.get("UOS_GATEWAY_NOTES_SHA256"),
                             memory_minimum=small_link["admission_bytes"] if small_link else None)
        output = Path(os.environ["UOS_GATEWAY_BUILD_EVIDENCE"])
        before_inputs = local_inputs(source) if source else {}
        print("uos-build-start kind=%s pid=%d source=%s" % (kind, os.getpid(), source or "link"), file=sys.stderr, flush=True)
        cache = os.environ.get("UOS_GATEWAY_CCACHE")
        command = [cache] + argv if cache and kind == "compile" and "-c" in argv and source else argv
        code, receipt = measured(command, Path.cwd(), dict(os.environ), output / "commands", kind,
                                 timeout=600 if kind == "compile" else 300,
                                 address_limit=small_link["address_limit"] if small_link else (3 if kind == "link" and source is None else 2) * GIB,
                                 low_priority=True, sample_interval_s=.02 if small_link else 1,
                                 sample_processes=bool(small_link))
        import json
        from build_support import atomic_json
        record = json.loads(receipt.read_text())
        record["admission"] = admitted
        record["measured_small_native_link"] = small_link
        atomic_json(receipt, record)
        # The receipt itself already binds exact command; bind consumed source bytes too.
        if source:
            record = json.loads(receipt.read_text())
            after_inputs = local_inputs(source)
            record["inputs_sha256"] = before_inputs
            changed = sorted(path for path in before_inputs.keys() | after_inputs.keys() if before_inputs.get(path) != after_inputs.get(path))
            record["changed_inputs"] = changed
            atomic_json(receipt, record)
            if changed:
                raise RuntimeError("Compiler consumed changing inputs; receipt=" + str(receipt))
        print("uos-build-end kind=%s code=%d receipt=%s" % (kind, code, receipt), file=sys.stderr, flush=True)
        return code
    finally:
        for lock in locks:
            lock.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print("cpp/gateway build: " + str(error), file=sys.stderr)
        raise SystemExit(3)
