#!/usr/bin/env python3
"""Attribute Heaptrack v3 allocation events to their nearest gateway phase."""

import argparse
from collections import defaultdict
from functools import lru_cache
import json
from pathlib import Path
import re
import subprocess

labels = [
    ("authentication", r"authenticate(?:ClientKvApiKey|ApiKeyToken)|erasePolicy|cachePolicy"),
    ("quota and usage", r"(api_key_policy|native_api_key)\.cpp|/paid_fallback/(ledger|rollups)|apiKeyUsageV3"),
    ("catalog", r"/catalog/(store|index|types)|catalogResponse|loadCatalogBody"),
    ("request handling", r"/(request|responses_request)\.cpp|prepareResponsesRequest|handleResponsesInternal|readJsonBodyWithLimit|JsonInput"),
    ("streaming and dispatch", r"/responses_(attempts|stream|buffered|failover_stream)\.cpp|codex/(responses_|dispatch|index)|/fetch_transport|/stream_relay|prepareCodexSubscriptionRequest"),
    ("terminal logging and telemetry", r"/handler/terminal_log|/openai_telemetry|/cache/prompt_analytics|/cache/telemetry_gate|logTerminal"),
    ("replay recording", r"/sentinel/(replay|upstream_capture)|[Ss]entinelReplay|[Uu]pstreamCapture"),
    ("SQLite shared", r"libsqlite3|/rt/(sqlite|kv_sqlite)\.cpp"),
]
rules = [(label, re.compile(pattern)) for label, pattern in labels]

def classify(frames):
    for name, path, library in frames:
        text = "|".join((name, path, library))
        for label, pattern in rules:
            if pattern.search(text):
                return label, f"{name} {path}".strip() or library
    return "other", "other"

def summarize(path):
    strings = [""]
    ips = [()]
    traces = [(0, 0)]
    allocations = []
    current, peak = defaultdict(int), defaultdict(int)
    sites, site_peak = defaultdict(int), defaultdict(int)
    active = 0
    high = 0

    @lru_cache(maxsize=None)
    def classification(trace):
        if trace == 0:
            return "other", "other"
        ip, parent = traces[trace]
        label, site = classify(ips[ip])
        return classification(parent) if label == "other" else (label, site)

    with subprocess.Popen(["zstdcat", path], stdout=subprocess.PIPE, text=True) as process:
        for line in process.stdout:
            command, _, content = line.rstrip("\n").partition(" ")
            if command == "v" and content.split()[1] != "3":
                raise RuntimeError("Expected a Heaptrack v3 allocation trace")
            if command == "s":
                strings.append(content.partition(" ")[2])
            elif command in {"i", "t", "a", "+", "-"}:
                parts = tuple(int(item, 16) for item in content.split())
                if command == "i":
                    module = strings[parts[1]]
                    frames = []
                    for offset in range(2, len(parts), 3):
                        name = strings[parts[offset]]
                        source = strings[parts[offset + 1]] if offset + 1 < len(parts) else ""
                        frames.append((name, source, module))
                    if not frames:
                        frames.append(("", "", module))
                    ips.append(frames)
                elif command == "t":
                    traces.append(parts)
                elif command == "a":
                    size, trace = parts
                    allocations.append((size, *classification(trace)))
                else:
                    size, label, site = allocations[parts[0]]
                    delta = size if command == "+" else -size
                    active += delta
                    high = max(high, active)
                    current[label] += delta
                    peak[label] = max(peak[label], current[label])
                    site_key = label, site
                    sites[site_key] += delta
                    site_peak[site_key] = max(site_peak[site_key], sites[site_key])
        if process.wait():
            raise RuntimeError("Trace decompression failed")
    result = {}
    for label in current:
        on_peak = sorted(((size, name) for (area, name), size in site_peak.items() if area == label), reverse=True)[:3]
        at_stop = sorted(((size, name) for (area, name), size in sites.items() if area == label), reverse=True)[:3]
        result[label] = {"peak_bytes": peak[label], "retained_bytes": current[label], "peak_sites": on_peak, "retained_sites": at_stop}
    return {"peak": high, "retained": active, "phases": result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.trace), indent=2))


if __name__ == "__main__":
    main()
