# C++ gateway suite repair on 2026-11-18

## Work contract v1

Restore the C++ port's Release suite at starting commit `1769741ee6d408f214c9c9dd4d8bcc7f5180a904`, using the failure inventory from the [CI workflow qualification](cppgw-ci-workflow-20261118.md). Use simdjson 5.2.0, simdutf 9.4.0 and Boost 1.92.0 for generic JSON and encoding behavior. Preserve the gateway's wire formats, status codes, stored data, authentication, validation, accounting, input limits and OS fixture isolation.

The repair covers C++ sources, fixtures and documentation under `cpp/gateway/`. The workflow, TypeScript, vendored code, parallel library replacements, and deployment are outside the repair scope. All build data stays in runner scratch space. The malformed-input tests must still exercise their rejection branches. Valid JSON used in production and persisted data must still reach the same semantic checks. Any changes to serialized fixture bytes must be traced to their consumers before updating assertions.

The open questions are the exact input behind the five JSON exceptions, the JWT bytes delivered to the parser, and the consumer of the changed 429 digest. Acceptance requires the portable driver with `CXX=clang++ UOS_CI_JOBS=4`, four `--config release --shard N/4` passes on ARM64, and `--config asan` on the affected binaries. The stop condition is a clean checkout with all 441 Release binaries passing, recorded sanitizer results, and every known failure resolved in this report.

## Build blocker

`randomUUID()` still indexed the alphabet that the old crypto-local hex routine owned. It now formats the bytes with the existing lowercase Boost hex helper before inserting the UUID separators. The version and variant masks are preserved. Crypto digest and JWK encoding use the shared simdutf-backed helpers, and JWK decoding checks the existing URL alphabet, padding and length rules before calling the shared decoder.

## Parser diagnostics

The test expectations use the shared parser diagnostic. The fixtures still check the complete error shapes and outcomes.

| Failing binary | Resolution |
| --- | --- |
| `deepseek_stream_differential_test` | The broken SSE object and empty SSE data still produce `malformed_event` with a `SyntaxError` cause. The expected cause message is `Invalid JSON` on both traces. |
| `claude_code_models_differential_test` | A truncated models response still fails catalog parsing with `SyntaxError`. Only its message changes. |
| `native_addon_differential_test` | Malformed inputs and provenance and BOM-prefixed inputs and receipts still fail at the artifact JSON boundary. The four rejected cases use the common syntax message. |
| `kv_conversion_legacy_codec_differential_test` | Non-JSON helper output still fails both decode and encode with `SyntaxError`. Requests, process lifecycle and codec validations are compared as before. |
| `anthropic_chat_stream_differential_test` | Malformed SSE still produces the same stream error and cause type. Its rendered cause is now `SyntaxError: Invalid JSON`. |
| `passkeys_webauthn_differential_test` | Truncated client data still fails verification. Only the warning's parser message changes. |

## Release and sanitizer receipts

Pending on the Ubuntu 24.04 ARM64 runner.
