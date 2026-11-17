"""Normalize only fresh opaque IDs and clocks; retain wire schemas, events and usage."""

import json
import re

UUID = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", re.I)
CLOCK_FIELDS = {"created_at", "created_at_ms", "updated_at_ms", "usage_reset_at_ms", "window_reset_at_ms", "fetched_at_ms", "access_token_expires_at_ms"}
TERMINAL_FIELDS = ("route", "status", "stream", "provider", "model", "delivery_outcome", "usage_observed", "usage_telemetry_status",
                   "input_tokens", "output_tokens", "total_tokens", "cached_input_tokens", "cache_write_input_tokens", "reasoning",
                   "output_token_allowance", "stream_terminal_type", "failure_kind", "admission_stage", "semantic_output_observed",
                   "upstream_event_kinds", "response_created_observed", "synthetic_terminal_type", "attempted_providers")


def normalize(value, key=None, identities=None):
    if identities is None:
        identities = {}
    if isinstance(value, dict):
        return {name: normalize(item, name, identities) for name, item in value.items()}
    if isinstance(value, list):
        return [normalize(item, key, identities) for item in value]
    if key in CLOCK_FIELDS and isinstance(value, (int, float)) and value > 1000000000:
        return "[clock]"
    if isinstance(value, str) and (UUID.fullmatch(value) or value.startswith(("resp_", "msg_"))):
        if value not in identities:
            identities[value] = "[opaque-" + str(len(identities) + 1) + "]"
        return identities[value]
    return value


def projection(trial):
    result = {"captures": {}, "terminals": {}, "durability": trial["durability"]}
    for name, capture in trial["captures"].items():
        if name in ("health", "health_head"):
            result["captures"][name] = {"status": capture["status"], "body_bytes": capture["body_bytes"] if name == "health_head" else None}
            continue
        if name.startswith("ws_"):
            result["captures"][name] = {key: capture[key] for key in ("status", "opcode", "sent", "received") if key in capture}
            continue
        record = {"status": capture["status"]}
        if "json" in capture:
            record["json"] = capture["json"]
        if capture.get("events"):
            record["events"] = [event["data"] for event in capture["events"]]
        if "text" in capture and capture["text"]:
            record["text"] = capture["text"]
        result["captures"][name] = record
        request_id = capture.get("headers", {}).get("x-uos-request-id")
        rows = [entry["value"] for entry in trial.get("terminals", []) if entry["value"].get("request_id") == request_id]
        if rows:
            result["terminals"][name] = [{field: row[field] for field in TERMINAL_FIELDS if field in row} for row in rows]
    return normalize(result)


def differences(left, right, path="$"):
    result = []
    if type(left) is not type(right):
        return [{"path": path, "native": left, "bun": right}]
    if isinstance(left, dict):
        if list(left) != list(right):
            result.append({"path": path + ".[field_order]", "native": list(left), "bun": list(right)})
        for key in left.keys() | right.keys():
            if key not in left or key not in right:
                result.append({"path": path + "." + key, "native": left.get(key), "bun": right.get(key), "absent_field": True})
            else:
                result.extend(differences(left[key], right[key], path + "." + key))
    elif isinstance(left, list):
        if len(left) != len(right):
            result.append({"path": path + ".[length]", "native": len(left), "bun": len(right)})
        for index, (a, b) in enumerate(zip(left, right)):
            result.extend(differences(a, b, path + "[" + str(index) + "]"))
    elif left != right:
        result.append({"path": path, "native": left, "bun": right})
    return result


def compare(trials):
    pairs = {}
    for trial in trials:
        pairs.setdefault(trial["trial"], {})[trial["runtime"]] = trial
    result = []
    for number, pair in pairs.items():
        if "native" not in pair or "bun" not in pair:
            continue
        native, bun = projection(pair["native"]), projection(pair["bun"])
        found = differences(native, bun)
        result.append({"trial": number, "equal": not found, "differences": found, "native_projection": native, "bun_projection": bun})
    return result
