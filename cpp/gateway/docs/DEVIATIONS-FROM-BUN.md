# Runtime deviations from Bun

The gateway keeps the application and protocol behavior described in [PORTING.md](../PORTING.md). This record covers deliberate generic runtime differences and the evidence that bounds them.

## JSON

The Linux ARM64 simdjson 5.2.0 and Boost.JSON 1.92 checkpoint passed 8/8 focused binaries and 71/71 cases, with 643 parser vectors retained. The package manifest is `c196ee22541cd11fcf67589a47ba7dc686c446e92bae0043b84e85da48a1cf4a`. [Behavior handback](/tmp/claude-1002/prod/assignment3/runtime/b1-20261117T230423Z/deviations.md) [Focused results](/tmp/claude-1002/prod/assignment3/runtime/b1-20261117T230423Z/focused-results.json)

- Raw parse failures use one `SyntaxError("Invalid JSON")`. A streamed Fetch body containing `[1,]` reports that diagnostic. Buffered Fetch keeps its malformed-body classification and cursor validation still rejects invalid encoded cursors.
- Parsed members follow decoded input order. For example `{"b":1,"2":2,"1":3}` serializes in that order. Explicit key-order helpers remain available for application consumers that require their projection.
- Raw malformed UTF-8, including encoded surrogate code points, is rejected before a JSON value is exposed. A JSON-escaped lone code unit such as `"\uD800"` still survives round trips where existing persisted JSON requires it. Native KV continues to use the separately identified storage codec.
- An overflowing JSON numeric literal such as `1e400` is rejected. Finite canonical doubles, subnormals, negative zero and compact number spelling keep their tested results.

The later v40 limit enforcement has its own [runtime table](RUNTIME.md#input-limits), source identity and boundary tests.

## UTF and Encoding

The simdutf 9.4.0 checkpoint kept the tested generic outputs and error classes. For example, a multibyte UTF-8 sequence split across streamed chunks is held until the next chunk by the bounded state adapter. Simdutf handles well-formed scans and transcoding, while the WHATWG adapter handles incomplete sequences, replacement, fatal errors and BOM state. Tested WTF-8/UTF-16 behavior protects callers' code-unit and storage contracts. The four focused Linux ARM64 binaries passed 56/56 cases. [Behavior handback](/tmp/claude-1002/prod/assignment3/runtime/c2-20261117T230537Z/GENERIC-BEHAVIOR.md) [Focused results](/tmp/claude-1002/prod/assignment3/runtime/c2-20261117T230537Z/focused-results.json)

## Incoming integrations

Add a component section after its verified handback. Record the changed behavior, one concrete input/outcome, the application or security rationale, and the exact source and test receipt. Use this location for URL/IDNA, HTTP, regex, WebSocket, passkey codecs, BigInt, ICU and the small helpers as their integrations land. Record limit-specific exceptions and tested boundary outcomes in [Runtime](RUNTIME.md#input-limits).
