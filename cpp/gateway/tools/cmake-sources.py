#!/usr/bin/python3
"""Generate the CMake source inventory from the authoritative port manifest."""
import hashlib
import json
from pathlib import Path
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    manifest = json.loads((root / "PORT-MANIFEST.json").read_text())
    modules = manifest["modules"]
    if len(modules) != 325 or manifest["summary"]["ported_modules"] != len(modules):
        raise RuntimeError("Port manifest is incomplete; expected all 325 ported modules")
    implementations = []
    for key, module in sorted(modules.items()):
        if not module["ported"]:
            raise RuntimeError("Unported module: " + key)
        relative = Path(module["impl"]).relative_to("cpp/gateway").as_posix()
        if not (root / relative).is_file():
            raise RuntimeError("Missing implementation: " + relative)
        implementations.append(relative)
    runtime = sorted(path.relative_to(root).as_posix() for path in (root / "src/rt").glob("*.cpp") if not path.name.startswith("."))
    all_sources = implementations + runtime
    actual = {path.relative_to(root).as_posix() for path in (root / "src").rglob("*.cpp") if not path.name.startswith(".")}
    if len(set(all_sources)) != len(all_sources) or set(all_sources) != actual:
        raise RuntimeError("Manifest/runtime inventory does not exactly cover production .cpp sources")
    lines = ["# Generated from PORT-MANIFEST.json; do not hand-maintain source paths.",
             "set(UOS_MODULE_COUNT %d)" % len(implementations), "set(UOS_RT_COUNT %d)" % len(runtime),
             "set(UOS_ALL_SOURCES"]
    lines += ['  "' + source + '"' for source in all_sources]
    lines += [")", "set(UOS_RT_SOURCES"] + ['  "' + source + '"' for source in runtime] + [")", ""]
    Path(sys.argv[1]).write_text("\n".join(lines))
    print(json.dumps({"module_count": len(implementations), "rt_count": len(runtime),
                      "source_count": len(all_sources), "manifest_sha256": hashlib.sha256((root / "PORT-MANIFEST.json").read_bytes()).hexdigest()}))


if __name__ == "__main__":
    main()
