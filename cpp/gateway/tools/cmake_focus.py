#!/usr/bin/python3
"""Resolve one unchanged Links closure for a focused optimized source-fix gate."""
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
import links


def target_name(test):
    return "uos_focus_" + hashlib.sha256(test.encode()).hexdigest()[:16]


def resolve(test):
    test = links.normalize_test(test)
    selected = links.resolve(test, links.read_registry())
    if selected.skip:
        raise RuntimeError("Focused test is explicitly skipped: " + test)
    runtime = sorted(path.relative_to(links.GATEWAY).as_posix() for path in (Path(links.GATEWAY) / "src/rt").glob("*.cpp") if not path.name.startswith("."))
    return {"test": test, "target": target_name(test), "sources": [test] + selected.sources + runtime, "environment": selected.env}


def resolve_target(target):
    for test in links.all_tests():
        if target_name(test) == target:
            return resolve(test)
    raise RuntimeError("Unknown focused-test target: " + target)


def main():
    selected = resolve(sys.argv[1])
    lines = ["# Generated from the unchanged Links closure.", "set(focused_objects)"]
    for source in selected["sources"]:
        lines += ['uos_object_for_source(object "' + source + '")', 'list(APPEND focused_objects "$<TARGET_OBJECTS:${object}>")']
    lines += ["add_executable(" + selected["target"] + " EXCLUDE_FROM_ALL ${focused_objects})",
              "target_link_libraries(" + selected["target"] + " PRIVATE uos_gateway_settings)",
              "uos_link_vendors(" + selected["target"] + " " + " ".join('"' + source + '"' for source in selected["sources"]) + ")", ""]
    Path(sys.argv[2]).write_text("\n".join(lines))
    print(json.dumps(selected))


if __name__ == "__main__":
    main()
