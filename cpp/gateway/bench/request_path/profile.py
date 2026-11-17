#!/usr/bin/env python3
"""Rank functions by sampled user cycles, deduplicating recursion in inclusive costs."""

import argparse
from collections import Counter, defaultdict
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


HEADER = re.compile(r"^\s*\d+/\d+\s+(\d+)\s*$")
FRAME = re.compile(r"^\t\s*[a-f0-9]+\s+(.*) \(((?:/|\[).*?)(?:\+0x([a-f0-9]+))?\)\s*$")
LOAD = re.compile(r"^\s*LOAD\s+(0x\w+)\s+(0x\w+)\s+0x\w+\s+(0x\w+)\s+0x\w+\s+([RWE ]+)\s+0x\w+", re.M)


def source_label(location):
    location = os.path.normpath(location)
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
            offset = int(frame[3], 16) if frame[3] else None
            # On AArch64 an FP caller carries the saved return PC, one 4-byte instruction after the call.
            if frames and offset is not None and offset >= 4:
                offset -= 4
            frames.append((frame[2], offset, frame[1]))
    if frames:
        yield period, frames


def symbolized_frames(stacks, symbolizer):
    offsets = defaultdict(set)
    for _, frames in stacks:
        for module, offset, _ in frames:
            if module.startswith("/") and offset is not None:
                offsets[module].add(offset)
    debug = {}
    for module, addresses in offsets.items():
        if not Path(module).is_file():
            continue
        elf = subprocess.check_output(["readelf", "-lW", module], text=True)
        segments = [(int(file, 16), int(virtual, 16), int(size, 16)) for file, virtual, size, flags in LOAD.findall(elf) if "E" in flags]
        # perf dsoff is a file offset. The executable segment's ELF VMA can differ, including under mold.
        virtual = {address + base - file: address for file, base, size in segments for address in addresses if file <= address < file + size}
        if not virtual:
            continue
        result = subprocess.check_output([symbolizer, "--output-style=JSON", "--obj=" + module], text=True,
                                         input="".join(f"{address:#x}\n" for address in sorted(virtual)))
        for line in result.splitlines():
            row = json.loads(line)
            offset = virtual[int(row["Address"], 16)]
            entries = []
            for item in row["Symbol"]:
                name = item["FunctionName"]
                if not name:
                    continue
                file, line = item["FileName"], item["Line"]
                location = source_label(f"{file}:{line}") if line else None
                # Minimal DWARF can omit the linkage names of inlined lambdas and local helpers.
                # Keep their source identity so unrelated `operator()` or `run` frames do not merge.
                origin = "" if "::" in name.split("(", 1)[0] else source_label(f"{item['StartFileName'] or file}:{item['StartLine']}")
                entries.append((name, origin, location))
            debug[module, offset] = entries
    for period, stack in stacks:
        frames = []
        for module, offset, function in stack:
            dso = Path(module).name
            expanded = debug.get((module, offset), [])
            if expanded:
                frames.extend(((dso, name, origin), location) for name, origin, location in expanded[:-1])
                name, _, location = expanded[-1]
                frames.append(((dso, function if function != "[unknown]" else name, ""), location))
            else:
                frames.append(((dso, function, ""), None))
        yield period, frames


def rank(data, limit, symbolizer):
    command = ["perf", "script", "-i", str(data), "--no-inline", "-F", "pid,tid,period,ip,sym,dso,dsoff"]
    with subprocess.Popen(command, stdout=subprocess.PIPE, text=True) as perf:
        stacks = list(samples(perf.stdout))
        if perf.wait() != 0:
            raise RuntimeError("perf script failed while reading the call chains")
    if len(stacks) < 20:
        raise RuntimeError(f"Only {len(stacks)} sampled call chains; extend the profiling batch")
    inclusive, exclusive = Counter(), Counter()
    inclusive_locations, exclusive_locations, leaf_callers = (defaultdict(Counter) for _ in range(3))
    count, weight, depth = 0, 0, 0
    for period, frames in symbolized_frames(stacks, symbolizer):
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

    def rows(costs, locations):
        top = []
        for (dso, function, origin), cycles in costs.most_common():
            if function == "[unknown]":
                continue
            source = locations[(dso, function, origin)].most_common(1)
            parents = leaf_callers[(dso, function, origin)].most_common(3)
            top.append({"function": function, "dso": dso, "location": source[0][0] if source else None,
                        "cycles": cycles, "percent": 100 * cycles / weight, "top_leaf_callers_cycles": dict(parents)})
            if len(top) == limit:
                break
        return top

    return {"samples": count, "sampled_cycles": weight, "mean_stack_depth": depth / count,
            "unresolved_leaf_percent": 100 * sum(cycles for (_, function, _), cycles in exclusive.items() if function == "[unknown]") / weight,
            "inclusive": rows(inclusive, inclusive_locations), "exclusive": rows(exclusive, exclusive_locations)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("perf_data", type=Path)
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--symbolizer", default=shutil.which("llvm-symbolizer"), help="LLVM symbolizer executable for inline DWARF and source lines")
    args = parser.parse_args()
    if args.top < 1:
        parser.error("--top must be positive")
    if not args.symbolizer:
        parser.error("Install llvm-symbolizer or pass --symbolizer for a versioned executable")
    print(json.dumps(rank(args.perf_data, args.top, args.symbolizer), indent=2))


if __name__ == "__main__":
    main()
