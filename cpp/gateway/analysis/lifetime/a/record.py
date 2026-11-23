#!/usr/bin/env python3
"""Record one reviewed file and verify the Area A report against its site data."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import sys


HERE = Path(__file__).resolve().parent
GATEWAY = HERE.parents[2]
REPORT = GATEWAY / "docs/findings/cppgw-lifetime-a-20261118.md"
DATA = HERE / "sites.json"
VERDICTS = ("DEFECT", "SAFE", "UNCLEAR")
CLASSES = {
    "a": "Capturing coroutine closure",
    "b": "Borrowed coroutine parameter or receiver",
    "c": "Callback ownership or cycle",
    "d": "Container invalidation",
    "e": "GCC 12 await-expression rule",
}
SECTION = "## Site verdicts\n"
CHECKBOX = re.compile(r"^- \[[ x]\] `([^\x60]+)`$", re.M)
SCOPE = re.compile(r"/(?:async|abort|clock|process_loop|stream|fetch[^/]*|tls[^/]*|http|websocket[^/]*|gzip)\.[^/]+$")


def validate(data):
    tree = data["source_tree"]
    files = data["files"]
    names = subprocess.check_output(
        ["git", "ls-tree", "-r", "--name-only", tree, "src/rt", "include/uos/rt"],
        cwd=GATEWAY,
        text=True,
    ).splitlines()
    expected = {p.removeprefix("cpp/gateway/") for p in names if SCOPE.search(p)}
    expected.add("include/uos/rt/process.hpp")
    if set(files) != expected:
        raise ValueError(f"Inventory mismatch: {sorted(set(files) ^ expected)}")
    source_tree = subprocess.check_output(
        ["git", "rev-parse", data["source_commit"] + "^{tree}"], cwd=GATEWAY, text=True
    ).strip()
    if source_tree != tree:
        raise ValueError("Source tree does not match the starting commit")
    defect_ids = set()
    related_ids = set()
    for file, entry in files.items():
        status = entry["status"]
        if status not in {"complete", "pending"} or (status == "pending" and entry["sites"]):
            raise ValueError(f"Invalid completion state for {file}")
        if status == "complete" and not entry.get("note"):
            raise ValueError(f"Missing review note for {file}")
        path = GATEWAY / file
        source = subprocess.check_output(
            ["git", "show", f"{tree}:cpp/gateway/{file}"], cwd=GATEWAY
        )
        if path.read_bytes() != source:
            raise ValueError(f"Source changed since audit began: {file}")
        last_line = source.count(b"\n") + (not source.endswith(b"\n"))
        seen = set()
        for site in entry["sites"]:
            lines = site["lines"]
            if not lines or lines != sorted(set(lines)) or lines[0] < 1 or lines[-1] > last_line:
                raise ValueError(f"Invalid line anchors for {file}: {lines}")
            if site["label"] in seen or not all(site[key] for key in ("label", "reason")):
                raise ValueError(f"Missing or duplicate site text for {file}: {site}")
            seen.add(site["label"])
            classes = site["classes"]
            if not classes or classes != sorted(set(classes)) or any(key not in CLASSES for key in classes):
                raise ValueError(f"Invalid hazard classes for {file}: {classes}")
            if site["verdict"] not in VERDICTS:
                raise ValueError(f"Invalid verdict for {file}: {site['verdict']}")
            if site["verdict"] == "DEFECT":
                if "related_defect" in site:
                    related_ids.add(site["related_defect"])
                    continue
                defect = site["defect"]
                if defect["id"] in defect_ids or len(defect["interleaving"]) != 2 or not defect["diff"]:
                    raise ValueError(f"Missing or duplicate defect evidence for {file}: {defect['id']}")
                defect_ids.add(defect["id"])
    if related_ids - defect_ids:
        raise ValueError(f"Unknown related defects: {sorted(related_ids - defect_ids)}")


def render(text, data):
    prefix, _, _ = text.partition(SECTION)
    files = data["files"]
    if set(CHECKBOX.findall(prefix)) != set(files) or prefix.count("## Checklist") != 1:
        raise ValueError("Checklist does not match the audit inventory")
    prefix = CHECKBOX.sub(
        lambda m: f"- [{'x' if files[m[1]]['status'] == 'complete' else ' '}] `{m[1]}`",
        prefix,
    )
    result = [prefix.rstrip(), "", SECTION.strip(), ""]
    defects = []
    verdicts = Counter()
    kinds = {key: Counter() for key in CLASSES}
    done = 0
    for file, entry in files.items():
        if entry["status"] == "pending":
            continue
        done += 1
        result.extend([f"### `{file}`", "", entry["note"], ""])
        for site in entry["sites"]:
            anchors = ", ".join(map(str, site["lines"]))
            kind = "/".join(site["classes"])
            verdict = site["verdict"]
            verdicts[verdict] += 1
            for key in site["classes"]:
                kinds[key][verdict] += 1
            defect_id = ""
            if "related_defect" in site:
                defect_id = f" ({site['related_defect']})"
            elif verdict == "DEFECT":
                defect = site["defect"]
                defects.append((defect["priority"], defect["id"], site, file, anchors))
                defect_id = f" ({defect['id']})"
            result.append(
                f"- **{verdict}{defect_id}** `{anchors}` ({kind}), "
                f"{site['label']}. {site['reason']}"
            )
        if entry["sites"]:
            result.append("")

    result.extend([
        "## Ranked defects and counts",
        "",
        f"Reviewed **{done}/{len(files)} files** and **{sum(verdicts.values())} site groups**. "
        "A site group can have multiple line anchors and hazard classes under the same verdict.",
        "",
        "| Hazard class | DEFECT | SAFE | UNCLEAR |",
        "| --- | ---: | ---: | ---: |",
    ])
    for key, title in CLASSES.items():
        result.append(f"| {key}. {title} | " + " | ".join(str(kinds[key][v]) for v in VERDICTS) + " |")
    result.extend(["| **Unique site groups** | " + " | ".join(str(verdicts[v]) for v in VERDICTS) + " |", ""])
    for index, (_, ident, site, file, anchors) in enumerate(sorted(defects), start=1):
        defect = site["defect"]
        result.extend([
            f"### {index}. {ident} ({defect['severity']}) — {site['label']}",
            "",
            f"`{file}:{anchors}`",
            "",
            *(f"- {step}" for step in defect["interleaving"]),
            "",
            "```diff",
            defect["diff"].rstrip(),
            "```",
            "",
        ])
    return "\n".join(result).rstrip() + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    record = commands.add_parser("record")
    record.add_argument("path", help="Gateway-relative file documented by a JSON object on stdin")
    commands.add_parser("verify")
    args = parser.parse_args()
    data = json.loads(DATA.read_text(encoding="utf-8"))
    report = REPORT.read_text(encoding="utf-8")
    if args.command == "record":
        if data["files"][args.path]["status"] != "pending":
            parser.error(f"{args.path} is already complete")
        entry = json.load(sys.stdin)
        data["files"][args.path] = {"status": "complete", **entry}
    validate(data)
    updated = render(report, data)
    if args.command == "record":
        DATA.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        REPORT.write_text(updated, encoding="utf-8")
    elif report != updated:
        parser.error("report checklist, site verdicts or ranking differ from sites.json")
    else:
        files = data["files"].values()
        print(f"Verified {sum(e['status'] == 'complete' for e in files)}/{len(files)} files "
              f"and {sum(len(e['sites']) for e in files)} site groups")


if __name__ == "__main__":
    main()
