"""Compare wire observations and apply the expectations in cases.json."""

import json
import re

MISSING = object()
UUID = r"[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}"
OPAQUE_ID = re.compile(rf"{UUID}|(?:resp|msg|fc|call|rs|ctc|embjob)_(?:resp_)?(?:[0-9a-f]{{32}}|{UUID})(?:_[0-9]+)*", re.I)
ISO_DATE = re.compile(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?Z")
ID_FIELDS = {"id", "event_id", "request_id", "target_key_id", "call_id", "item_id", "response_id", "job_id"}
CLOCK_FIELDS = {
    "created", "created_at", "created_at_ms", "updated_at", "updated_at_ms", "completed_at", "completed_at_ms",
    "usage_reset_at_ms", "reset_at_ms", "expires_at_ms", "revoked_at_ms", "observed_at_ms", "checked_at_ms",
    "access_token_exp_ms", "occurred_at_ms", "paid_fallback_reset_at_ms", "last_checked_at_ms", "freshness_at_ms",
    "timestamp", "started_at_ms", "finished_at_ms", "at_ms", "window_reset_at_ms", "generated_at_ms",
}
HEADERS = {
    "content-type", "cache-control", "etag", "location", "vary", "set-cookie", "www-authenticate",
    "content-security-policy", "x-content-type-options", "referrer-policy", "x-frame-options",
    "x-uos-request-id", "x-uos-provider-request-id", "x-uos-upstream", "x-uos-attempted-providers",
    "x-uos-router-revision", "x-uos-warning", "x-uos-git-sha", "x-uos-deployment-id", "x-uos-cache",
    "x-uos-idempotency-replayed", "retry-after", "ratelimit", "ratelimit-policy",
    "ratelimit-limit", "ratelimit-remaining", "ratelimit-reset", "x-codex-limit-name",
    "x-codex-primary-used-percent", "x-ratelimit-limit-requests", "x-ratelimit-limit-tokens",
    "x-ratelimit-remaining-requests", "x-ratelimit-remaining-tokens", "x-ratelimit-reset-requests",
    "x-ratelimit-reset-tokens", "upgrade", "connection", "sec-websocket-protocol", "sec-websocket-accept",
}
CORS_LISTS = {"access-control-allow-headers", "access-control-allow-methods", "access-control-expose-headers", "vary"}


def lookup(value, pointer):
    for part in pointer.split("/")[1:]:
        key = part.replace("~1", "/").replace("~0", "~")
        if isinstance(value, list) and key.isdecimal():
            value = value[int(key)] if int(key) < len(value) else MISSING
        elif isinstance(value, dict):
            value = value.get(key, MISSING)
        else:
            return MISSING
    return value


def printable(value):
    return "<absent>" if value is MISSING else value


def equal(a, b):
    if type(a) is not type(b) and (type(a) not in (int, float) or type(b) not in (int, float)):
        return False
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(equal(a[key], b[key]) for key in a)
    if isinstance(a, list):
        return len(a) == len(b) and all(equal(x, y) for x, y in zip(a, b))
    return a == b


def delta(path, bun, native, classification="FUNCTIONAL"):
    return {"classification": classification, "path": path, "bun": printable(bun), "native": printable(native)}


def diff(left, right, path=""):
    if isinstance(left, dict) and isinstance(right, dict):
        for key in sorted(left.keys() | right.keys()):
            yield from diff(left.get(key, MISSING), right.get(key, MISSING), path + "/" + key.replace("~", "~0").replace("/", "~1"))
    elif isinstance(left, list) and isinstance(right, list):
        if len(left) != len(right):
            yield delta(path + "/length", len(left), len(right))
        for index, (a, b) in enumerate(zip(left, right)):
            yield from diff(a, b, f"{path}/{index}")
    elif not equal(left, right):
        cosmetic = isinstance(left, str) and isinstance(right, str) and path.endswith("/error/message")
        yield delta(path, left, right, "COSMETIC" if cosmetic else "FUNCTIONAL")


def normalize(value, ids, unordered, path=""):
    if isinstance(value, list):
        if path in unordered and all(isinstance(item, dict) and unordered[path] in item for item in value):
            value = sorted(value, key=lambda item: item[unordered[path]])
        return [normalize(item, ids, unordered, f"{path}/{index}") for index, item in enumerate(value)]
    if isinstance(value, dict):
        return {key: normalize(value[key], ids, unordered, f"{path}/{key}") for key in sorted(value)}
    field = path.rsplit("/", 1)[-1]
    if field in ID_FIELDS and isinstance(value, str) and OPAQUE_ID.fullmatch(value):
        prefix = value.partition("_")[0] if "_" in value else "uuid"
        return ids.setdefault(value, f"<{prefix}:{len(ids) + 1}>")
    if field in CLOCK_FIELDS:
        if type(value) in (int, float) and 1_000_000_000 <= value <= 4_102_444_800_000:
            return "<timestamp-ms>" if value > 4_102_444_800 else "<timestamp-s>"
        if isinstance(value, str) and ISO_DATE.fullmatch(value):
            return "<timestamp-iso>"
    if path.endswith("/error/message") and isinstance(value, str):
        return ISO_DATE.sub("<timestamp-iso>", value)
    return value


def comparable_headers(headers):
    return {name: values for name, values in headers.items() if name in HEADERS or name.startswith(("access-control-", "x-ratelimit-"))}


def content_type_semantics(value):
    media_type, separator, parameters = value.partition(";")
    if not separator or re.fullmatch(r"\s*charset\s*=\s*(?:utf-8|\"utf-8\")\s*", parameters, re.I):
        return media_type.strip().lower()
    return value


def header_value(name, values):
    if name in CORS_LISTS:
        return sorted(token.strip().lower() for value in values for token in value.split(","))
    if name == "content-type":
        return [content_type_semantics(value) for value in values]
    if name == "x-uos-request-id":
        return ["<uuid>" if re.fullmatch(UUID, value, re.I) else value for value in values]
    if name == "sec-websocket-accept":
        return ["<validated WebSocket digest>"]  # The client verifies the digest against its own nonce.
    if name in ("upgrade", "connection"):
        return [value.lower() for value in values]
    return values


def close_reset_values(name, a, b):
    if len(a) != 1 or len(b) != 1:
        return False
    if name in ("ratelimit-reset", "retry-after") and a[0].isdigit() and b[0].isdigit():
        return min(int(a[0]), int(b[0])) >= 1 and abs(int(a[0]) - int(b[0])) <= 2
    if name == "ratelimit":
        fields = [re.fullmatch(r'("api-key";r=\d+);t=(\d+)', item[0]) for item in (a, b)]
        return all(fields) and fields[0][1] == fields[1][1] and abs(int(fields[0][2]) - int(fields[1][2])) <= 2
    return False


def header_diffs(left, right, status):
    a, b = map(comparable_headers, (left, right))
    for name in sorted(a.keys() | b.keys()):
        if name == "connection" and status != 101:
            continue
        x, y = a.get(name, MISSING), b.get(name, MISSING)
        if x is MISSING or y is MISSING:
            yield delta("/headers/" + name, x, y)
        elif header_value(name, x) != header_value(name, y) and not close_reset_values(name, x, y):
            yield delta("/headers/" + name, x, y)
        elif x != y and (name in ("content-type", "upgrade") or name in CORS_LISTS):
            yield delta("/headers/" + name, x, y, "COSMETIC")


def release_checks(spec, observation, commit):
    """Expected identity of the package or the detached, unstamped Bun source."""
    deployment = "unknown" if commit == "unknown" else f"vps-{commit}"
    status = observation["status"]
    if status is None:
        return {}
    checks = {
        "/headers/" + name: [target]
        for name, target in (("x-uos-git-sha", commit), ("x-uos-deployment-id", deployment))
        if status != 101 or name in observation["headers"]
    }
    route = spec["path"].partition("?")[0]
    if status == 200 and spec.get("method", "GET") == "GET":
        if route == "/health":
            checks.update({"/body/release/git_sha": commit, "/body/release/deployment_id": deployment})
        if route == "/admin/api-keys/changes":
            rows = lookup(observation, "/body/data")
            if isinstance(rows, list):
                checks.update({f"/body/data/{index}/release": commit for index in range(len(rows))})
    return checks


def expectations(spec, observation, release_fields=None):
    errors = []
    if observation.get("wire_error"):
        errors.append({"path": "/wire_error", "expected": "complete wire exchange", "actual": observation["wire_error"]})
    if observation["mock_errors"] or observation["benchmark_failures"]:
        errors.append({"path": "/mock", "expected": "no rejected fixture traffic",
                       "actual": [observation["mock_errors"], observation["benchmark_failures"]]})
    expected = spec["expect"]
    checks = {"/status": expected["status"], **expected.get("values", {})}
    observation["routes"] = [call["route"] for call in observation["dispatches"]]
    if "routes" in expected:
        checks["/routes"] = expected["routes"]
    if spec.get("progressive"):
        checks["/progressive"] = True
    for path, target in [*checks.items(), *(release_fields or {}).items()]:
        actual = lookup(observation, path)
        if not equal(actual, target):
            errors.append({"path": path, "expected": target, "actual": printable(actual)})
    status = observation["status"]
    media_type = expected.get("media_type", None if status in (101, 204) else "application/json")
    headers = observation["headers"]
    if status is not None:
        observed_types = header_value("content-type", headers.get("content-type", []))
        if observed_types != ([media_type] if media_type is not None else []):
            errors.append({"path": "/headers/content-type", "expected": media_type, "actual": headers.get("content-type", [])})
    if status is not None and status >= 400 and spec.get("transport") != "ws":
        for path in ("/body/error/code", "/body/error/type"):
            if not isinstance(lookup(observation, path), str):
                errors.append({"path": path, "expected": "error identity string", "actual": printable(lookup(observation, path))})
    for path in expected.get("absent", []):
        actual = lookup(observation, path)
        if actual is not MISSING:
            errors.append({"path": path, "expected": "<absent>", "actual": actual})
    for name in expected.get("positive_headers", []):
        values = observation["headers"].get(name, [])
        if len(values) != 1 or not values[0].isdigit() or int(values[0]) < 1:
            errors.append({"path": "/headers/" + name, "expected": "one positive integer", "actual": values})
    for path, length in expected.get("lengths", {}).items():
        actual = lookup(observation, path)
        count = len(actual) if isinstance(actual, (list, str)) else None
        if count != length:
            errors.append({"path": path + "/length", "expected": length, "actual": count})
    if spec.get("request_id", status != 101) and status is not None:
        values = headers.get("x-uos-request-id", [])
        if len(values) != 1 or not re.fullmatch(UUID, values[0], re.I):
            errors.append({"path": "/headers/x-uos-request-id", "expected": "one UUID", "actual": values})
    return errors


def compare(spec, bun, native, ids, *, release_fields=None):
    changes = list(diff(bun["status"], native["status"], "/status"))
    changes.extend(header_diffs(bun["headers"], native["headers"], bun["status"]))
    unordered = spec.get("unordered", {})
    for path, key in unordered.items():
        left, right = lookup(bun, path), lookup(native, path)
        if isinstance(left, list) and isinstance(right, list):
            a, b = ([row.get(key) for row in rows] for rows in (left, right))
            if a != b and sorted(map(str, a)) == sorted(map(str, b)):
                changes.append(delta(path + "/order", a, b, "COSMETIC"))
    for key in ("body", "events", "event_types", "terminal", "routes", "wire_error", "telemetry"):
        left = normalize(bun.get(key, MISSING), dict(ids["bun"]), unordered, "/" + key)
        right = normalize(native.get(key, MISSING), dict(ids["native"]), unordered, "/" + key)
        changes.extend(diff(left, right, "/" + key))
    if release_fields:
        changes = [
            item for item in changes
            if not all(equal(item[name], release_fields[name].get(item["path"], MISSING)) for name in ("bun", "native"))
        ]
    return sorted(changes, key=lambda item: item["classification"] != "FUNCTIONAL")


def redact(value, secrets):
    if isinstance(value, str):
        for secret in secrets:
            value = value.replace(secret, "<synthetic credential>")
    elif isinstance(value, list):
        return [redact(item, secrets) for item in value]
    elif isinstance(value, dict):
        return {key: redact(item, secrets) for key, item in value.items()}
    return value


def cell(value):
    encoded = json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    return encoded.replace("|", "&#124;").replace("`", "&#96;")


def markdown(receipt):
    lines = ["| Case | Bun | Native | Result |", "| --- | ---: | ---: | --- |"]
    for row in receipt["cases"]:
        statuses = [row["observations"].get(runtime, {}).get("status") for runtime in ("bun", "native")]
        descriptions = []
        for item in row["differences"]:
            descriptions.append(f'{item["classification"]} `{item["path"]}` {cell(item["bun"])} → {cell(item["native"])}')
        expected = row["expectation_failures"]
        affected = {"both": expected["bun"]} if expected["bun"] == expected["native"] else expected
        for runtime, errors in affected.items():
            for item in errors:
                descriptions.append(f'FUNCTIONAL ({runtime}) `{item["path"]}` expected {cell(item["expected"])} and observed {cell(item["actual"])}')
        detail = "<br>".join(descriptions) or "Match"
        bun, native = (status if status is not None else "—" for status in statuses)
        lines.append(f'| {row["id"]} | {bun} | {native} | {detail} |')
    return "\n".join(lines) + "\n"
