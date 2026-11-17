#!/usr/bin/env python3
"""Rank functions by sampled user cycles, deduplicating recursion in inclusive costs."""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import subprocess


HEADER = re.compile(r"^\s*\d+/\d+\s+(\d+)\s*$")
FRAME = re.compile(r"^\t\s*[a-f0-9]+\s+(.*?) \(([^()]*)\)(?: \(inlined\))?\s*$")
SOURCE = re.compile(r"^\s+(.+:\d+)\s*$")


def source_label(location):
    for marker in ("cpp/gateway/", "boost_1_92_0/", "/libstdc++-v3/", "/glibc/"):
        if marker in location:
            return marker.lstrip("/") + location.rsplit(marker, 1)[1]
    return location


def samples(stream):
    period, frames = 0, []
    for line in stream:
        if header := HEADER.match(line):
            if frames:
                yield period, frames
            period, frames = int(header[1]), []
        elif frame := FRAME.match(line):
            frames.append([(Path(frame[2]).name, frame[1]), None])
        elif frames and (source := SOURCE.match(line)):
            frames[-1][1] = source_label(source[1].strip())
    if frames:
        yield period, frames


def rank(data, limit):
    command = ["perf", "script", "-i", str(data), "-F", "pid,tid,period,ip,sym,dso,srcline", "--full-source-path", "--inline"]
    inclusive, exclusive = Counter(), Counter()
    inclusive_locations, exclusive_locations, leaf_callers = (defaultdict(Counter) for _ in range(3))
    count, weight, depth = 0, 0, 0
    with subprocess.Popen(command, stdout=subprocess.PIPE, text=True) as perf:
        for period, frames in samples(perf.stdout):
            if not period:
                raise RuntimeError("perf emitted a stack without its sample period")
            count += 1
            weight += period
            depth += len(frames)
            leaf, location = frames[0]
            exclusive[leaf] += period
            if location:
                exclusive_locations[leaf][location] += period
            if len(frames) > 1:
                leaf_callers[leaf][frames[1][0][1]] += period
            for function, location in dict(reversed(frames)).items():
                inclusive[function] += period
                if location:
                    inclusive_locations[function][location] += period
        if perf.wait() != 0:
            raise RuntimeError("perf script failed while symbolizing the call chains")
    if count < 20:
        raise RuntimeError(f"Only {count} sampled call chains; extend the profiling batch")

    def rows(costs, locations):
        top = []
        for (dso, function), cycles in costs.most_common(limit):
            source = locations[(dso, function)].most_common(1)
            parents = leaf_callers[(dso, function)].most_common(3)
            top.append({"function": function, "dso": dso, "location": source[0][0] if source else None,
                        "cycles": cycles, "percent": 100 * cycles / weight, "top_leaf_callers_cycles": dict(parents)})
        return top

    return {"samples": count, "sampled_cycles": weight, "mean_stack_depth": depth / count,
            "inclusive": rows(inclusive, inclusive_locations), "exclusive": rows(exclusive, exclusive_locations)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("perf_data", type=Path)
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()
    print(json.dumps(rank(args.perf_data, args.top), indent=2))


if __name__ == "__main__":
    main()
