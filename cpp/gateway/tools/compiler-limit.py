#!/usr/bin/python3
"""CMake compiler/linker launcher using the existing two global gateway slots."""
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from build_support import GIB, admission, local_inputs, measured


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
        admission(kind, os.environ.get("UOS_GATEWAY_RESOURCE_LOG"), os.environ.get("UOS_GATEWAY_NOTES_SHA256"))
        output = Path(os.environ["UOS_GATEWAY_BUILD_EVIDENCE"])
        before_inputs = local_inputs(source) if source else {}
        print("uos-build-start kind=%s pid=%d source=%s" % (kind, os.getpid(), source or "link"), file=sys.stderr, flush=True)
        cache = os.environ.get("UOS_GATEWAY_CCACHE")
        command = [cache] + argv if cache and kind == "compile" and "-c" in argv and source else argv
        code, receipt = measured(command, Path.cwd(), dict(os.environ), output / "commands", kind,
                                 timeout=600 if kind == "compile" else 300,
                                 address_limit=(3 if kind == "link" and source is None else 2) * GIB, low_priority=True)
        # The receipt itself already binds exact command; bind consumed source bytes too.
        if source:
            import json
            from build_support import atomic_json
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
