#!/usr/bin/env python3
"""Print the next wave of port assignments from PORT-MANIFEST.json.

    python3 cpp/gateway/tools/plan.py [agents=8] [budget_loc=2600]

A module is eligible when every import is already ported or sits in the same
assignment. Import cycles are kept together; a cycle larger than the budget is
reported separately because it needs a headers-first pass."""
import json, sys

def main():
    agents = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    budget = int(sys.argv[2]) if len(sys.argv) > 2 else 2600
    modules = json.load(open("cpp/gateway/PORT-MANIFEST.json"))["modules"]
    deps = {name: set(entry["deps"]) for name, entry in modules.items()}
    sys.setrecursionlimit(100000)
    index, low, stack, on, component, groups, counter = {}, {}, [], set(), {}, [], [0]
    def visit(node):
        index[node] = low[node] = counter[0]; counter[0] += 1; stack.append(node); on.add(node)
        for dep in deps[node]:
            if dep not in index:
                visit(dep); low[node] = min(low[node], low[dep])
            elif dep in on:
                low[node] = min(low[node], index[dep])
        if low[node] == index[node]:
            group = []
            while True:
                top = stack.pop(); on.discard(top); group.append(top)
                if top == node: break
            for member in group: component[member] = len(groups)
            groups.append(sorted(group))
    for name in sorted(deps):
        if name not in index: visit(name)
    size = {i: sum(modules[m]["loc"] for m in g) for i, g in enumerate(groups)}
    needs = {i: {component[d] for m in g for d in deps[m]} - {i} for i, g in enumerate(groups)}
    done = {i for i, g in enumerate(groups) if all(modules[m]["ported"] for m in g)}
    pending = set(range(len(groups))) - done
    # Transitive dependant weight: port what unblocks the most first.
    dependants = {i: set() for i in range(len(groups))}
    for i in range(len(groups)):
        for d in needs[i]: dependants[d].add(i)
    weight = {}
    def unblocks(i):
        if i not in weight:
            seen, todo = set(), [i]
            while todo:
                for nxt in dependants[todo.pop()]:
                    if nxt not in seen: seen.add(nxt); todo.append(nxt)
            weight[i] = sum(size[s] for s in seen)
        return weight[i]
    chunks, load, owner = [[] for _ in range(agents)], [0] * agents, {}
    oversized = []
    progress = True
    while progress:
        progress = False
        for i in sorted(pending - set(owner), key=lambda i: (-unblocks(i), -size[i])):
            missing = [d for d in needs[i] if d not in done]
            if any(d not in owner for d in missing): continue
            owners = {owner[d] for d in missing}
            if len(owners) > 1: continue
            if size[i] > budget * 2 and not owners:
                if i not in oversized: oversized.append(i)
                continue
            slot = next(iter(owners)) if owners else min(range(agents), key=lambda a: load[a])
            if load[slot] and load[slot] + size[i] > budget: continue
            owner[i] = slot; chunks[slot].append(i); load[slot] += size[i]; progress = True
    out = {"remaining_loc": sum(size[i] for i in pending), "assignments": [], "oversized_cycles": []}
    for slot, chunk in enumerate(chunks):
        files = [m for i in chunk for m in groups[i]]
        if files: out["assignments"].append({"loc": load[slot], "files": files})
    for i in oversized:
        out["oversized_cycles"].append({"loc": size[i], "files": groups[i], "blocked_on": sorted(m for d in needs[i] if d not in done for m in groups[d])})
    json.dump(out, sys.stdout, indent=1); print()

if __name__ == "__main__":
    main()
