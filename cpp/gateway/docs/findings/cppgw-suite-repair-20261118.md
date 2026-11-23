# C++ gateway suite repair on 2026-11-18

## Work contract v1

Restore the C++ port's Release suite at starting commit `1769741ee6d408f214c9c9dd4d8bcc7f5180a904`, using the CI workflow qualification as the starting failure inventory. Use simdjson 5.2.0, simdutf 9.4.0 and Boost 1.92.0 for generic JSON and encoding behavior. Preserve the gateway's wire formats, status codes, stored data, authentication, validation, accounting, input limits and OS fixture isolation. [Prior failure inventory](cppgw-ci-workflow-20261118.md)

The repair covers C++ sources, fixtures and documentation under `cpp/gateway/`. The workflow, TypeScript, vendored code, parallel library replacements, and deployment are outside the repair scope. All build data stays in runner scratch space. The malformed-input tests must still exercise their rejection branches. Valid JSON used in production and persisted data must still reach the same semantic checks. Any changes to serialized fixture bytes must be traced to their consumers before updating assertions.

The open questions are the exact input behind the five JSON exceptions, the JWT bytes delivered to the parser, and the consumer of the changed 429 digest. Acceptance requires the portable driver with `CXX=clang++ UOS_CI_JOBS=4`, four `--config release --shard N/4` passes on ARM64, and `--config asan` on the affected binaries. The stop condition is a clean checkout with all 441 Release binaries passing, recorded sanitizer results, and every known failure resolved in this report.

## Build blocker

`randomUUID()` still indexed the alphabet that the old crypto-local hex routine owned. It now formats the bytes with the existing lowercase Boost hex helper before inserting the UUID separators. The version and variant masks are preserved. Crypto digest and JWK encoding use the shared simdutf-backed helpers, and JWK decoding checks the existing URL alphabet, padding and length rules before calling the shared decoder.

## Parser diagnostics

The test expectations use the shared parser diagnostic. The fixtures still check the complete error shapes and outcomes.

- `deepseek_stream_differential_test`. The broken SSE object and empty SSE data still produce `malformed_event` with a `SyntaxError` cause. The expected cause message is `Invalid JSON` on both traces.
- `claude_code_models_differential_test`. A truncated models response still fails catalog parsing with `SyntaxError`. Only its message changes.
- `native_addon_differential_test`. Malformed inputs and provenance and BOM-prefixed inputs and receipts still fail at the artifact JSON boundary. The four rejected cases use the common syntax message.
- `kv_conversion_legacy_codec_differential_test`. Non-JSON helper output still fails both decode and encode with `SyntaxError`. Requests, process lifecycle and codec validations are compared as before.
- `anthropic_chat_stream_differential_test`. Malformed SSE still produces the same stream error and cause type. Its rendered cause is now `SyntaxError: Invalid JSON`.
- `passkeys_webauthn_differential_test`. Truncated client data still fails verification. Only the warning's parser message changes.

## Exceptions in fixture readers

- `compaction_summary_forwarding_test`. Static registration parsed a 71,469,423-byte bundle of 15 forwarding traces through the 16 MiB parser entry point. The bundled fixture gets a 72 MiB read limit. Individual requests still exercise the real forwarding and admission checks.
- `native_authority_test`. Static registration parsed the 40,807,665-byte native authority assembly bundle through the 16 MiB entry point. The shared fixture reader uses a 40 MiB limit. The RPC and SQLite assertions are retained.
- `native_principal_control_test`. The same assembly bundle failure prevented private-control scenarios from registering. The bounded fixture reader restores those scenarios.
- `native_runtime_binding_test`. The same assembly bundle failure prevented runtime-binding scenarios from registering. The bounded fixture reader restores those scenarios.
- `jev_compaction_differential_test`. The test parsed a deliberately out-of-range `1e999` during setup to obtain infinity. It now checks that the Jev response parser catches and rejects the wire input, then constructs infinity directly to retain the `noulAnswer` finite-number assertion.

The larger allowances apply to repository fixtures. Production keeps its default byte and nesting limits.

## Authentication, normalization, quota and replay

- `kernel_attestation_differential_test`. Algorithm request 11 has a 13-byte header `{"alg":1e400}` and a 228-byte payload after base64url decoding. Payload validation request 26 decodes to a 229-byte payload with `"installation_id":1e400`. Both numbers are out of range. The parser rejects those tokens before field validation. Both responses remain HTTP 401 with `invalid_kernel_token`. The `{"alg":null}` vector still reaches the algorithm check and valid signed JWTs still verify. The empty JWT parts case keeps its parse failure with the common message.
- `harmony_differential_test`. The normalizer compacts JSON accepted by the bounded parser and preserves upstream text on failure. `1e400` follows the same raw-string path as other unparseable arguments, so the client receives the original argument text. The integer-like-key case expects parser input order with all values intact. The wire field remains a string.
- `native_codex_429_differential_test`. The first mismatch, `b57.h0`, includes 5,000 nested arrays. The result digest changes from `b73ef4a13b381702` to `16605f5f878cd9e7`. The serialized fields `usageLimitReached`, `retryAtMs`, `quotaBlockSource` and `resetDeadlineIsStable` change from `true`, `1700000060000`, `"body_resets_at"` and `true` to `false`, `null`, `null` and `false`. The forwarded body remains 10,068 bytes with digest `e23dfe767edc3487`. The duplicate-key scan now uses the same 128-level bound as simdjson. At depth 128 the body still proves quota, and at 129 a valid `Retry-After` supplies retry timing alone. The routing and banked-reset consumers require `usageLimitReached` before they can use any retry deadline as quota evidence. Forty test digests change across the five newly rejected body shapes and eight header sets.
- `anthropic_replay_test`. The store's locale-sorted canonical bytes put `"_"`, `"10"`, and `"9"` in that order. Parsing restored payloads now retains the canonical order, including the position of integer-like keys. Only the rendered order changes in the fixture. Fingerprints, receipt sizes, values and opaque provider signature bytes are checked separately.

## Quota projection timing

An exact-tree rerun at 02:00 UTC also exposed an intermittent failure in `quota_projection_test`. Its balance-history fixture placed the newest sample one second after the hour began and read through the wall clock. During that first second, the reader correctly excluded the future sample. The fixture now uses fixed timestamps and checks the same bounded read one millisecond before the observation and at the observation. The production range selector retains its inclusive cutoff.

## Release and sanitizer receipts

The runner is Ubuntu 24.04.5 LTS on ARM64 with Clang 21.1.8 from apt.llvm.org, Boost.JSON and Boost.Container 1.92.0 built from the official Boost source archive, and OpenSSL 3.0.13. SQLite 3.54.0 was built with the workflow's archive checksum and `CFLAGS='-O2 -DSQLITE_DQS=3'`. The installed SQLite CLI returned `3.54.0|double quoted string` on the DQS check.

The first Release links succeeded. The fixture mount namespace hid the Boost build prefix under `/tmp`, so those processes exited 127 before starting any cases. Installing the same versioned Boost libraries in `/usr/local/lib` and updating the loader cache resolved the startup failure. The successful runs use the driver's real loopback-only network, private temporary filesystem and empty home.

The first complete shard 4 exposed two more dependency mismatches on Ubuntu 24.04. The loader selected SQLite 3.45.1 for `rt/sqlite_test` despite the pinned CLI, and ICU 74.2 changed the `rt/url_idna_test` Unicode vectors. Selecting `/usr/local/lib` through the driver's `LD_LIBRARY_PATH` runs the suite against the pinned SQLite library. ICU was rebuilt at the port's pin, 78.3, from the official source archive (SHA-256 `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0`). Its headers and versioned libraries were installed under `/usr/local`, and the URL object was rebuilt against the new headers. Runner-local pkg-config metadata retains the driver's existing explicit ICU data-library link. Both tests passed with the pinned libraries.

The C++ source and test receipts identify commit `380a9b0a19fca261c910967ee24078f1b22aec9a`, tree `abe280afaefb730fc669cb7df50c2214cc605381` and driver input SHA-256 `9629fe15f1187dd6309599aef3c46f242b0ed2f9f137ee6fd045d375b710e461`. The Release receipts cover 441 distinct binaries, each selected once, and each shard reported 369/369 production objects. Both ARM64-only binaries, `permissions_linux_test` and `runtime_native_startup_test`, passed.

| Release shard | Binaries passed | Binaries failed | Cases passed | Cases failed |
| ------------- | --------------: | --------------: | -----------: | -----------: |
| 1/4           |             111 |               0 |         1291 |            0 |
| 2/4           |             110 |               0 |         1033 |            0 |
| 3/4           |             110 |               0 |         1373 |            0 |
| 4/4           |             110 |               0 |         1334 |            0 |
| Total         |             441 |               0 |         5031 |            0 |

The `asan` configuration passed 19/19 binaries and 347/347 cases with AddressSanitizer, UndefinedBehaviorSanitizer and leak detection. The selection covers the 15 original failures, the quota projection fixture and the runtime Base64, crypto and JSON binaries.

```sh
for shard in 1 2 3 4; do
  CXX=clang++ UOS_CI_JOBS=4 bash cpp/gateway/tools/ci-build.sh --config release --shard "$shard/4"
done
CXX=clang++ UOS_CI_JOBS=3 bash cpp/gateway/tools/ci-build.sh --config asan "$test_path"
```

For these commands, `clang++` resolves to the installed Clang 21. The build cache is external to the checkout. `UOS_CI_CXXFLAGS` points to the Boost 1.92.0 headers, `UOS_CI_LDFLAGS` adds its build prefix library path, `PKG_CONFIG_PATH` selects the ICU 78.3 metadata, and `LD_LIBRARY_PATH=/usr/local/lib` selects the pinned runtime libraries.

## Publication

The HTTPS push preflight for `agent-jobs/cppgw-suite-repair-20261118` reached GitHub and stopped because the runner had no GitHub credentials. The counts above come from the direct ARM64 runner.
