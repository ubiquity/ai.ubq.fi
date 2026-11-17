#!/usr/bin/python3
"""Inspect ELF runtime closure without executing an application or its loader."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

sys.dont_write_bytecode = True
READELF = Path("/usr/bin/readelf")
LIBRARY_ROOTS = (Path("/usr/lib"), Path("/lib"), Path("/lib64"))


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def regular(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or path.is_symlink():
        raise ValueError(f"Required input is not a regular non-symlink file: {path}")
    return path


def inspect(path, system_library=False):
    regular(path)
    with path.open("rb") as stream:
        header = stream.read(20)
    if len(header) < 20 or header[:4] != b"\x7fELF" or header[4:6] != b"\x02\x01":
        raise ValueError(f"Required input is not ELF64 little-endian: {path}")
    command = [str(READELF), "--wide", "--dynamic", "--program-headers", "--version-info", "--file-header", str(path)]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=15, env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"}, check=False)
    if completed.returncode:
        raise ValueError(f"ELF inspection failed for {path}: {completed.stderr.strip()}")
    output = completed.stdout
    needed = sorted(set(re.findall(r"\(NEEDED\).*?\[(.*?)\]", output)))
    interpreter = re.findall(r"Requesting program interpreter:\s*(.*?)\]", output)
    runpaths = re.findall(r"\((?:RUNPATH|RPATH)\).*?\[(.*?)\]", output)
    safe_system_paths = {str(root.resolve(strict=True)) for root in LIBRARY_ROOTS if root.exists()}
    if runpaths and (not system_library or any(part not in safe_system_paths for entry in runpaths for part in entry.split(":"))):
        raise ValueError(f"Package binary must use the explicit host closure, not RPATH/RUNPATH: {path}")
    versions = sorted(set(re.findall(r"Name:\s*((?:GLIBC|GLIBCXX|CXXABI)_[0-9.]+)", output)))
    machine = re.search(r"^\s*Machine:\s*(.*?)\s*$", output, re.MULTILINE)
    return {"machine": machine.group(1) if machine else "unknown", "needed": needed,
            "interpreter": interpreter[0] if interpreter else None, "versions": versions, "systemRunpaths": runpaths}


def find_library(name):
    if not name or "/" in name or name in (".", ".."):
        raise ValueError(f"Unsafe DT_NEEDED library name: {name}")
    for root in LIBRARY_ROOTS:
        candidate = root / name
        if candidate.exists():
            resolved = candidate.resolve(strict=True)
            if not any(resolved.is_relative_to(item.resolve(strict=True)) for item in LIBRARY_ROOTS if item.exists()):
                raise ValueError(f"Host library escapes qualified system roots: {candidate}")
            return regular(resolved)
    raise ValueError(f"Required host runtime library is unavailable: {name}")


def closure(binaries, host_programs=False):
    executables = []
    libraries = {}
    pending = []
    machine = None
    for label, path in sorted(binaries.items()):
        info = inspect(path, system_library=host_programs)
        if machine is not None and machine != info["machine"]:
            raise ValueError("Package ELF machine identities differ")
        machine = info["machine"]
        executables.append({"path": label, "sha256": digest(path), **info})
        pending.extend(info["needed"])
        if info["interpreter"]:
            interpreter = Path(info["interpreter"]).resolve(strict=True)
            regular(interpreter)
            libraries[str(interpreter)] = {"path": str(interpreter), "sha256": digest(interpreter), "needed": [], "machine": machine}
    visited = set()
    while pending:
        name = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        path = find_library(name)
        info = inspect(path, system_library=True)
        if info["machine"] != machine:
            raise ValueError(f"Host library ELF machine differs: {path}")
        libraries[str(path)] = {"path": str(path), "sha256": digest(path), "soname": name, **info}
        pending.extend(info["needed"])
    return {"schema": "uos-native-runtime-closure-v1", "platform": sys.platform, "machine": machine,
            "executables": executables, "libraries": [libraries[key] for key in sorted(libraries)],
            "hostData": [name for name in ("/etc/ssl", "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf", "/etc/localtime", "/dev/null", "/dev/urandom", "/dev/random") if Path(name).exists()]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", action="append", required=True, metavar="PACKAGE_PATH=ELF_PATH")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binaries = {}
    for value in args.binary:
        label, separator, filename = value.partition("=")
        if not separator or label in binaries or Path(label).is_absolute() or ".." in Path(label).parts:
            raise ValueError("Each --binary must have a unique safe package-relative path")
        binaries[label] = Path(filename).resolve(strict=True)
    document = closure(binaries)
    args.output.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
    print(f"Inspected {len(document['executables'])} executables and {len(document['libraries'])} host libraries; application not executed")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Runtime closure refused: {error}", file=sys.stderr)
        sys.exit(1)
