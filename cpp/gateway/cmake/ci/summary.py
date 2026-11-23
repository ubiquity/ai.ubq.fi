#!/usr/bin/env python3
"""Render the driver's final receipt as a GitHub Actions job summary."""
import argparse
import json
from pathlib import Path

LABELS = {"release": "Release", "asan": "ASan, UBSan and LeakSanitizer"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_root", type=Path)
    parser.add_argument("--config", choices=LABELS, default="release")
    options = parser.parse_args()
    receipts = list(options.build_root.glob(f"*/{options.config}/receipt.json"))
    print(f"### C++ gateway {LABELS[options.config]} results\n")
    if not receipts:
        print("The driver produced no final receipt. See the step logs and any uploaded diagnostics.")
        return
    if len(receipts) != 1:
        parser.error(f"expected one {options.config} receipt in {options.build_root}")
    receipt = json.loads(receipts[0].read_text())
    results = receipt["results"]
    failing = [row for row in results if row["status"] != "PASS"]
    cases = receipt["cases"]
    print("| Binaries built | Binaries passed | Binaries failed | Cases passed | Cases failed |")
    print("| ---: | ---: | ---: | ---: | ---: |")
    print(f"| {sum(row['built'] for row in results)} | {len(results) - len(failing)} | {len(failing)} "
          f"| {cases['passed']} | {cases['failed']} |")
    if receipt["compile_exit"] or not receipt["inputs_unchanged"]:
        print("\nThe driver reported an object build error or changed inputs. See the receipt and compiler logs.")
    if failing:
        print("\nFailing tests\n")
        for row in failing:
            print(f"- `{row['test']}` ({row['status']})")


if __name__ == "__main__":
    main()
