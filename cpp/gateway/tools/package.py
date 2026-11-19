#!/usr/bin/python3
"""Create a deterministic native gateway directory; never install or launch it."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys

sys.dont_write_bytecode = True
import vendor_sources

GATEWAY = Path(__file__).resolve().parents[1]
WORKSPACE = GATEWAY.parents[1]
spec = importlib.util.spec_from_file_location("uos_runtime_manifest", GATEWAY / "tools/runtime-manifest.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


def safe_directory(path):
    path = path.absolute()
    if path.resolve(strict=True) != path or not stat.S_ISDIR(path.lstat().st_mode):
        raise ValueError(f"Input directory must be canonical without symlinks: {path}")
    return path


def read_json(path):
    runtime.regular(path)
    if path.stat().st_size > 1024 * 1024:
        raise ValueError(f"Manifest is oversized: {path}")
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise ValueError(f"Manifest must be an object: {path}")
    return value


def copy_file(source, destination, executable=False):
    runtime.regular(source)
    before = runtime.digest(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination, follow_symlinks=False)
    destination.chmod(0o755 if executable else 0o644)
    if before != runtime.digest(source) or before != runtime.digest(destination):
        raise ValueError(f"Input changed during packaging: {source}")


def copy_vendor_licenses(stage):
    production_sources = (path.relative_to(GATEWAY).as_posix() for path in (GATEWAY / "src").rglob("*.cpp")
                          if not path.name.startswith("."))
    libraries = vendor_sources.required(vendor_sources.selected(GATEWAY), production_sources)
    for library in libraries:
        destination = stage / "licenses" / library.name
        copy_file(GATEWAY / "third_party" / library.name / "LICENSE", destination / "LICENSE")
        notice = GATEWAY / "docs/licenses" / library.name / "NOTICE"
        if library.name in ("simdjson", "simdutf") or notice.exists():
            copy_file(notice, destination / "NOTICE")
    return {library.name: library.version for library in libraries}


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    path.chmod(0o644)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--profile-manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build = safe_directory(args.build_dir)
    if args.profile_manifest.absolute() != build / "profile-owner.json":
        raise ValueError("Compiler/profile ownership must belong to the selected build directory")
    profile = read_json(args.profile_manifest)
    if profile.get("schema") != "uos-gateway-profile-v1" or profile.get("profile") not in ("release", "clang-release") or profile.get("sanitizer") is not None or profile.get("source_root") != str(GATEWAY) or profile.get("workspace_root") != str(WORKSPACE):
        raise ValueError("The qualified compiler/profile identity is required")
    binaries = {"bin/uos-gateway": build / "uos_gateway"}
    document = runtime.closure(binaries)
    operators = runtime.closure({name: Path(name).resolve(strict=True) for name in ("/bin/sh", "/usr/bin/env")}, host_programs=True)
    required = {"libsqlite3.so.0", "libz.so.1", "libbrotlidec.so.1", "libzstd.so.1"}
    needed = {name for row in document["executables"] for name in row["needed"]}
    if not required.issubset(needed):
        raise ValueError(f"Primary gateway does not link direct native libraries: {sorted(required - needed)}")
    output = args.output.absolute()
    data = (WORKSPACE / ".data").resolve(strict=True)
    if not output.is_relative_to(data) or output == data or output.exists() or output.parent.resolve(strict=True) != output.parent:
        raise ValueError("Package output must be a new directory under the canonical worktree .data")
    stage = output.with_name(output.name + ".partial")
    if stage.exists():
        raise ValueError(f"An unfinished package already exists; preserve and reconcile it: {stage}")
    stage.mkdir(mode=0o755)
    stage.chmod(0o755)
    try:
        for name, source in sorted(binaries.items()):
            copy_file(source, stage / name, True)
        assets = safe_directory(WORKSPACE / "static")
        for source in sorted(assets.rglob("*")):
            if source.is_symlink():
                raise ValueError(f"Static assets must not be symlinks: {source}")
            if source.is_file():
                copy_file(source, stage / "static" / source.relative_to(assets))
        for name in ("favicon.png", "favicon-32.png"):
            copy_file(WORKSPACE / name, stage / name)
        for name in ("launch-native.sh", "uos-gateway-native.service", "README.md"):
            copy_file(GATEWAY / "ops" / name, stage / "ops" / name, name.endswith(".sh"))
        copy_vendor_licenses(stage)
        copy_file(GATEWAY / "packaging/SQLITE-LICENSE", stage / "licenses/sqlite/LICENSE")
        copy_file(GATEWAY / "packaging/DENOKV-LICENSE", stage / "licenses/denokv/LICENSE")
        copy_file(GATEWAY / "docs/RUNTIME.md", stage / "README.md")
        copy_file(args.profile_manifest, stage / "build-profile.json")
        copy_file(GATEWAY / "runtime/launch-schema.json", stage / "runtime/launch-schema.json")
        write_json(stage / "runtime-closure.json", document)
        write_json(stage / "operator-closure.json", operators)
        write_json(stage / "package-info.json", {"schema": "uos-native-package-v1",
                                          "platform": "linux", "architecture": "arm64", "profileSha256": runtime.digest(args.profile_manifest)})
        for row in document["executables"]:
            if runtime.digest(binaries[row["path"]]) != row["sha256"] or runtime.digest(stage / row["path"]) != row["sha256"]:
                raise ValueError("A selected executable changed after ELF closure inspection")
        for row in document["libraries"]:
            if runtime.digest(Path(row["path"])) != row["sha256"]:
                raise ValueError("A host runtime library changed after ELF closure inspection")
        ordered_files = sorted((path for path in stage.rglob("*") if path.is_file()), key=lambda path: path.relative_to(stage).as_posix())
        entries = [f"{runtime.digest(path)}  {path.relative_to(stage).as_posix()}\n" for path in ordered_files]
        (stage / "manifest.sha256").write_text("".join(entries))
        (stage / "manifest.sha256").chmod(0o644)
        for path in sorted(stage.rglob("*"), reverse=True):
            if path.is_dir():
                path.chmod(0o755)
            os.utime(path, (0, 0), follow_symlinks=False)
        os.utime(stage, (0, 0))
        stage.rename(output)
    except BaseException:
        print(f"Incomplete owned package retained for diagnosis: {stage}", file=sys.stderr)
        raise
    print(f"Packaged {len(entries)} files; manifest SHA256 {runtime.digest(output / 'manifest.sha256')}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Native package refused: {error}", file=sys.stderr)
        sys.exit(1)
