#!/usr/bin/python3
"""Finite serialized hazard-surface baseline; every TU retains an independent receipt."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

GATEWAY = Path(__file__).resolve().parents[1]
EVIDENCE = Path("/tmp/claude-1002/prod/analysis_fuzz")
SOURCES = ["src/rt/url.cpp", "src/rt/regex_compile.cpp", "src/rt/regex_match.cpp",
           "src/rt/kv_jsc_deserialize.cpp", "src/rt/fetch_transport_http.cpp", "src/rt/websocket_frame.cpp",
           "src/responses_stream.cpp", "src/runtime/http.cpp", "src/auth/passkeys.cpp", "src/rt/async.cpp"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True)
    parser.add_argument("--inventory", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--source", action="append")
    args = parser.parse_args()
    rows = []
    started = time.monotonic()
    for index, source in enumerate(args.source or SOURCES):
        label = args.label + "-" + str(index) + "-" + Path(source).stem
        command = ["/usr/bin/python3", "-B", str(GATEWAY / "tools/analyze.py"), "analyze", "--label", label,
                   "--source", source, "--database", args.database, "--inventory", args.inventory, "--timeout", "180"]
        print("analysis-start " + source + " label=" + label, flush=True)
        with (EVIDENCE / (label + ".output.json")).open("x") as output:
            child = subprocess.run(command, cwd=GATEWAY, stdout=output, stderr=subprocess.STDOUT)
        result = EVIDENCE / (label + ".json")
        record = json.loads(result.read_text()) if result.exists() else {}
        rows.append({"source": source, "code": child.returncode, "result": str(result),
                     "diagnostics": len(record.get("diagnostics", [])), "classification": record.get("classification", "tool-failure")})
        (EVIDENCE / (args.label + ".progress.json")).write_text(json.dumps(rows, indent=2) + "\n")
        print("analysis-end " + source + " code=" + str(child.returncode) + " diagnostics=" + str(rows[-1]["diagnostics"]), flush=True)
        if time.monotonic() - started > 2400:
            raise RuntimeError("Finite baseline exceeded its 40-minute aggregate budget")
    (EVIDENCE / (args.label + ".json")).write_text(json.dumps({"results": rows, "duration_s": time.monotonic() - started}, indent=2) + "\n")
    return 1 if any(row["code"] for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
