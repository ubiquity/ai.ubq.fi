#!/usr/bin/env python3
"""Regenerate cpp/gateway/PORT-MANIFEST.json: every first-party runtime TypeScript
module, its line count, direct imports, the bun tests that import it directly, and
whether it is recorded as ported in cpp/gateway/PORTED.txt
(one TS path per line, maintained by the orchestrator once a chunk reports tests passing). Run from the repository root."""
import json, os, re, subprocess
from pathlib import Path

ALIAS = {"#http": "src/runtime/http.ts", "#kv": "src/storage/kv.ts"}
PORTED = {line.strip() for line in open("cpp/gateway/PORTED.txt", encoding="utf-8") if line.strip()} if os.path.exists("cpp/gateway/PORTED.txt") else set()
IMPORT = re.compile(r'''(?:from|import)\s*\(?\s*["']([^"']+)["']''')
ROOT = Path(__file__).resolve().parents[1]
CUSTOM_MODULE_KEYS = {
    "cpp/gateway/src/admin/gemini_quota.cpp",
    "cpp/gateway/src/provider/gemini.cpp",
}


def tracked(*roots):
    out = subprocess.check_output(["git", "ls-files", *roots], text=True)
    return [line for line in out.split("\n") if line.endswith(".ts")]


def resolve(origin, spec, known):
    if spec in ALIAS:
        return ALIAS[spec]
    if not spec.startswith("."):
        return None
    path = os.path.normpath(os.path.join(os.path.dirname(origin), spec))
    for candidate in (path, path + ".ts"):
        if candidate in known:
            return candidate
    return None


def imports(path, known):
    text = open(path, encoding="utf-8").read()
    found = {resolve(path, match.group(1), known) for match in IMPORT.finditer(text)}
    found.discard(None)
    found.discard(path)
    return text.count("\n"), sorted(found)


def cpp_paths(ts):
    stem = ts[:-3]
    stem = stem[4:] if stem.startswith("src/") else stem
    stem = stem.replace("-", "_")
    return f"cpp/gateway/include/uos/{stem}.hpp", f"cpp/gateway/src/{stem}.cpp"


def main():
    existing = json.loads((ROOT / "PORT-MANIFEST.json").read_text())
    sources = [f for f in tracked("src", "serve.ts", "lib") if not f.startswith("lib/codex/")]
    known = set(sources)
    modules = {}
    for source in sources:
        lines, deps = imports(source, known)
        header, impl = cpp_paths(source)
        modules[source] = {
            "loc": lines, "deps": deps, "tests": [], "header": header, "impl": impl,
            "ported": source in PORTED,
        }
    for test in tracked("tests"):
        _, deps = imports(test, known)
        for dep in deps:
            modules[dep]["tests"].append(test)
    missing_custom = CUSTOM_MODULE_KEYS - set(existing["modules"])
    if missing_custom:
        raise RuntimeError("Existing C++ adapter modules are missing: " + ", ".join(sorted(missing_custom)))
    for key in CUSTOM_MODULE_KEYS:
        modules[key] = existing["modules"][key]
    total = sum(m["loc"] for m in modules.values())
    done = sum(m["loc"] for m in modules.values() if m["ported"])
    summary = {"modules": len(modules), "loc": total, "ported_modules": sum(m["ported"] for m in modules.values()), "ported_loc": done}
    with open("cpp/gateway/PORT-MANIFEST.json", "w", encoding="utf-8") as handle:
        json.dump({"summary": summary, "modules": modules}, handle, indent=1, sort_keys=True)
        handle.write("\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
