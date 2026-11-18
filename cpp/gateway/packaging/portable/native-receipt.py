#!/usr/bin/python3
"""Bind the portable KV artifact to its reproducible build command log."""

import hashlib
import json
from pathlib import Path
import sys

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
