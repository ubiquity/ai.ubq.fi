"""Record manually reviewed files and verify the Area C audit ledger."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import sys


GATEWAY = Path(__file__).resolve().parents[3]
REPO = GATEWAY.parents[1]
REPORT = GATEWAY / "docs/findings/cppgw-lifetime-c-20261118.md"
LEDGER = Path(__file__).with_name("sites.json")
MODULES = ("provider", "codex", "deepseek", "paid_fallback", "harmony", "chat", "embeddings", "live")
VERDICTS = ("DEFECT", "SAFE", "UNCLEAR")


def git(*args):
    return subprocess.check_output(["git", "-C", str(REPO), *args], text=True)


def site_row(site):
    return f"- {site['lines']} · {site['kind']} · **{site['verdict']}** · {site['reason']}"


def file_section(path, entry):
    lines = [f"### `{path}`", "", entry["note"]]
    if entry["sites"]:
        lines.extend(["", *(site_row(site) for site in entry["sites"])])
    return "\n".join(lines) + "\n"


def candidates():
    for module in MODULES:
        for directory in (f"include/uos/{module}", f"src/{module}"):
            for file in (GATEWAY / directory).rglob("*"):
                if file.is_file():
                    yield str(file.relative_to(GATEWAY))


def parse_records():
    entries = {}
    current = None
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        if line.startswith("@ "):
            path, note = line[2:].split("|", 1)
            if path in entries or not note:
                raise ValueError(f"Duplicate or undocumented file {path}")
            current = {"status": "reviewed", "note": note, "sites": []}
            entries[path] = current
            continue
        if current is None:
            raise ValueError("Each file starts with @ path|summary")
        lines, kind, verdict, reason = line.split("|", 3)
        current["sites"].append(dict(lines=lines, kind=kind, verdict=verdict, reason=reason))
    return entries


def verify_entry(path, entry):
    count = len((GATEWAY / path).read_text().splitlines())
    for site in entry["sites"]:
        lines, kind, verdict, reason = (site[key] for key in ("lines", "kind", "verdict", "reason"))
        if verdict not in VERDICTS or not kind or not reason:
            raise ValueError(f"Invalid verdict at {path}:{lines}")
        for position in lines.split(","):
            if not re.fullmatch(r"[0-9]+(?:-[0-9]+)?", position):
                raise ValueError(f"Invalid line anchor {path}:{lines}")
            start, _, end = position.partition("-")
            if not 1 <= int(start) <= int(end or start) <= count:
                raise ValueError(f"Out-of-range line anchor {path}:{position}")


def verify(ledger, report, require_complete):
    files = ledger["files"]
    checklist = dict((path, mark) for mark, path in re.findall(r"^- \[([ x])\] `([^`]+)`$", report, re.M))
    if set(files) != set(candidates()) or set(checklist) != set(files):
        raise ValueError("Source inventory, report checklist, and JSON file list differ")
    if git("rev-parse", ledger["audited_commit"] + "^{tree}").strip() != ledger["audited_tree"]:
        raise ValueError("Source commit and tree differ")
    scope = [f"cpp/gateway/{path}" for path in files]
    subprocess.run(["git", "-C", str(REPO), "diff", "--quiet", ledger["audited_commit"], "--", *scope], check=True)
    counts = Counter()
    reviewed = 0
    for path, entry in files.items():
        checked = checklist[path] == "x"
        if checked != (entry["status"] == "reviewed"):
            raise ValueError(f"Checklist status differs for {path}")
        if checked:
            verify_entry(path, entry)
            if file_section(path, entry) not in report:
                raise ValueError(f"Report and JSON site verdicts differ for {path}")
            counts.update(site["verdict"] for site in entry["sites"])
            reviewed += 1
        elif entry != {"status": "pending", "sites": []}:
            raise ValueError(f"Pending entry has verdicts in {path}")
    print(f"Files {reviewed}/{len(files)}. " + ", ".join(f"{key} {counts[key]}" for key in VERDICTS))
    if require_complete and reviewed != len(files):
        raise ValueError("Audit still has pending files")


def record(ledger, report, entries):
    files = ledger["files"]
    for path, entry in entries.items():
        if path not in files or files[path]["status"] != "pending" or f"- [ ] `{path}`\n" not in report:
            raise ValueError(f"File is outside the pending checklist: {path}")
        verify_entry(path, entry)
    owned = [str(path.relative_to(REPO)) for path in (REPORT, LEDGER)]
    for path, entry in entries.items():
        report = report.replace(f"- [ ] `{path}`\n", f"- [x] `{path}`\n", 1)
        report += "\n" + file_section(path, entry)
        files[path] = entry
        REPORT.write_text(report)
        LEDGER.write_text(json.dumps(ledger, indent=2, ensure_ascii=False) + "\n")
        git("diff", "--check", "--", *owned)
        git("commit", "--only", "-m", f"audit(cpp): review {path}", "--", *owned)
    print(f"Committed {len(entries)} file verdicts")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("record", "verify"))
    parser.add_argument("--complete", action="store_true")
    args = parser.parse_args()
    ledger = json.loads(LEDGER.read_text())
    report = REPORT.read_text()
    if args.action == "record":
        record(ledger, report, parse_records())
    else:
        verify(ledger, report, args.complete)


if __name__ == "__main__":
    main()
