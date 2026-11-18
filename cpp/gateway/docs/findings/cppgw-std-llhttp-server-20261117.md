# Server HTTP parser replacement with llhttp 9.4.4

## Contract v1

Replace row 6 of the C++ OSS replacement plan on base `093a9e05`. llhttp supplies the request-line, header and chunked-body grammar. The server retains request dispatch, raw header projection, header limits, body flow, pipelining, upgrade boundaries and the existing socket and delivery lifecycle.

| Input | Acceptance surface |
| --- | --- |
| HTTP/1.0 and HTTP/1.1 origin-form and absolute-form requests | Preserve method, target, ordered header names and values, Host requirement for HTTP/1.1 and connection behavior. |
| Fixed length or chunked requests | Frame boundaries come from llhttp. Continue to stream body data only when the existing connection lifecycle requests it. Preserve bytes for the next request. |
| HTTP Upgrade | Preserve bytes after the head for the existing upgrade listener. |
| Malformed or oversized requests | Use llhttp strict mode and preserve the server's 400 and 431 responses where applicable. Maintain resource bounds. |

Writes are confined to the assigned parser and its immediate glue, the versioned llhttp vendor drop and its three registration files, the report and deviations, and the assigned fuzz and test paths. The fetch client, HTTP session and writer, socket handoff implementation, TypeScript, hosted workflows and deployment are outside this change.

Verify the archive hash and static library registration, use the Links resolver to select every consumer of `src/runtime/http.cpp`, and run those closures with the strict ARM64 flags in release and ASan/UBSan configurations. Run the new libFuzzer harness for 120 seconds and measure old and new request parsing at `-O2` with identical inputs. Record exact case counts and remaining defects. Stop after verifying the committed parser tree and preserving the bounded task commits for the parallel C++ integration. The measured standard-parser deviations and the scope of the existing Bun-recorded fixtures require discovery.

## Continuation checkpoint

Resumed at `d0d2ed8b15725c8005671a4155abe216f727051a`, tree
`fb45fadc7f20371ed9016f47a480abb8eff6059d`. Strict Clang 21.1.8 on Linux ARM64
built the inherited closure and passed all four standalone parser cases. CMake
3.31.6 built the three-member `uos_vendor_llhttp` C archive, and its test
registry resolved all 442 binaries. The retained real-socket wire run passed
102 of 106 comparisons and four of five harness cases. The four differences
are strict parser refusals for an undelimited transfer coding, duplicate
Content-Length, an ASCII DEL in a header and raw UTF-8 in a request target.

The adapter now limits a data call to the remaining payload bytes reported by
llhttp. This preserves delivery of the payload fragment before a delimiter
error, including when both arrive in the same socket read. The parser test
covers this boundary and verifies both orders of the length/transfer-coding
conflict.

## Discovery and local gates

The active checkout has 442 registered binaries. The Links resolver finds 88
that list `src/runtime/http.cpp`, plus the standalone parser test that includes
it. The portable continuation builder compiles each source once and uses the
repository CI fixture boundary with a fresh network, mount and PID namespace,
empty home and temporary directories, and an allowlisted environment. The
release run selects all 442 closures. The sanitizer run selects the 89 server
closures. The standalone test passed 5/5 after the chunk fix.

The archive SHA-256 matched
`5600adb1498f82a9c719b8eb19310021737388e0d92362a6d47be532710d3c96`.
`sha256sum --check SHA256SUMS` and direct `cmp` against the five extracted
upstream inputs passed. The generated upstream C file contains trailing
whitespace. The first-party diff passes `git diff --check` with the verified
vendor directory excluded.

Bun 1.4.0 loopback probes used `bun --no-env-file --no-install` inside an
isolated network namespace. They confirmed all four wire differences, along
with the metadata-limit and connection decisions recorded in
[Deviations from Bun](../DEVIATIONS-FROM-BUN.md).

The CMake registration builds all three C files. The portable production
driver `cmake/ci/driver.py` and the Mac script `tools/macos-build.sh` read the
single-member `library.source` view when building archives. The legacy SDK
`tools/unit.mk` has a vendor object rule for C++ inputs only. These build
drivers are outside the assigned files. The local verifier uses
`library.sources` and the C compiler for the three llhttp inputs.

## Adapter and production line count

llhttp owns method, target, header, transfer-coding, chunk extension and
trailer syntax. Its length and pause positions determine where the next
request or upgrade bytes start. The adapter projects ordered Latin-1
`rawHeaders` and keeps the existing Host requirement, strict request-line
spacing, byte and field budgets, `Expect: 100-continue`, fixed-length body
delivery and request and upgrade dispatch. The connection still controls
when the body flows and when the response settles. A `Proxy-Connection`
token can affect llhttp's keep-alive decision. Only an actual `Connection`
header can authorize the upgrade handoff.

`git diff --numstat 093a9e05..971bd53a -- cpp/gateway/src/runtime/http.cpp`
reports 185 physical lines removed and 223 added. The removed code split and
validated request lines and header fields, recognized methods and transfer
codings, and ran a manual chunk decoding state machine. The added lines
include the callback adapter and the retained bounds and dispatch glue.
The count excludes tests, registration and vendored code.

## Benchmark

Both binaries call `ConnectionState::parseHead` and, for chunked requests,
`decodeChunked`. The baseline includes `http.cpp` from `093a9e05`. Both
link the same optimized runtime and application dependencies. Each binary
reuses a connection and a warmed input buffer, consumes the same bytes in
the indicated segment size and resets the request and response after each
operation. The chunked case has two 4,096-byte data chunks, one extension and
one trailer. Clang 21.1.8 `-O2 -g0` on ARM64 Neoverse N2, pinned to CPU 0.
Timing is median thread CPU time over seven runs of 50,000 operations after
1,000 warmups. A separate 1,000-operation run counts C++ allocations via
regular, array and aligned `operator new`.

| Request | Wire bytes | Bytes per segment | Before ns/op | llhttp ns/op | Before allocations/op | llhttp allocations/op |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| GET /health | 54 | 54 | 666.24 | 334.04 | 13 | 4 |
| POST /v1/responses head | 320 | 320 | 2706.33 | 891.54 | 50 | 12 |
| POST /v1/responses head, fragmented | 320 | 16 | 4111.17 | 1233.41 | 50 | 12 |
| Chunked POST | 8355 | 1024 | 1646.92 | 1392.00 | 33 | 17 |
