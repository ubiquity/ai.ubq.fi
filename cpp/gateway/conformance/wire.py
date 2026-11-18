"""Capture public HTTP, SSE and WebSocket observations."""

from hashlib import sha256
from http import client as http
import json
from pathlib import Path
from string import Template
import time

from client import endpoint, websocket
from fixtures import SCENARIOS, encode

MAX_BODY = 8 * 1024 * 1024
TIMEOUT = 6


def reject_non_json_number(value):
    raise ValueError(f"Invalid JSON number {value}")


def expand(value, variables):
    if isinstance(value, str):
        return Template(value).substitute(variables)
    if isinstance(value, list):
        return [expand(item, variables) for item in value]
    if isinstance(value, dict):
        return {key: expand(item, variables) for key, item in value.items()}
    return value


def payload(spec):
    if "fixture" in spec:
        return SCENARIOS[spec["fixture"]].body, "application/json"
    if "json" in spec:
        return encode(spec["json"]), "application/json"
    if "raw" in spec:
        return spec["raw"].encode(), "application/json"
    if "ndjson" in spec:
        return Path(__file__).with_name(spec["ndjson"]).read_bytes(), "application/x-ndjson"
    if "multipart" in spec:
        boundary = "uos-conformance"
        parts = []
        for name, value in spec["multipart"].items():
            prefix = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n'.encode()
            parts.append(prefix + (value.encode() if isinstance(value, str) else encode(value)) + b"\r\n")
        return b"".join(parts) + f"--{boundary}--\r\n".encode(), "multipart/form-data; boundary=" + boundary
    return None, None


def headers_from(values):
    result = {}
    for key, value in values:
        name = key.lower()
        result.setdefault(name, []).append(value)
    return result


def event_kind(event):
    value = event["data"]
    if value == "[DONE]":
        return "[DONE]"
    if not isinstance(value, dict):
        return "non-object"
    if "type" in value:
        return value["type"]
    if "error" in value:
        return "error"
    if value.get("usage") is not None and not value.get("choices"):
        return "usage"
    choice = (value.get("choices") or [{}])[0]
    if choice.get("finish_reason"):
        return "finish:" + choice["finish_reason"]
    delta = choice.get("delta", {})
    if delta.get("tool_calls"):
        return "chat.tool_calls"
    if delta.get("content"):
        return "chat.content"
    if delta.get("role"):
        return "chat.role"
    return "chat.chunk"


def read_sse(response, result, on_event):
    events = result["events"] = []
    data, fields, size = [], {}, 0
    for raw in iter(lambda: response.readline(MAX_BODY + 1), b""):
        size += len(raw)
        if size > MAX_BODY:
            raise ValueError("SSE exceeded the capture bound")
        line = raw.rstrip(b"\r\n").decode()
        if not line:
            if data:
                joined = "\n".join(data)
                value = joined if joined == "[DONE]" else json.loads(joined, parse_constant=reject_non_json_number)
                event = {**fields, "data": value}
                events.append(event)
                if on_event is not None:
                    on_event(event)
            data, fields = [], {}
            continue
        name, _, value = line.partition(":")
        value = value.removeprefix(" ")
        if name == "data":
            data.append(value)
        elif name in ("event", "id", "retry"):
            fields[name] = value
    if data:
        raise ValueError("SSE ended before the event separator")
    result["event_types"] = [event_kind(event) for event in events]
    result["terminal"] = result["event_types"][-1] if events else None


def request(origin, spec, credentials, on_event=None):
    started = time.monotonic()
    result = {"status": None, "headers": {}, "body": None}
    auth = spec.get("auth", "primary")
    token = credentials.get(auth)
    try:
        if spec.get("transport") == "ws":
            capture = websocket(origin, spec["path"], token=token, relay=spec.get("relay", False))
            values = [line.split(":", 1) for line in capture["head"][1:] if ":" in line]
            result.update(status=capture["status"], headers=headers_from((key, value.strip()) for key, value in values))
            if "received" in capture:
                result["body"] = {"opcode": capture["opcode"], "echo": capture["received"]}
            return result
        body, content_type = payload(spec)
        padded = spec.get("pad_to_bytes")
        if padded is not None:
            if body is None or not len(body) < padded <= 33 * 1024 * 1024:
                raise ValueError("Invalid padded request size")
            body = body.ljust(padded, b" ")
        headers = {"Connection": "close"}
        if token:
            headers["Authorization"] = "Bearer " + token
        if content_type is not None:
            headers["Content-Type"] = content_type
        headers.update(spec.get("headers", {}))
        client = http.HTTPConnection(*endpoint(origin), timeout=TIMEOUT)
        try:
            try:
                client.request(spec.get("method", "GET"), spec["path"], body, headers)
            except BrokenPipeError:
                # A server can deliver a refusal before reading the rest of an oversized upload.
                if padded is None:
                    raise
            response = client.getresponse()
            result.update(status=response.status, headers=headers_from(response.getheaders()))
            media_type = response.getheader("content-type", "").partition(";")[0].lower()
            if media_type == "text/event-stream":
                read_sse(response, result, on_event)
            else:
                raw = response.read(MAX_BODY + 1)
                if len(raw) > MAX_BODY:
                    raise ValueError("HTTP response exceeded the capture bound")
                if raw:
                    if media_type.endswith(("/json", "+json")):
                        result["body"] = json.loads(raw, parse_constant=reject_non_json_number)
                    elif len(raw) <= 65536:
                        result["body"] = raw.decode("utf-8", errors="replace")
                    else:
                        result["body"] = {"bytes": len(raw), "sha256": sha256(raw).hexdigest()}
        finally:
            client.close()
    except (OSError, http.HTTPException, RuntimeError, ValueError) as error:
        result["wire_error"] = f"{type(error).__name__}: {error}"
    finally:
        result["elapsed_ms"] = round((time.monotonic() - started) * 1000, 3)
    return result
