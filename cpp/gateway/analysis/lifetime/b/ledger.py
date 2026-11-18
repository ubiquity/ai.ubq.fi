#!/usr/bin/env python3
"""Record reviews one file per commit, or check the audit against its source tree."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import sys


AUDIT = Path(__file__).resolve().parent
GATEWAY = AUDIT.parents[2]
REPORT = GATEWAY / "docs/findings/cppgw-lifetime-b-20261118.md"
INDEX = AUDIT / "sites.json"
VERDICTS = ("DEFECT", "SAFE", "UNCLEAR")
SCOPE = re.compile(
    r"(?:src|include/uos)/"
    r"(?:(?:runtime|storage|cache|kernel|worker)/.+|rt/(?:kv[^/]*|sqlite[^/]*|fs|process)\.[^/]+)"
)
VERDICT_START = "\n## File verdicts\n\n<!-- prettier-ignore-start -->\n\n"
RANKING_START = "\n<!-- prettier-ignore-end -->\n\n## Ranked defects and counts\n"
COUNT_LINE = re.compile(
    r"^\d+/\d+ files reviewed. \*\*DEFECT \d+, SAFE \d+, UNCLEAR \d+\.\*\*$",
    re.MULTILINE,
)


def git(*args, **kwargs):
    return subprocess.run(
        ["git", "-C", str(GATEWAY), *args], check=True, text=True, **kwargs
    )


def count_line(files):
    reviewed = sum(file["reviewed"] for file in files.values())
    totals = Counter(site["verdict"] for file in files.values() for site in file["sites"])
    return f"{reviewed}/{len(files)} files reviewed. **" + ", ".join(
        f"{name} {totals[name]}" for name in VERDICTS
    ) + ".**"


def quote(value):
    return value.replace("|", "\\|")


def render_file(path, file):
    lines = [f"### `{path}`", ""]
    if file.get("note"):
        lines.extend([file["note"], ""])
    if file["sites"]:
        lines.extend(["| Lines | Site | Verdict | Ownership or failure |", "| --- | --- | --- | --- |"])
        for site in file["sites"]:
            suffix = f" ([{site['defect']}](#{site['defect'].lower()}))" if "defect" in site else ""
            lines.append(
                f"| {site['lines']} | {quote(site['symbol'])} | **{site['verdict']}**{suffix} | "
                f"{quote(site['evidence'])} |"
            )
        lines.append("")
    return "\n".join(lines) + "\n"


def split_report(report):
    before, verdicts = report.split(VERDICT_START, 1)
    verdicts, ranking = verdicts.split(RANKING_START, 1)
    return before, verdicts, ranking


def read_reviews(batch):
    """Blocks start with '@@ path'. Subsequent rows are note or pipe-delimited sites."""
    path = None
    for raw in batch:
        line = raw.strip()
        if not line:
            continue
        if line.startswith("@@ "):
            if path:
                yield path, file
            path = line[3:]
            file = {"reviewed": True, "sites": []}
        elif path is None:
            raise ValueError("Expected '@@ gateway-relative-path'")
        elif line.startswith("note "):
            if "note" in file:
                raise ValueError(f"{path}: duplicate file note")
            file["note"] = line[5:]
        else:
            lines, verdict, kinds, symbol, evidence = line.split("|", 4)
            verdict, _, defect = verdict.partition(":")
            site = {
                "lines": lines, "verdict": verdict, "kinds": kinds.split(","),
                "symbol": symbol, "evidence": evidence,
            }
            if defect:
                site["defect"] = defect
            file["sites"].append(site)
    if path:
        yield path, file


def validate_file(path, file):
    if file["reviewed"] and not (file.get("note") or file["sites"]):
        raise ValueError(f"{path}: missing review evidence")
    source_lines = len((GATEWAY / path).read_text().splitlines())
    keys = set()
    for site in file["sites"]:
        key = (site["lines"], site["symbol"])
        if key in keys:
            raise ValueError(f"{path}: duplicate site {key}")
        keys.add(key)
        for span in site["lines"].split(","):
            ends = tuple(int(item) for item in span.split("-"))
            if not 1 <= len(ends) <= 2 or not 1 <= ends[0] <= ends[-1] <= source_lines:
                raise ValueError(f"{path}: invalid source locator {span}")
        if site["verdict"] not in VERDICTS or not site["evidence"] or not site["symbol"]:
            raise ValueError(f"{path}: invalid site {key}")
        if not set(site["kinds"]) <= set("abcde"):
            raise ValueError(f"{path}: invalid hazard class in {key}")
        if site["verdict"] == "DEFECT" and not re.fullmatch(r"D\d+", site.get("defect", "")):
            raise ValueError(f"{path}: missing ranking reference in {key}")


def update(path, file, index, author):
    files = index["files"]
    if next((name for name, entry in files.items() if not entry["reviewed"]), None) != path:
        raise ValueError(f"{path}: expected the first unticked file")
    validate_file(path, file)
    before, verdicts, ranking = split_report(REPORT.read_text())
    previous_count = count_line(files)
    files[path] = file
    unchecked = f"- [ ] `{path}`"
    if before.count(unchecked) != 1 or previous_count not in ranking:
        raise ValueError(f"{path}: report checklist or counts are out of sync")
    before = before.replace(unchecked, f"- [x] `{path}`")
    ranking = ranking.replace(previous_count, count_line(files), 1)
    REPORT.write_text(
        before + VERDICT_START + verdicts + render_file(path, file) + RANKING_START + ranking
    )
    INDEX.write_text(json.dumps(index, indent=2) + "\n")
    git("diff", "--check", "--", str(INDEX), str(REPORT))
    git("add", "--", str(INDEX), str(REPORT))
    git(
        "-c", "user.name=Codex", "-c", "user.email=codex@openai.com", "commit",
        "-m", f"docs(cpp): audit lifetime in {path}", "--", str(INDEX), str(REPORT),
        stdout=subprocess.PIPE if not author else None,
    )
    if not author:
        print(path, flush=True)


def verify(index, complete):
    files = index["files"]
    tracked = git("ls-files", "-z", stdout=subprocess.PIPE).stdout.split("\0")
    if set(files) != {name for name in tracked if SCOPE.fullmatch(name)}:
        raise ValueError("Inventory and tracked source scope differ")
    git("diff", "--exit-code", index["source_commit"], "--", *files, stdout=subprocess.PIPE)
    before, verdicts, ranking = split_report(REPORT.read_text())
    for path, file in files.items():
        validate_file(path, file)
        mark = "x" if file["reviewed"] else " "
        if before.count(f"- [{mark}] `{path}`") != 1:
            raise ValueError(f"{path}: report checklist differs")
        if complete and not file["reviewed"]:
            raise ValueError(f"{path}: review unfinished")
    expected = "".join(render_file(path, file) for path, file in files.items() if file["reviewed"])
    if verdicts.strip() != expected.strip():
        raise ValueError("Report site entries differ from JSON")
    if COUNT_LINE.findall(ranking) != [count_line(files)]:
        raise ValueError("Report totals differ from JSON")
    if complete:
        needed = {site["defect"] for file in files.values() for site in file["sites"] if "defect" in site}
        ranked = set(re.findall(r"<a id=\"(d\d+)\"></a>", ranking))
        if {key.lower() for key in needed} != ranked:
            raise ValueError("Ranking and defect references differ")
    print(count_line(files))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("record", "verify", "complete"))
    parser.add_argument("--commit-output", action="store_true")
    args = parser.parse_args()
    index = json.loads(INDEX.read_text())
    if args.command == "record":
        for path, file in read_reviews(sys.stdin):
            update(path, file, index, args.commit_output)
    else:
        verify(index, args.command == "complete")


if __name__ == "__main__":
    main()
