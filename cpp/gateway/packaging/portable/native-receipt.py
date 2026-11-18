#!/usr/bin/python3
"""Bind the portable KV artifact to its reproducible build command log."""

import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import tomllib

work = Path(sys.argv[1])
(log,) = (work / "kv-build").glob("*/build.log")
native = Path("/native")
proof = native / "kv/native-kv.provenance.json"
binding = native / "kv-provenance-sha256"


def digest(content):
    return hashlib.sha256(content).hexdigest()


original = proof.read_bytes()
raw_log = log.read_bytes()
receipt = json.loads(original)
if digest(original) != binding.read_text().strip() or digest(raw_log) != receipt["build"]["logSha256"]:
    raise ValueError("Native KV build receipt does not match its original log")

# Cargo progress order and durations vary. Preserve the actual command and exit records.
records = [json.loads(line.replace(str(log.parent), "/build/native-kv"))
           for line in raw_log.decode().splitlines() if line.startswith(('{"command":', '{"exitCode":'))]
commands = records[::2]
statuses = records[1::2]
if (not commands or len(commands) != len(statuses) or any(set(row) != {"command", "cwd"} for row in commands)
        or any(row != {"exitCode": 0} for row in statuses)):
    raise ValueError("Native KV command log is incomplete")
command_log = "".join(json.dumps(row, sort_keys=True) + "\n" for row in records).encode()
(native / "kv/build-commands.jsonl").write_bytes(command_log)
receipt["build"]["logSha256"] = digest(command_log)
final = (json.dumps(receipt, indent=2) + "\n").encode()
proof.write_bytes(final)
binding.write_text(digest(final) + "\n")

# Cargo has verified these source packages against the frozen lock before compiling.
licenses = native / "kv-licenses"
rust = work / "rust-1.98.1-aarch64-unknown-linux-gnu"
toolchain = licenses / "rust"
toolchain.mkdir(parents=True)
for name in ("COPYRIGHT", "LICENSE-APACHE", "LICENSE-MIT"):
    shutil.copyfile(rust / name, toolchain / name)
shutil.copyfile(rust / "rustc/share/doc/rust/COPYRIGHT-library.html", toolchain / "COPYRIGHT-library.html")

inventory = {}
notice = re.compile(r"^(?:licen[cs]e|copying|copyright|notice|unlicense)(?:$|[-_.])", re.IGNORECASE)
workspace_licenses = {
    "https://github.com/denoland/deno_error": "deno-error.LICENSE",
    "https://github.com/napi-rs/napi-rs": "napi-rs.LICENSE",
}
for manifest in sorted((log.parent / "cargo-home/registry/src").glob("*/*/Cargo.toml")):
    crate = manifest.parent
    package = tomllib.loads(manifest.read_text())["package"]
    files = {p for p in crate.rglob("*") if notice.match(p.name) and p.is_file()}
    if package.get("license-file"):
        files.add(crate / package["license-file"])
    destination = licenses / "registry" / crate.name
    copied = []
    inventory[crate.name] = {"license": package.get("license"), "notices": copied}
    if not files:
        upstream = workspace_licenses.get(package.get("repository"))
        if not upstream or package.get("license") != "MIT":
            raise ValueError(f"No license notice for {crate.name}")
        destination.mkdir(parents=True)
        shutil.copyfile(Path("/usr/local/share/uos-crate-licenses") / upstream, destination / "LICENSE")
        copied.append("LICENSE")
    for source in sorted(files):
        if not source.resolve(strict=True).is_relative_to(crate):
            raise ValueError(f"License file escapes registry package: {source}")
        relative = source.relative_to(crate)
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        copied.append(relative.as_posix())
(licenses / "registry.json").write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n")
