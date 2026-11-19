# Native gateway residual review

Base `74062589` on `agent-jobs/cppgw-input-limits-20261119-44456009556-1`.

## Status

| Defect                                                 | Verdict | Commit and regression                                                                                                                                                                                |
| ------------------------------------------------------ | ------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Admin default-model field and application input limits | FIXED   | `fix(cpp): validate oversized request fields before library calls`. Regression cases in `admin_defaults_differential_test`, `admin_codex_reset_test`, `openai_compat_part11_test` and `images_test`. |
| SSE body settlement after rejected response headers    | FIXED   | `fix(cpp): cancel SSE bodies when response headers are rejected`. Regression in `serve_delivery_http_test`.                                                                                          |
| Chat roles and Responses tool types                    | Pending | Request and conformance regressions pending                                                                                                                                                          |

## Input limits

With a runtime configuration present, a trimmed model above 1 MiB reached the whitespace regex before the catalog lookup. Defaults now use the existing 200 UTF-16 code unit model ID limit from Decisions and return the exact unknown-model response. The guard applies to submitted model changes. The regression compares the error body to the recorded unknown-model case and checks the stored configuration.

The application audit found the same ordering on the banked-reset account hash, chat and Responses model IDs entering provider matching, multipart image numbers (`n`, `partial_images`, `output_compression`) and oversized MIME types in inline image URLs. Those fields now use their ordinary invalid-request paths before the regex or ICU call. Decisions, SystemOne and agent message tags already check their shorter field bounds. The HTTP parser checks query limits before constructing the application URL. Passkey login and registration have a 64 KiB body limit and Claude credential submission catches account-selector rejection as HTTP 400.

The supervisor brief route also matched JSON source and thread IDs before checking their lengths. It now checks the same 32- and 200-byte maximums encoded in the ASCII patterns. `codex_supervisor_brief_test` covers both validation responses. The supervisor output route receives these IDs in the bounded HTTP query string.

## SSE header rejection

Native HTTP rejects DEL in response headers after Fetch Headers has accepted it. Before the fix, this path emitted the fallback 500 with the SSE body still pending. The transport now cancels the original body with the header error before rethrowing it to the existing fallback handler. The loopback test checks the fallback status, content type and body, one upstream cancellation, one terminal settlement, one release and one interrupted delivery log. The server drains the request before the test completes.

## Verification

Pending. This report will record the change-scoped Release and sanitizer gates, one full Release shard, and the native registration check.
