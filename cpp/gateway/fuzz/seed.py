#!/usr/bin/python3
"""Materialize small public/synthetic parser seeds and preserve their origins."""
import base64
import hashlib
import json
from pathlib import Path
import re

GATEWAY = Path(__file__).resolve().parents[1]
ROOT = GATEWAY / "fuzz/corpus"
MANIFEST = []


def seed(target, data, origin):
    if isinstance(data, str):
        data = data.encode()
    name = hashlib.sha256(data).hexdigest()
    path = ROOT / target / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() != data:
        raise RuntimeError("Seed hash collision")
    path.write_bytes(data)
    MANIFEST.append({"target": target, "path": str(path.relative_to(GATEWAY)), "sha256": name,
                     "bytes": len(data), "origin": origin})


def main():
    for data in ('null', 'true', '-0', '9007199254740993', '{"a":[1,2],"__proto__":null}',
                 '"\\ud800"', '[1,', '{', '[' * 64 + '0' + ']' * 64):
        seed("json", data, "synthetic JSON edge forms matching tests/rt/json_test.cpp")
    for data in ("https://example.com/a?x=1#frag", "http://[::1]:80/", "../a%2Fb?x=a+b", "file:///tmp/fixture",
                 "https://faß.de/", "http://0x7f.1/", "http://[::ffff:127.0.0.1]/", "https://例え.テスト/", "http://%zz/"):
        seed("url", data, "public/synthetic WHATWG vectors from tests/rt/url_test.cpp")
    for pattern, subject, flags in (("(a+)+$", "aaaaaaaa!", 0), ("(?<x>a)|b", "ab", 1), ("[a-z]+", "ABCabc", 2),
                                    ("^.*$", "line\nnext", 4), ("\\p{Letter}+", "λabc", 1), ("[", "x", 0)):
        encoded = pattern.encode()
        seed("regex", bytes((len(encoded), flags)) + encoded + subject.encode(), "synthetic patterns represented in tests/rt/regex_test.cpp")
    for data in (b'data: {"type":"response.completed"}\n\n', b': comment\r\ndata: [DONE]\r\n\r\n',
                 b'data: {"x":1}\r\r', b'event: message\ndata: {"a":\ndata: 1}\n\n', b'\xef\xbb\xbfdata: {}\n\n'):
        seed("sse", data, "public synthetic SSE framing forms")
    vectors = GATEWAY / "tests/rt/kv_jsc_deserialize_vectors.inc"
    for value in re.findall(r'vectors\.push_back\(\{"[^"\n]*", "([0-9a-f]+)"', vectors.read_text()):
        seed("kv_jsc", bytes.fromhex(value), "tests/rt/kv_jsc_deserialize_vectors.inc: Bun 1.4.0 public generated vector")
    for data in (b'GET / HTTP/1.1\r\nHost: fixture.invalid\r\n\r\n',
                 b'POST / HTTP/1.1\r\nHost: fixture.invalid\r\nContent-Length: 3\r\n\r\nabc',
                 b'POST / HTTP/1.1\r\nHost: fixture.invalid\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n',
                 b'GET / HTTP/1.1\nHost: fixture.invalid\n\n', b'GET / HTTP/1.0\r\n\r\n'):
        seed("http_request", data, "synthetic node:http request forms in runtime HTTP differential tests")
    for data in (b'HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc',
                 b'HTTP/1.0 204 Empty\n\n', b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n',
                 b'3\r\nabc\r\n0\r\nX-Foo: bar\r\n\r\n', b'fffffffffffffffff\r\n'):
        seed("fetch_response", data, "synthetic fetch head and chunked framing vectors")
    for data in (b'\x81\x02hi', b'\x81\x82\x00\x00\x00\x00hi', b'\x88\x02\x03\xe8',
                 b'\x01\x01a\x80\x01b', b'\x89\x00', b'\x82\x7f\xff\xff\xff\xff\xff\xff\xff\xff'):
        seed("websocket", data, "public synthetic RFC6455 receiver vectors")
    for data in (b'\xa0', b'\x00', b'\x81\x01', b'\xa1\x61a\x01', b'\x9f\xff', b'\x1b\xff\xff\xff\xff\xff\xff\xff\xff'):
        seed("passkeys", b'\x00' + data, "synthetic CBOR decoder edges")
    for data in (b'\x30\x00', b'\x02\x01\x01', b'\x30\x80\x02\x01\x01\x00\x00', b'\x30\x81\xff'):
        for mode in (1, 2):
            seed("passkeys", bytes((mode,)) + data, "synthetic ASN.1/Certificate schema edges")
    vectors = GATEWAY / "tests/passkeys_webauthn_differential_vectors.inc"
    for raw in re.findall(r'R"VEC\((.*?)\)VEC"', vectors.read_text()):
        case = json.loads(raw)
        for encoded in case.get("crls", {}).values():
            if isinstance(encoded, str):
                seed("passkeys", b'\x01' + base64.b64decode(encoded), "public synthetic WebAuthn differential CRL vector")
        body = json.loads(case.get("body", "{}"))
        response = body.get("response", {}).get("response", {})
        encoded = response.get("attestationObject")
        if encoded:
            data = base64.urlsafe_b64decode(encoded + "=" * (-len(encoded) % 4))
            if len(data) <= 4095:
                seed("passkeys", b'\x00' + data, "public synthetic WebAuthn differential attestationObject")
    (GATEWAY / "fuzz/seeds.json").write_text(json.dumps(MANIFEST, indent=2) + "\n")
    print(json.dumps({target: sum(1 for item in MANIFEST if item["target"] == target)
                      for target in sorted({item["target"] for item in MANIFEST})}))


if __name__ == "__main__":
    main()
