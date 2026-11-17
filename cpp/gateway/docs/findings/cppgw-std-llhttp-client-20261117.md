# llhttp HTTP client response findings

## Work contract v1

Replace OSS plan rows 7 and 8 with the pinned llhttp 9.4.4 release. The baseline is the clean C++ port at `093a9e0511e9d0deae5495dc324e53ff4dccd719`. The HTTP response parser must preserve the runtime interfaces, response headers, streaming and failure boundaries required by gateway callers. Use strict llhttp behavior for wire grammar. Preserve bounded head reads, header counts where applicable, raw field order and duplicates, trailing bytes at message boundaries, and ordinary JavaScript-style failures.

| Consumer | Boundary and behavior |
| --- | --- |
| Fetch response head | Parse incrementally, skip informational responses, reject unsolicited upgrades, and preserve status, reason and ordered fields |
| Fetch response body | Parse Content-Length, chunked and EOF-delimited bodies, honor bodyless response semantics, and keep content decoding separate |
| WebSocket handshake | Parse a bounded response head, retain the raw header representation, and hand bytes beyond the head to the existing upgrade path |

The request writer, networking lifecycle, content decoders, WebSocket upgrade validation and frame handling, server request parsing, TypeScript, deployment and credentials are outside this change. The owned production regions are the fetch response parser, its internal parser types and the WebSocket response-head function. Vendor registration, component tests and fuzzing, documentation and the report are the other assigned areas.

The body pump and existing component expectations are outside the enumerated paths. Scope clarification is pending. The initial work uses the owned paths. Further unknowns include llhttp's strict grammar at the accepted boundaries and support for multi-file C archives in existing build entry points.

Verify the archive and individual file hashes, compile with the port's fatal warning flags, run component and affected closures plain and under ASan/UBSan, and run the library harness for 120 seconds. Compare the baseline and candidate in a matched optimized parser benchmark with allocation counting. Stop after the gates settle or a specific scope, toolchain or runner limit prevents further verification. Record commands, exact outcomes and the remaining work here.

## Runner

Ubuntu 24.04.5 ARM64 with four CPUs, GCC 14.2.0 and Bun 1.4.0. Build outputs and downloads use an external temporary directory. Test and oracle processes use fresh working, home and temporary directories and an environment allowlist. Passwordless privileged network namespace setup is available. Unprivileged user namespace mapping fails on this runner.

```sh
curl -fsSL https://codeload.github.com/nodejs/llhttp/tar.gz/refs/tags/release/v9.4.4 -o "$scratch/llhttp.tar.gz"
printf '%s  %s\n' 5600adb1498f82a9c719b8eb19310021737388e0d92362a6d47be532710d3c96 "$scratch/llhttp.tar.gz" | sha256sum -c
```

The archive hash matched. Runtime, test and performance receipts are pending.
