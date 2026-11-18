#!/usr/bin/python3
"""Assemble the relocatable payload, without choosing a runtime host closure."""

import json
from pathlib import Path
import shutil
import subprocess
import sys

GATEWAY = Path("/source/cpp/gateway")
ROOT = GATEWAY.parents[1]
OUTPUT = Path("/release")
sys.path.insert(0, str(GATEWAY / "tools"))
import package as native

# Reuse the package verifier and closure reader from the source being built.
sys.modules["runtime_manifest"] = native.runtime
from seal import EXECUTABLES, manifest, require_portable


def main(commit, tree):
    payload = OUTPUT / "payload"
    payload.mkdir(parents=True)
    kv = Path("/native/kv")
    permissions = Path("/native/permissions")
    provenance = Path("/native/kv-provenance-sha256").read_text().strip()
    inputs = ROOT / "tools/native-kv/inputs.json"
    native.verify_native_kv(kv, inputs, provenance)
    native.verify_permissions(permissions)
    for name in EXECUTABLES:
        binary = Path("/build") / Path(name).name.replace("-", "_")
        target = payload / name
        native.copy_file(binary, target, executable=True)
        subprocess.run(["strip", "--strip-unneeded", str(target)], check=True)
        require_portable(native.runtime.inspect(target), name)

    for asset in sorted((ROOT / "static").rglob("*")):
        if asset.is_symlink() or not (asset.is_file() or asset.is_dir()):
            raise ValueError(f"Unsafe static asset: {asset}")
        if asset.is_file():
            native.copy_file(asset, payload / asset.relative_to(ROOT))
    for name in ("v8-helper.mjs", "typed-value.ts"):
        where = Path("src/kv-conversion") / name
        native.copy_file(ROOT / where, payload / where)
    for name in ("launch-schema.json", "offline-schema.json"):
        native.copy_file(GATEWAY / "runtime" / name, payload / "runtime" / name)
    native.copy_file(GATEWAY / "ops/launch-native.sh", payload / "ops/launch-native.sh", executable=True)
    for name in ("native-kv.node", "native-kv.provenance.json"):
        native.copy_file(kv / name, payload / "native/kv" / name)
    native.copy_file(inputs, payload / "native/kv/inputs.json")
    for name in ("linux-arm64.node", "manifest.json"):
        native.copy_file(permissions / name, payload / "native/permissions" / name)

    native.copy_vendor_licenses(payload)
    native.copy_file(kv / "LICENSE", payload / "licenses/native-kv/LICENSE")
    shutil.copytree("/opt/uos/licenses", payload / "licenses", dirs_exist_ok=True)
    packages = ["libc6", "clang-21", "libstdc++-12-dev", "libgcc-12-dev", "libssl-dev", "zlib1g-dev", "libbrotli-dev", "libzstd-dev", "mold"]
    profile = {
        "schema": "uos-gateway-portable-build-v1", "gitCommit": commit, "gitTree": tree,
        "target": "aarch64-linux-gnu", "isa": "armv8-a", "glibcCeiling": "2.36",
        "compiler": subprocess.check_output(["clang++-21", "--version"], text=True).splitlines()[0],
        "debianPackages": dict(line.split("\t", 1) for line in subprocess.check_output(
            ["dpkg-query", "-W", "-f=${Package}\t${Version}\n", *packages], text=True).splitlines()),
        "static": {"Boost.JSON/Container": "1.92.0", "SQLite": "3.54.0", "ICU": "78.3", "llhttp": "9.4.4",
                   "simdjson": "5.2.0", "simdutf": "9.4.0"},
        "linkFlags": ["-static-libstdc++", "-static-libgcc"],
    }
    native.write_json(payload / "build-profile.json", profile)
    native.write_json(payload / "package.json", {"schema": "uos-native-package-v1", "platform": "linux", "architecture": "arm64",
                      "nativeKvProvenanceSha256": provenance, "profileSha256": native.runtime.digest(payload / "build-profile.json")})
    (payload / "manifest.sha256").write_text(manifest(payload))
    native.copy_file(GATEWAY / "docs/RASPBERRY-PI.md", OUTPUT / "README.md")
    native.copy_file(GATEWAY / "tools/runtime-manifest.py", OUTPUT / "runtime_manifest.py")
    native.copy_file(GATEWAY / "packaging/portable/seal.py", OUTPUT / "seal.py")


if __name__ == "__main__":
    main(*sys.argv[1:])
