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

## Exceptions in fixture readers

| Failing binary | Root cause and repair |
| --- | --- |
| `compaction_summary_forwarding_test` | Static registration parsed a 71,469,423-byte bundle of 15 forwarding traces through the 16 MiB parser entry point. The bundled fixture gets a 72 MiB read limit. Individual requests still exercise the real forwarding and admission checks. |
| `native_authority_test` | Static registration parsed the 40,807,665-byte native authority assembly bundle through the 16 MiB entry point. The shared fixture reader uses a 40 MiB limit. The RPC and SQLite assertions are retained. |
| `native_principal_control_test` | The same assembly bundle failure prevented private-control scenarios from registering. The bounded fixture reader restores those scenarios. |
| `native_runtime_binding_test` | The same assembly bundle failure prevented runtime-binding scenarios from registering. The bounded fixture reader restores those scenarios. |
| `jev_compaction_differential_test` | The test parsed a deliberately out-of-range `1e999` during setup to obtain infinity. It now checks that the Jev response parser catches and rejects the wire input, then constructs infinity directly to retain the `noulAnswer` finite-number assertion. |

The larger allowances apply to repository fixtures. Production keeps its default byte and nesting limits.

## Authentication, normalization, quota and replay

| Failing binary | Root cause and repair |
| --- | --- |
| `kernel_attestation_differential_test` | The decoded header for request 11 is the intact 13-byte `{"alg":1e400}`. Its payload decodes to 228 bytes. The bounded parser rejects the unsupported number before the algorithm check. The expected response remains HTTP 401 with `invalid_kernel_token`. The real `{"alg":null}` vector continues to reach the algorithm check, and valid signed JWTs still go through verification. The empty JWT parts case keeps its parse failure with the common message. |
| `harmony_differential_test` | The normalizer compacts JSON accepted by the bounded parser and preserves upstream text on failure. `1e400` follows the same raw-string path as other unparseable arguments, so the gateway preserves the argument instead of inventing a null value. The integer-like-key case expects parser input order with all values intact. The wire field remains a string. |
| `native_codex_429_differential_test` | The first mismatch, `b57.h0`, includes 5,000 nested arrays. The result digest changes from `b73ef4a13b381702` to `16605f5f878cd9e7`. The serialized fields `usageLimitReached`, `retryAtMs`, `quotaBlockSource` and `resetDeadlineIsStable` change from `true`, `1700000060000`, `"body_resets_at"` and `true` to `false`, `null`, `null` and `false`. The forwarded body remains 10,068 bytes with digest `e23dfe767edc3487`. The duplicate-key scan now uses the same 128-level bound as simdjson. At depth 128 the body still proves quota, and at 129 a valid `Retry-After` supplies retry timing alone. Routing and settlement require the body classification to record quota proof. |
| `anthropic_replay_test` | The store's locale-sorted canonical bytes put `"_"`, `"10"`, and `"9"` in that order. Parsing restored payloads now retains that order, where Bun moved both integer keys to the front. Only the rendered order changes in the fixture. Fingerprints, receipt sizes, values and opaque provider signature bytes are checked separately. |

## Release and sanitizer receipts

Pending on the Ubuntu 24.04 ARM64 runner.
