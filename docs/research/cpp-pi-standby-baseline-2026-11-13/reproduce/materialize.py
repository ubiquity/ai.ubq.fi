#!/usr/bin/env python3
"""Render accepted host-scoped recipe sources into a fresh task directory; never execute them."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shlex


def sha(value):
    return hashlib.sha256(value).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipe", required=True)
    parser.add_argument("--worktree", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    bundle = json.loads(Path(__file__).with_name("recipes.json").read_text())
    if args.recipe not in bundle["recipes"]:
        parser.error("Unknown recipe; choices: " + ", ".join(bundle["recipes"]))
    recipe = bundle["recipes"][args.recipe]
    work = args.worktree.resolve(strict=True)
    output = args.output.resolve()
    if output.exists():
        parser.error("Output already exists; accepted receipts must never be overwritten")
    if not (output.is_relative_to(Path("/tmp")) or output.is_relative_to(work / ".data")):
        parser.error("Output must be below /tmp or the selected worktree's .data directory")
    for name, expected in recipe["sourceInputsSha256"].items():
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts:
            parser.error("Unsafe source input path")
        if sha((work / name).read_bytes()) != expected:
            parser.error("Frozen source input mismatch: " + name)
    rewrites = {
        recipe["originalRoot"]: str(output),
        recipe["originalWorktree"]: str(work),
        recipe["originalUnitPrefix"]: "uos-pr1-replay-" + sha(str(output).encode())[:10] + "-",
    }
    rendered = {}
    for name, expected in recipe["files"].items():
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts:
            parser.error("Unsafe archived source path")
        blob = bundle["blobs"][expected]
        value = blob["utf8"]
        if sha(value.encode()) != expected or blob["sha256"] != expected:
            parser.error("Archived source checksum mismatch: " + name)
        for before, after in rewrites.items():
            value = value.replace(before, after)
        rendered[name] = value.encode()
    if "planPath" in recipe:
        name = recipe["planPath"]
        plan = json.loads(rendered[name])
        for input_path in plan["inputs"]:
            candidate = Path(input_path)
            if candidate.is_relative_to(output):
                relative = str(candidate.relative_to(output))
                if relative not in rendered:
                    parser.error("Missing archived plan input: " + relative)
                plan["inputs"][input_path] = sha(rendered[relative])
        rendered[name] = (json.dumps(plan, indent=2) + "\n").encode()
    output.mkdir(parents=True, mode=0o700)
    for name, value in rendered.items():
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(value)
        destination.chmod(0o600)
    if "planPath" in recipe:
        (output / recipe["planPath"]).parent.joinpath("state").mkdir(mode=0o700)
    for name in ["tmp", "control"]:
        (output / name).mkdir(mode=0o700)
    argv = [str(output / item) if item in rendered else item for item in recipe["argv"]]
    if "planPath" in recipe:
        plan_path = recipe["planPath"]
        argv = ["/usr/bin/python3", str(output / recipe["entrypoint"]), str((output / plan_path).parent), sha(rendered[plan_path])]
    result = {
        "recipe": args.recipe,
        "acceptedEvidenceRef": recipe["acceptedEvidenceRef"],
        "requiredSourceHead": recipe["requiredSourceHead"],
        "requiredSourceTree": recipe["requiredSourceTree"],
        "worktree": str(work),
        "rewrites": rewrites,
        "renderedSha256": {name: sha(value) for name, value in rendered.items()},
        "argv": argv,
        "executed": False,
        "next": "Obtain the explicit task-only resource/evidence slot; register argv from this worktree, then capture once.",
    }
    (output / "rendered-recipe.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    print("Command only; not executed: " + shlex.join(argv))


if __name__ == "__main__":
    main()
