#!/usr/bin/python3
"""Assemble the relocatable payload, without choosing a runtime host closure."""

from datetime import datetime, timezone
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


def main(commit, tree, epoch):
    if len(commit) != 40 or any(character not in "0123456789abcdef" for character in commit):
        raise ValueError("The portable package requires the full lowercase Git commit")
    if len(tree) != 40 or any(character not in "0123456789abcdef" for character in tree):
        raise ValueError("The portable package requires the full lowercase Git tree")
    payload = OUTPUT / "payload"
    payload.mkdir(parents=True)
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
    for name in ("favicon.png", "favicon-32.png"):
        native.copy_file(ROOT / name, payload / name)
    native.copy_file(GATEWAY / "runtime/launch-schema.json", payload / "runtime/launch-schema.json")
    native.copy_file(GATEWAY / "ops/launch-native.sh", payload / "ops/launch-native.sh", executable=True)

    vendored_versions = native.copy_vendor_licenses(payload)
    shutil.copytree("/opt/uos/licenses", payload / "licenses", dirs_exist_ok=True)
    native.copy_file(GATEWAY / "packaging/SQLITE-LICENSE", payload / "licenses/sqlite/LICENSE")
    native.copy_file(GATEWAY / "packaging/DENOKV-LICENSE", payload / "licenses/denokv/LICENSE")
    native.copy_file(Path("/usr/share/common-licenses/GPL-3"), payload / "licenses/gcc/GPL-3")
    packages = ["libc6", "clang-21", "libstdc++-12-dev", "libgcc-12-dev", "libssl-dev", "zlib1g-dev", "libbrotli-dev", "libzstd-dev", "mold"]
    profile = {
        "schema": "uos-gateway-portable-build-v1", "gitCommit": commit, "gitTree": tree,
        "buildDate": datetime.fromtimestamp(int(epoch), timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "target": "aarch64-linux-gnu", "isa": "armv8-a", "glibcCeiling": "2.36",
        "compiler": subprocess.check_output(["clang++-21", "--version"], text=True).splitlines()[0],
        "debianPackages": dict(line.split("\t", 1) for line in subprocess.check_output(
            ["dpkg-query", "-W", "-f=${Package}\t${Version}\n", *packages], text=True).splitlines()),
        "static": {"Boost": "1.92.0", "SQLite": "3.54.0", "ICU": "78.3", **vendored_versions},
        "linkFlags": ["-static-libstdc++", "-static-libgcc"],
    }
    native.write_json(payload / "build-profile.json", profile)
    native.write_json(OUTPUT / "VERSION", profile)
    native.write_json(payload / "package-info.json", {"schema": "uos-native-package-v1", "platform": "linux", "architecture": "arm64",
                      "profileSha256": native.runtime.digest(payload / "build-profile.json")})
    (payload / "manifest.sha256").write_text(manifest(payload))
    native.copy_file(GATEWAY / "docs/RASPBERRY-PI.md", OUTPUT / "README.md")
    native.copy_file(GATEWAY / "packaging/portable/INSTALL.md", OUTPUT / "INSTALL.md")
    native.copy_file(GATEWAY / "tools/runtime-manifest.py", OUTPUT / "runtime_manifest.py")
    native.copy_file(GATEWAY / "packaging/portable/seal.py", OUTPUT / "seal.py")


if __name__ == "__main__":
    main(*sys.argv[1:])
