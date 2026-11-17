#!/usr/bin/env python3
"""Render median and run-to-run range, and rank measured native/Bun throughput ratios."""

import argparse
from collections import defaultdict
import json
from pathlib import Path
import statistics


METRICS = {
    "requests_per_s": ("req/s", 1), "p50_ms": ("p50 ms", 3), "p95_ms": ("p95 ms", 3), "p99_ms": ("p99 ms", 3),
    "gateway_cpu_ms_per_request": ("CPU ms/req", 3), "peak_rss_mib": ("peak RSS MiB", 1),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt", type=Path)
    args = parser.parse_args()
    runs = defaultdict(list)
    for trial in json.loads(args.receipt.read_text())["trials"]:
        runs[trial["scenario"], trial["runtime"]].append(trial)
    labels = ["Scenario", "Runtime", "n/rep", *(label for label, _ in METRICS.values())]
    print("| " + " | ".join(labels) + " |")
    print("|" + "---|" * len(labels))
    ratios = []
    for name in dict.fromkeys(name for name, _ in runs):
        rates = {}
        for runtime in ("bun", "native"):
            group = runs[name, runtime]
            if not group:
                continue
            rates[runtime] = statistics.median(row["requests_per_s"] for row in group)
            cells = [name, runtime, str(group[0]["requests"])]
            for metric, (_, digits) in METRICS.items():
                values = [row[metric] for row in group]
                cells.append(f"{statistics.median(values):.{digits}f} [{min(values):.{digits}f}–{max(values):.{digits}f}]")
            print("| " + " | ".join(cells) + " |")
        if len(rates) == 2:
            ratios.append((rates["native"] / rates["bun"], name))
    if ratios:
        print("\nNative/Bun median throughput, lowest first\n")
        for factor, name in sorted(ratios):
            print(f"- {name} {factor:.3f}×")


if __name__ == "__main__":
    main()
