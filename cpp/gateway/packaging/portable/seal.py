#!/usr/bin/python3
"""Create a new immutable gateway directory for the installed Debian libraries."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
import runtime_manifest as runtime

EXECUTABLES = ("bin/uos-gateway",)
INTERPRETER = "/lib/ld-linux-aarch64.so.1"
DIRECT_LIBRARIES = {"libssl.so.3", "libcrypto.so.3", "libz.so.1", "libbrotlidec.so.1", "libzstd.so.1"}
SYSTEM_LIBRARIES = DIRECT_LIBRARIES | {"libc.so.6", "libm.so.6", "ld-linux-aarch64.so.1"}
COMMAND_ENV = {"PATH": "/usr/bin:/bin", "LC_ALL": "C"}


def manifest(root):
    records = []
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        mode = path.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        name = path.relative_to(root).as_posix()
        if not stat.S_ISREG(mode) or path.stat().st_nlink != 1 or mode & 0o022 or "\n" in name:
            raise ValueError(f"Unsafe package entry: {path}")
        if name != "manifest.sha256":
            records.append(f"{runtime.digest(path)}  {name}\n")
    return "".join(records)


def require_portable(info, path):
    needed = set(info["needed"])
    versions = info["versions"]
    if (info["machine"] != "AArch64" or info["interpreter"] != INTERPRETER or
            not DIRECT_LIBRARIES <= needed <= SYSTEM_LIBRARIES or
            any(not version.startswith("GLIBC_") or tuple(map(int, version[6:].split("."))) > (2, 36) for version in versions)):
        raise ValueError(f"Executable exceeds the portable ARM64 ABI: {path}: {info}")


def verify_loader(binaries, closure):
    paths = set()
    for binary in binaries.values():
        result = subprocess.run(["/usr/bin/ldd", "-v", str(binary)], env=COMMAND_ENV, capture_output=True, text=True, timeout=20, check=True)
        if result.stderr or "not found" in result.stdout:
            raise ValueError(f"Unresolved runtime dependency for {binary}: {result.stderr}{result.stdout}")
        listing = result.stdout.split("\n\n", 1)[0]
        for match in re.finditer(r"^\s*(?:\S+ => )?(/\S+)\s+\(0x[0-9a-f]+\)$", listing, re.M):
            paths.add(str(Path(match[1]).resolve(strict=True)))
    qualified = {row["path"] for row in closure["libraries"]}
    if paths != qualified or any(not name.startswith("/usr/lib/") for name in qualified):
        raise ValueError(f"Loader and package closure differ: {sorted(paths ^ qualified)}")


def seal(payload, output):
    payload = payload.resolve(strict=True)
    output = output.absolute()
    if output.exists() or output.is_symlink() or output.parent.resolve(strict=True) != output.parent or output.is_relative_to(payload):
        raise ValueError("Choose a new package directory outside the payload with a canonical existing parent")
    original = runtime.regular(payload / "manifest.sha256").read_text()
    if manifest(payload) != original:
        raise ValueError("Payload manifest verification failed")

    # The tracked closure reader expects Arch's flat library layout by default.
    runtime.LIBRARY_ROOTS = (Path("/usr/lib/aarch64-linux-gnu"), *runtime.LIBRARY_ROOTS)
    stage = Path(tempfile.mkdtemp(prefix=".uos-seal-", dir=output.parent))
    try:
        shutil.copytree(payload, stage, dirs_exist_ok=True)
        if manifest(stage) != original:
            raise ValueError("Payload changed while copying")
        binaries = {name: stage / name for name in EXECUTABLES}
        for name, binary in binaries.items():
            require_portable(runtime.inspect(binary), name)
        closure = runtime.closure(binaries)
        verify_loader(binaries, closure)
        operators = {name: Path(name).resolve(strict=True) for name in ("/bin/sh", "/usr/bin/env")}
        operator_closure = runtime.closure(operators, host_programs=True)
        verify_loader(operators, operator_closure)
        for name, document in (("runtime-closure.json", closure), ("operator-closure.json", operator_closure)):
            (stage / name).write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
            (stage / name).chmod(0o644)
        (stage / "manifest.sha256").write_text(manifest(stage))
        stage.chmod(0o755)
        stage.rename(output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)

    return {"packageRoot": str(output), "packageManifestSha256": runtime.digest(output / "manifest.sha256")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    options = parser.parse_args()
    os.umask(0o022)
    print(json.dumps(seal(Path(__file__).resolve().parent / "payload", options.output), sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f"Portable package refused: {error}")
