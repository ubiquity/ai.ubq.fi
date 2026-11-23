"""Finite, synthetic source-derived regex workloads; no runtime/environment input."""
import json
from pathlib import Path


def fixtures():
    rows = []
    for name, unit, count in (
        ("redact-word", "key", 70000), ("redact-dots", "key.", 50000),
        ("redact-dashes", "a-key-", 35000), ("redact-assignments", "a_key:", 35000),
        ("redact-brackets", "tokens: [", 25000), ("redact-quotes", "api_key: 'x ", 20000),
    ):
        rows.append(dict(name=name, operation="redact", input=unit * count, iterations=1))
    transcript = '\n'.join((
        'user: Inspect request routing and preserve full history.',
        'assistant: The parser preserves the original field order.',
        'tool: {"status":200,"items":12,"latency_ms":31}',
        'Authorization: Bearer syntheticcanary000000000000000000000',
        'client_secret: "synthetic alpha beta" progress continues',
        'tokens: [firstSynthetic, {nested: secondSynthetic}] done',
        'log: 2026-11-17T09:00:00Z one admission and one settlement',
    ))
    rows.append(dict(name="redact-transcript", operation="redact", input=(transcript + '\n') * 16, iterations=2))
    rows.append(dict(name="redact-unicode", operation="redact", input=("😀 日本語 café 🧪 client_secret: 'synthetic value' kept\n" * 64), iterations=2))
    key = r'''\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1(?:[.-]\w*){0,16}(["']?)\s*(?=[:=])'''
    for name, pattern, flags, operation, subject, iterations in (
        ("credential-key-fail", key, "gi", "matchAll", "key" * 70000, 1),
        ("credential-key-hit", key, "gi", "matchAll", transcript * 16, 5),
        ("bearer-header", r"^Bearer\s+(\S[^\n\r\u2028\u2029]*)$", "i", "exec", "Bearer " + "a" * 256, 200),
        ("sse-lines", r"\r\n|\r|\n", "", "split", ('data: {"delta":"content"}\r\n\r\n' * 512), 5),
        ("unicode-property", r"[\p{L}\p{N}_]", "gu", "matchAll", ("😀é日本_123🧪" * 256), 5),
        ("global-whitespace", r"\s+", "g", "replace", ("abc \t\n 日本😀 " * 512), 5),
        ("base64-failure", r"^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$", "", "test", "A" * 24576 + "!", 2),
        ("unicode-sticky", r"(?<unit>.)", "uy", "exec", "😀é_日本", 200),
        ("provider-model", r"^(?:rs|fc|msg|ctc)_[A-Za-z0-9_-]{1,240}$", "", "test", "msg_" + "a" * 240, 200),
        ("title-failure", r"<title[^>]*>([^<]*)</title>", "i", "exec", "<title>" + "a" * 24576, 2),
    ):
        rows.append(dict(name=name, operation=operation, pattern=pattern, flags=flags, input=subject, iterations=iterations))
    next(row for row in rows if row["name"] == "unicode-sticky")["lastIndex"] = 1
    return rows


if __name__ == "__main__":
    import sys
    Path(sys.argv[1]).write_text(json.dumps(fixtures(), ensure_ascii=False, separators=(",", ":")) + "\n")
