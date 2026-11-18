#!/usr/bin/env python3
"""Find lifetime candidates and commit individually reviewed area D files."""

import argparse
import collections
import json
from pathlib import Path
import re
import subprocess
import sys

GATEWAY = Path(__file__).resolve().parents[3]
REPOSITORY = GATEWAY.parents[1]
LEDGER = Path(__file__).with_name("sites.json")
REPORT = GATEWAY / "docs/findings/cppgw-lifetime-d-20261118.md"
AREAS = frozenset("handler admin auth authority sentinel models catalog lib jev_compaction kv_conversion systemone decisions".split())
END = "<!-- SITE-LEDGER-END -->"
COUNTS = "<!-- AUDIT-COUNTS -->"
VERDICTS = ("SAFE", "DEFECT", "UNCLEAR")
KINDS = {
    "a": "coroutine closure",
    "b": "borrowed argument or receiver",
    "c": "callback ownership",
    "d": "container or RAII lifetime",
    "e": "await expression",
    "s": "suspension or task ownership",
}
# Remove comments and literals without shifting source coordinates.
NON_CODE = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|R"([^ ()\\\t\n]{0,16})\([\s\S]*?\)\1"|"(?:[^"\\\n]|\\[\s\S])*"|\'(?:[^\'\\\n]|\\[\s\S])*\'')
AWAIT = re.compile(r"\bco_await\b")
LOOK = re.compile(
    r"\b(?:co_await|co_yield|Task|Promise|awaitable|spawn|allSettled|race|any|all|co_spawn|async|setTimeout|setInterval|"
    r"queueMicrotask|onAbort|onDelivery|onFinish|onEvent|addEventListener|register|subscribe|dispatch|"
    r"keepAlive|makeReadableStream|ReadableStream|SourceAlgorithm|forAwait|withFinally|scopeExit|"
    r"string_view|span|this|weak_from_this|shared_from_this)\b"
    r"|(?:\.|->)(?:on[A-Z]\w*|then|catch_|finally_|listen|post|setCallback|attach|async_\w+)\s*\("
    r"|\[[^]\n]*\]\s*(?:\(|\{|->)"
)


def git(*args):
    return subprocess.check_output(["git", "-C", str(REPOSITORY), *args], text=True).strip()


def file_list(commit):
    prefix = GATEWAY.relative_to(REPOSITORY).as_posix() + "/"
    names = git("ls-tree", "-r", "--name-only", commit, "--", prefix + "src", prefix + "include/uos").splitlines()
    found = []
    for name in names:
        path = name.removeprefix(prefix)
        pieces = path.split("/")
        subpath = pieces[1:] if pieces[0] == "src" else pieces[2:]
        if len(subpath) == 1 or subpath[0] in AREAS:
            found.append(path)
    return sorted(found, key=lambda path: (path.startswith("include/"), path))


def code_mask(source):
    return NON_CODE.sub(lambda match: re.sub(r"[^\n]", " ", match[0]), source)


def input_lines(path):
    source = (GATEWAY / path).read_text()
    return list(zip(source.splitlines(), code_mask(source).splitlines()))


def matches(path, rule):
    return [number for number, (_, code) in enumerate(input_lines(path), 1) if rule.search(code)]


def statistic(data):
    files = data["files"]
    reviewed = sum(entry["reviewed"] for entry in files.values())
    sites = [site for entry in files.values() for site in entry["sites"]]
    counts = collections.Counter(site["verdict"] for site in sites)
    return f"{reviewed}/{len(files)} files reviewed. {len(sites)} sites: " + ", ".join(f"{counts[v]} {v}" for v in VERDICTS) + "."


def render(path, entry):
    result = [f"### `{path}`", "", entry["note"], ""]
    for site in entry["sites"]:
        locations = ", ".join(str(line) for line in site["lines"])
        result.append(f"- L{locations} **{site['verdict']}** · {site['detail']}")
    if entry["sites"]:
        result.append("")
    return "\n".join(result) + "\n"


def check_entry(path, entry):
    lines = input_lines(path)
    covered = set()
    for site in entry["sites"]:
        anchors = site["lines"]
        if anchors != sorted(set(anchors)) or not anchors or anchors[0] < 1 or anchors[-1] > len(lines):
            raise ValueError(f"{path}: invalid lines {anchors}")
        if not site["kinds"] or any(kind not in KINDS for kind in site["kinds"]):
            raise ValueError(f"{path}: unknown kind in {site}")
        if site["verdict"] not in VERDICTS or not site["detail"]:
            raise ValueError(f"{path}: invalid verdict {site}")
        covered.update(anchors)
    omitted = set(matches(path, AWAIT)) - covered
    if omitted:
        raise ValueError(f"{path}: unclassified co_await lines {sorted(omitted)}")


def parse_entry(path, note, rows):
    entry = {"reviewed": True, "note": note, "sites": []}
    await_lines = matches(path, AWAIT)
    for row in rows:
        location, kinds, verdict, detail = row.split("|", 3)
        anchors = set()
        for span in location.split(","):
            if span.startswith("@"):
                start, _, stop = span[1:].partition("-")
                anchors.update(line for line in await_lines if int(start) <= line <= int(stop or start))
            else:
                anchors.add(int(span))
        entry["sites"].append({"lines": sorted(anchors), "kinds": list(kinds), "verdict": verdict, "detail": detail})
    check_entry(path, entry)
    return entry


def record(data):
    notes = []
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        if line.startswith("FILE|"):
            _, path, note = line.split("|", 2)
            notes.append((path, note, []))
        else:
            notes[-1][2].append(line)
    # Validate the whole input before changing the first file.
    ready = {}
    for path, note, rows in notes:
        if path in ready or data["files"][path]["reviewed"]:
            raise ValueError(f"{path}: already reviewed")
        ready[path] = parse_entry(path, note, rows)
    for path, entry in ready.items():
        report = REPORT.read_text()
        todo = f"- [ ] `{path}`"
        if report.count(todo) != 1 or report.count(END) != 1 or report.count(COUNTS) != 1:
            raise ValueError(f"{path}: incorrect checklist or section marker")
        data["files"][path] = entry
        report = report.replace(todo, f"- [x] `{path}`")
        report = report.replace(END, render(path, entry) + END)
        report = re.sub(r"^.*" + re.escape(COUNTS) + r"$", statistic(data) + " " + COUNTS, report, flags=re.M)
        REPORT.write_text(report)
        LEDGER.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
        git("add", str(LEDGER), str(REPORT))
        git("diff", "--cached", "--check")
        print(git("-c", "user.name=Codex", "-c", "user.email=codex@openai.com", "commit", "-m", f"audit(cpp): review {path} lifetimes").splitlines()[0])


def verify(data, complete):
    paths = file_list(data["source_commit"])
    if paths != list(data["files"]) or git("rev-parse", data["source_commit"] + "^{tree}") != data["source_tree"]:
        raise ValueError("inventory differs from the audited tree")
    report = REPORT.read_text()
    for path, entry in data["files"].items():
        item = f"- [{'x' if entry['reviewed'] else ' '}] `{path}`"
        if report.count(item) != 1:
            raise ValueError(f"{path}: Markdown checklist discrepancy")
        source = str((GATEWAY / path).relative_to(REPOSITORY))
        commit_blob = git("rev-parse", data["source_commit"] + ":" + source)
        if git("hash-object", source) != commit_blob:
            raise ValueError(f"{path}: source differs from the audited commit")
        if entry["reviewed"]:
            check_entry(path, entry)
            if render(path, entry) not in report:
                raise ValueError(f"{path}: Markdown verdict discrepancy")
        elif complete:
            raise ValueError(f"{path}: unreviewed")
    if statistic(data) + " " + COUNTS not in report:
        raise ValueError("Markdown count discrepancy")
    print(statistic(data))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("pending", "scan", "record", "verify"))
    parser.add_argument("paths", nargs="*")
    parser.add_argument("--complete", action="store_true")
    args = parser.parse_args()
    data = json.loads(LEDGER.read_text())
    if args.command == "record":
        record(data)
    elif args.command == "verify":
        verify(data, args.complete)
    else:
        for path, entry in data["files"].items():
            if args.paths and not any(path == selection or path.startswith(selection + "/") for selection in args.paths):
                continue
            if args.command == "pending":
                if not entry["reviewed"]:
                    print(path)
            else:
                result = [(number, original.strip()) for number, (original, code) in enumerate(input_lines(path), 1) if LOOK.search(code)]
                print(f"\n{path} ({len(result)} candidate lines)")
                for number, original in result:
                    print(f"{number:5} {original}")


if __name__ == "__main__":
    main()
