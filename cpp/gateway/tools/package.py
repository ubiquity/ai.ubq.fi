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


def verify_native_kv(directory, inputs_path, expected_provenance):
    directory = safe_directory(directory)
    inputs = read_json(inputs_path)
    provenance_path = directory / "native-kv.provenance.json"
    provenance = read_json(provenance_path)
    if runtime.digest(provenance_path) != expected_provenance:
        raise ValueError("Native KV provenance differs from independently supplied hash")
    if inputs.get("schema") != 1 or provenance.get("schema") != 1 or provenance.get("inputsSha256") != runtime.digest(inputs_path):
        raise ValueError("Native KV frozen-input identity differs")
    if inputs.get("sdk") != {"name": "@deno/kv", "version": "0.14.0"} or inputs.get("source", {}).get("commit") != "9fe65cc138ff56ca5bfe7a1bf8c53e832246f9d5":
        raise ValueError("Native KV source identity is unsupported")
    for field in ("source", "patches", "lock", "toolchain", "recipe"):
        if provenance.get(field) != inputs.get(field):
            raise ValueError(f"Native KV provenance differs from frozen {field}")
    build = provenance.get("build", {})
    if build.get("exitCode") != 0 or not isinstance(build.get("logSha256"), str) or len(build["logSha256"]) != 64:
        raise ValueError("Native KV build is not successful and hash-bound")
    artifact = provenance.get("artifact", {})
    for field, value in inputs.get("artifact", {}).items():
        if artifact.get(field) != value:
            raise ValueError(f"Native KV artifact differs from frozen {field}")
    if artifact.get("file") != "native-kv.node" or artifact.get("platform") != "linux" or artifact.get("architecture") != "arm64" or artifact.get("napiVersion") != 4:
        raise ValueError("Native KV artifact is not the supported Linux ARM64 NAPI4 input")
    binary = runtime.regular(directory / "native-kv.node")
    runtime.inspect(binary)
    if runtime.digest(binary) != artifact.get("sha256"):
        raise ValueError("Native KV artifact hash differs from provenance")
    symbols = subprocess.run([str(runtime.READELF), "--wide", "--dyn-syms", str(binary)], capture_output=True, text=True, timeout=15,
                             env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"}, check=True).stdout
    if "napi_register_module_v1" not in symbols:
        raise ValueError("Native KV artifact has no actual Node-API module registration")


def verify_permissions(directory):
    directory = safe_directory(directory)
    manifest = read_json(directory / "manifest.json")
    if manifest.get("path") != "linux-arm64.node" or manifest.get("architecture") != "arm64":
        raise ValueError("Linux confinement manifest is invalid")
    binary = runtime.regular(directory / "linux-arm64.node")
    info = runtime.inspect(binary)
    if info["machine"] != "AArch64" or runtime.digest(binary) != manifest.get("sha256"):
        raise ValueError("Linux confinement artifact is invalid or corrupt")


def copy_file(source, destination, executable=False):
    runtime.regular(source)
    before = runtime.digest(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination, follow_symlinks=False)
    destination.chmod(0o755 if executable else 0o644)
    if before != runtime.digest(source) or before != runtime.digest(destination):
        raise ValueError(f"Input changed during packaging: {source}")


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    path.chmod(0o644)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--profile-manifest", type=Path, required=True)
    parser.add_argument("--native-kv", type=Path, required=True)
    parser.add_argument("--native-kv-inputs", type=Path, required=True)
    parser.add_argument("--native-kv-provenance-sha256", required=True)
    parser.add_argument("--permissions", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build = safe_directory(args.build_dir)
    if args.profile_manifest.absolute() != build / "profile-owner.json":
        raise ValueError("Compiler/profile ownership must belong to the selected build directory")
    profile = read_json(args.profile_manifest)
    if profile.get("schema") != "uos-gateway-profile-v1" or profile.get("profile") not in ("release", "clang-release") or profile.get("sanitizer") is not None or profile.get("source_root") != str(GATEWAY) or profile.get("workspace_root") != str(WORKSPACE):
        raise ValueError("The qualified compiler/profile identity is required")
    verify_native_kv(args.native_kv, args.native_kv_inputs, args.native_kv_provenance_sha256)
    verify_permissions(args.permissions)
    binaries = {"bin/uos-gateway": build / "uos_gateway", "bin/uos-kv-convert": build / "uos_kv_convert", "bin/uos-kv-proof": build / "uos_kv_proof"}
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
        # The pinned offline V8 helper imports this unchanged TypeScript graph
        # primitive; neither is application code executed at gateway startup.
        for name in ("v8-helper.mjs", "typed-value.ts"):
            copy_file(WORKSPACE / "src/kv-conversion" / name, stage / "src/kv-conversion" / name)
        for name in ("native-kv.node", "native-kv.provenance.json"):
            copy_file(args.native_kv / name, stage / "native/kv" / name)
        copy_file(args.native_kv_inputs, stage / "native/kv/inputs.json")
        for name in ("linux-arm64.node", "manifest.json"):
            copy_file(args.permissions / name, stage / "native/permissions" / name)
        for name in ("launch-native.sh", "uos-gateway-native.service", "README.md"):
            copy_file(GATEWAY / "ops" / name, stage / "ops" / name, name.endswith(".sh"))
        copy_file(GATEWAY / "docs/RUNTIME.md", stage / "README.md")
        copy_file(args.profile_manifest, stage / "build-profile.json")
        copy_file(GATEWAY / "runtime/launch-schema.json", stage / "runtime/launch-schema.json")
        copy_file(GATEWAY / "runtime/offline-schema.json", stage / "runtime/offline-schema.json")
        write_json(stage / "runtime-closure.json", document)
        write_json(stage / "operator-closure.json", operators)
        write_json(stage / "package.json", {"schema": "uos-native-package-v1", "nativeKvProvenanceSha256": args.native_kv_provenance_sha256,
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
