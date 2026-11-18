# Fetch response framing follow-up

## Work contract v3

The starting commit is `de4ff43111082c5bf9c7cbbc5c2f8a3c219804a6`, tree
`37903b18b303babb6a2e664d27dc3786a613660c`. Carry the llhttp 9.4.4 parser from the response
head through the live body pump, bring fetch assertions into line with standard HTTP, and measure the short SSE path
against both the pre-llhttp reference and the inherited client. Preserve status, headers, body bytes, error types,
backpressure, redirect draining, connection boundaries, truncation errors and the existing head limits.

| Response | Completion and delivery |
| --- | --- |
| Content-Length | Deliver only the declared bytes. Reject premature EOF and keep surplus bytes out of the body. |
| Chunked and SSE | Deliver payload progressively, preserve message boundaries across fragmented input, and reject ambiguous transfer framing. |
| Close-delimited | Feed raw payload until an orderly EOF. Network failures and timeouts still fail the body. |
| HEAD, 204, 304 and interim 1xx | Honor HTTP bodyless semantics, skip interim heads, and reject an unsolicited upgrade. |

Owned production changes are the fetch parser and internal types plus the live pump and its parser call. The fetch
expectation files, library fuzz adapter, deviations and findings are also in scope. The request writer, content
decoders, WebSocket, vendor/build integration, unrelated tests, providers, production and credentials are outside the
change. Fixture traffic is isolated loopback only, with an empty home and fresh temporary directory.

The inherited findings name one fetch assertion failure and a separate ICU-sensitive IDNA failure. Establish the
actual set of failures from the registered tests. The portable driver is reported to omit two vendored C sources
from the archive, so check the stock driver and keep any runner-only build adaptation in scratch. Build SQLite
3.54.0, Boost 1.92.0 and ICU 78.3 outside the checkout.

Acceptance uses `bash cpp/gateway/tools/ci-build.sh --config release` with all registered tests, the same selection
with Clang ASan/UBSan, the library fuzz harness, and the matched short SSE measurement. Because the portable driver
adds all runtime objects to every binary, the fetch link closure is the full suite. The release and sanitizer compiles
each take tens of minutes on four CPUs. Use external logs and per-test receipts, reuse immutable build objects between
iterations, and diagnose build or fixture failures against the inherited source. Stop after the gates settle or a
specific runner, build, ownership or publication limit prevents further progress. Record exact commands and counts
and keep task commits local only while remote publication is unavailable.
