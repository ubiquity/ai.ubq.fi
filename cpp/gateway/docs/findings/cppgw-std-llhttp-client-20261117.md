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

## Continuation contract v2

The continuation starts at commit `542c1aff6f3305f39541e6f2553db7eef7cd1482`, tree
`2beb8ee0c43c44f0b9d5abbbfa686d8ef344d4c4`. Verify the committed replacement and finish qualification using strict library grammar. Fetch preserves bounded heads, ordered fields, interim responses, bodyless semantics, streamed delivery and safe connection reuse. WebSocket preserves the 16 KiB head bound, Latin-1 fields, and the first frame when it arrives in the same read as the upgrade. The owned paths cover the shared parser, the WebSocket response head, vendor registration, the new tests and fuzz target, and documentation. The request writer, connection lifecycle, content decoding, other lanes, deployment and credentials are outside this work. Two scope questions remain open because the existing fixed-length and EOF body pump and the Bun-era expectation table are in files absent from the owned-path list.

The completion gates are strict GCC 14 optimized and Clang 21 ASan/UBSan builds, isolated fixture runs through the Links registry, 120 seconds of libFuzzer on the library adapter, and same-runner optimized parser timing and allocation counts against `093a9e05`. The focused build is expected to take roughly two minutes, a full compile and suite tens of minutes, and fuzz execution two minutes after compilation. Each build uses its own external output and one execution handle, with status summaries from its logs and per-test receipts. A compile or fixture failure is diagnosed against the inherited tree before attributing it to the parser. Stop after the authorized changes and gates settle, recording unresolved ownership or runner limits with the exact evidence.

## Inherited build

Rebuilt official Boost 1.92.0 with `./bootstrap.sh --with-libraries=json,container --with-toolset=gcc` and
`./b2 -j2 toolset=gcc-14 --with-json --with-container variant=release link=static runtime-link=shared threading=multi --stagedir="$scratch/boost-stage" stage`. Built SQLite 3.54.0 with
`./configure --prefix="$scratch/sqlite-prefix" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`
and `make -j1 && make install`. The SQLite archive matched the checksum in [CI.md](../CI.md). Installed Clang 21.1.8 from the LLVM Noble repository. Bun reported 1.4.0 with `bun --no-env-file --version`.

The pinned llhttp archive matched its published SHA-256. All five vendored files matched the specified archive entries by `cmp`, and `sha256sum -c SHA256SUMS` passed 5/5. The vendored licence is the upstream archive's `LICENSE` entry.

The inherited focused release gate compiled 44/44 runtime objects and seven test binaries with GCC 14.2.0 and the fatal port warnings. Six binaries passed and one failed, with 74/75 cases passing. The new library test passed 8/8. The existing fetch transport binary passed 32/33. Its grouped Bun response grammar case failed on expected strict-grammar differences. Existing WebSocket runtime, lifetime and connection fixtures passed. The runner used the repository's Linux network, mount and PID fixture boundary through passwordless sudo and restored the invoking uid before each test. Inputs were unchanged across the run.

The portable build driver at `cmake/ci/driver.py` currently generates a vendor archive from only `Library.source` with the C++ driver. It omits the other two llhttp C translation units. The scratch runner uses the repository's source closure, fixture boundary and test runner, with an external Ninja generation adjustment to compile all three vendor sources as C and archive them. The native CMake registration already selects all three sources. The portable driver itself is outside the assigned paths.
