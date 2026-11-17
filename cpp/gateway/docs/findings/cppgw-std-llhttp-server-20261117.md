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
