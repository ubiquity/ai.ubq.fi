#!/usr/bin/python3
"""Require exact marked defect diagnostics and a separate clean control receipt."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--suffix", default="v1")
    parser.add_argument("--coroutine-suffix")
    args = parser.parse_args()
    result = []
    for name in ("defects", "coroutines", "clean"):
        suffix = args.coroutine_suffix if name == "coroutines" and args.coroutine_suffix else args.suffix
        path = args.evidence / ("control-" + name + "-" + suffix + ".json")
        receipt = json.loads(path.read_text())
        source = Path(receipt["source"])
        expected = [(number, match.group(1)) for number, line in enumerate(source.read_text().splitlines(), 1)
                    if (match := re.search(r"CONTROL: ([A-Za-z0-9_.-]+)", line))]
        actual = {(int(item["line"]), item["check"].split(",", 1)[0]) for item in receipt["diagnostics"]}
        missing = [(number, check) for number, check in expected if (number, check) not in actual]
        unchanged = hashlib.sha256(source.read_bytes()).hexdigest() == receipt["source_sha256"]
        passed = unchanged and not missing and (receipt["code"] == 0 and not actual if name == "clean" else receipt["code"] == 1)
        result.append({"control": name, "receipt": str(path), "expected": expected,
                       "diagnostics": len(actual), "missing": missing, "source_unchanged": unchanged, "passed": passed})
    output = {"controls": result, "passed": all(item["passed"] for item in result)}
    target = args.evidence / ("controls-verified-" + args.suffix + ".json")
    target.write_text(json.dumps(output, indent=2) + "\n")
    print(json.dumps(output, indent=2))
    return 0 if output["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
