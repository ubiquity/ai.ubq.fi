# Deterministic regex limits on Linux ARM64

Base `claude/cpp-gateway-port` at `0c54578151b88da9afe6d014007dbffe1176214b`, tree `914c34ba615cd5de878ba8454595980668f3619d`. For a resumed run, read this checklist and `git log` before continuing.

- [x] Locate the execution, compile and allocation hooks and confirm the ARM64 runner supports Linux namespaces.
- [x] Install the workflow dependencies and build SQLite 3.54.0 from the checksum-verified release.
- [x] Measure matcher callbacks in the existing fixtures.
- [x] Implement the work limits and give wall-clock backstops distinct errors.
- [x] Add regressions with an injected clock.
- [x] Audit other wall-clock limits.
- [x] Run 20 consecutive rounds of four isolated ASan compaction copies.
- [ ] Finish the release and sanitizer fixture gates. Bind results to the verified commit and tree.

## Callback calibration

Ubuntu 26.04.1 ARM64, four CPUs, Clang 21.1.8, Boost 1.90, ICU 78.2 and SQLite 3.54.0. SQLite was built from the workflow archive with `--disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`. The CI driver verified its version inside the namespace.

The temporary counter in `0a6c6265922309698964faf7992088da215e3a2c` counted callbacks over each matcher lifetime, including operations that completed without a callback. It logged only category, subject size, callback and search counts, and elapsed microseconds. The initial regex run tested that tree (`d738be45eaf5f641d9b31e73a8a96e9de88f580c`). The remaining runs used the same sources at `fe5157d201d26bb874cd72358a02ee7e49f6f01e`, tree `93a13506895093a48139543543502f21bbeaced9`.

For each filter below, ran `bash cpp/gateway/tools/ci-build.sh --config asan <filter>` with `UOS_CI_JOBS=4`, the task-local ccache wrappers for Clang and `UOS_CI_SQLITE_PREFIX=/usr/local`. The driver used isolated Linux network, mount and PID namespaces with an empty home and fresh tmpfs. Filters were `rt/regex_test.cpp`, `compaction`, `images`, `_sse_`, `responses_stream`, `supervisor`, `input_normalization`, `transport_normalization`, `fetch_transport_test` and `sentinel_upstream_capture`. The instrumented supervisor run reported two existing performance assertions over 2 seconds in `codex_supervisor_secret_test` while the release build was compiling. Those cases reached their redaction assertions and contributed callback counts.

| Category | Matcher operations | Peak callbacks | Subject at peak (bytes) | Fixture | New limit at that size |
| --- | ---: | ---: | ---: | --- | ---: |
| Generic | 992,012 | 317 | 635,017 | `codex_supervisor_secret_test` | 1,334 |
| Image | 296 | 1,048 | 5,242,901 | `images_test` | 4,096 |
| Compaction and shared whitespace pattern | 39,880 | 559 | 2,796,204 | `sentinel_upstream_capture_test` | 2,240 |
| SSE framing | 880 | 26 | 65,539 | `responses_stream_test` | 104 |
| Supervisor credential key | 68,982 | 1,689 | 200,000 | `codex_supervisor_brief_test` | 4,096 |
| Stored base64 | 4,322 | 36 | 80,001 | `regex_test` | 144 |

The base allowances cover short-subject bursts, including 16 generic callbacks at 801 bytes. The slopes target about four times the peak at its measured size. The previous 4,096-callback ceiling limits the supervisor case to 2.43 times the measured peak and the image case to 3.91 times. Short searches can complete before QuickJS reaches its first callback. The compaction floor fixture did 1,284,750 searches over 8,101,176 bytes with zero callbacks. The largest ASCII image and stored-base64 cases used their existing linear scans and had zero callbacks.

The existing 52-byte catastrophic-backtracking fixture reached the previous 4,096-callback ceiling while the calibration clock allowance was relaxed. The new generic allowance at that size is 64. The regression asserts the exact rejection point with a frozen clock and with 20 ms added per clock read. The source and subject caps, stack guard and single-allocation ceiling still apply.

## Backstop sizing

Timing was used only to size the wall-clock backstops. The largest measured legitimate matcher operation was the compaction floor scan at 2.844 seconds under ASan. Scaling that scan to 16 MiB and a 20× slower host gives `2.844 × (16,777,216 / 8,101,176) × 20 ≈ 118 seconds`. Supplemental ASan probes reached all 4,096 callbacks in 2.733 seconds for the fixed supervisor pattern at 1 MiB and in 1.027 seconds for the SSE boundary pattern at 16 MiB, or about 55 and 21 seconds at 20×. The five-minute matching backstop exceeds each estimate. The backstop runs at cooperative checks, including between short VM searches.

Libregexp provides parser stack probes and has no periodic compile interrupt. The 4 KiB source cap and 128 KiB parser stack allowance provide deterministic bounds. An isolated ASan probe compiling a nearly 4 KiB Unicode set of repeated `\\p{RGI_Emoji}` took 6.660 seconds during concurrent builds. A sticky regex can compile two programs, giving `6.660 × 2 × 20 ≈ 267 seconds`. The five-minute compilation backstop covers that estimate and gives a distinct error at stack probes or after the engine returns. The match and compile backstops use the same steady clock. The regression clocks exercise expiry without sleeping.

## Loaded compaction repeat

Verified code at `f28e78d4c04a4b99b76214defccf04c767fe210c`, tree `3669b33d160b324e82f3582c84fb661db1bffce3`, source SHA-256 `eeec06005c3f96d0d1f52f2c5f819a84638a95eb3b9f3cb8f330665c5c329eb0`.

```sh
scratch="$RUNNER_TEMP/cppgw-regex-budget"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 \
  python3 -B "$scratch/incremental-compaction.py" "$scratch"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 \
  python3 -B "$scratch/repeat-compaction-incremental.py" "$scratch"
```

The incremental build used the CI driver's ASan Ninja recipes to recompile `regex.cpp`, `regex_compile.cpp`, `regex_match.cpp`, `regex_tables.cpp` and `jev_compaction_test.cpp`. It substituted those objects in the stored driver link command and reused the ASan objects for the unchanged sources. The repeat used the same binary (SHA-256 `723b0b0836fe0a0cf2afcc90b15db20ebed5f246d8e8ecea130ac6c742bccb7f`) for all rounds, with the driver's `find_boundary`, `boundary_command` and `execute` helpers and its ASan, UBSan and leak options. Each copy had a separate loopback-only network, mount and PID namespace, empty home and fresh tmpfs.

All 20 rounds completed with four overlapping copies per round. All 80 executions and 1,920 cases passed, including the slowed-clock floor fixture in each copy. The shortest four-copy overlap was 25.122 seconds. The repeat took 696.266 seconds, with a slowest single copy of 36.517 seconds.

## Wall-clock audit

Searched native `src/`, `include/uos/` and `runtime/` for clock reads, timeout and deadline setters and comparisons, and checked what each expiry can stop. File paths below are relative to `cpp/gateway`.

| Classification | File and limit | What expires |
| --- | --- | --- |
| CPU work, same defect | `src/rt/regex_match.cpp`, `src/rt/regex.cpp` — `kRegexLargeSubjectDeadline` | The fixed image data URL and stored base64 validators and the base64 alphabet substitutions are already linear under the encoded-byte caps. Remove their elapsed-time rejection. |
| Network | `src/runtime/http.cpp` — `Server::headersTimeout`, `requestTimeout`, `keepAliveTimeout`, `timeout` | Header and body receipt, connection idle, and keep-alive. Gateway startup disables the generic request and socket inactivity timers. |
| Network | `src/rt/fetch_transport_net.cpp`, `include/uos/rt/fetch_transport.hpp` — `ClientOptions::timeoutMs` and pool keep-alive; `src/rt/websocket.cpp`, `src/rt/websocket_client.cpp` — `kWebSocketIdleTimeout`, `CLOSE_TIMEOUT`, `handshakeTimeout` | Fetch connect and socket I/O, pool reuse, and WebSocket handshake, idle and close. |
| Network | `src/inference_deadline.cpp`, `include/uos/inference_deadline.hpp` — `BUFFERED_INFERENCE_DEADLINE_MS`, `STREAM_FIRST_EVENT_DEADLINE_MS`, `STREAM_INACTIVITY_DEADLINE_MS`, `PAID_PROVIDER_FIRST_HEADERS_DEADLINE_MS` | Origin service deadlines for provider attempts and gaps between events. The stream adapters in `src/deepseek/stream.cpp`, `src/provider/{openrouter_streams,anthropic_chat_stream,anthropic_responses}.cpp` and `src/responses_stream.cpp` use these defaults or equivalent caller overrides. Codex, Cerebras, Claude, DeepSeek, Lithos, Metered and Surplus dispatch also use these deadlines or pass the request's remaining allowance. |
| Network | `src/bounded_response_body.cpp` — `BOUNDED_RESPONSE_BODY_TIMEOUT_MS`; `src/codex/dispatch.cpp` — `CODEX_MODEL_UNSUPPORTED_PROBE_TIMEOUT_MS`; `src/images.cpp` — `BUFFERED_INFERENCE_DEADLINE_MS`; `src/live/calls.cpp` — `LIVE_CALL_BODY_TIMEOUT_MS` and the 1 s error-body read; `src/live/upstream.cpp` — `LIVE_SIDEBAND_HANDSHAKE_TIMEOUT_MS` | Wait for stream reads, multipart upload, small diagnostic bodies or a sideband peer. |
| Network | `src/provider/{surplus,metered,openrouter,capacity,claude_code_usage,claude_code_models,lithos_models}.cpp` — `SURPLUS_FETCH_TIMEOUT_MS`, `METERED_FETCH_TIMEOUT_MS`, `OPENROUTER_FETCH_TIMEOUT_MS`, `OPENROUTER_DECISIONS_TIMEOUT_MS`, `PROVIDER_CAPACITY_CODEX_TIMEOUT_MS`, `USAGE_TIMEOUT_MS`, the 8 s model lookup and `LITHOS_MODELS_FETCH_TIMEOUT_MS`; `src/metered_quota.cpp` — `METERED_QUOTA_FETCH_TIMEOUT_MS`; `src/models/{codex_models_fetch,openrouter_models}.cpp` — `SINGLE_ACCOUNT_CATALOG_TIMEOUT_MS` and `OPENROUTER_FETCH_TIMEOUT_MS` | Provider information and model lookups, quota reads and decisions requests. |
| Network | `src/health.cpp` — `ACTIVE_UPSTREAM_HEALTH_TIMEOUT_MS`; `src/auth/local_development_key.cpp` — `LOCAL_DEVELOPMENT_PRICING_DEADLINE_MS`; `src/auth/{passport,passport_sessions}.cpp` — 10 s fetches; `src/codex/{auth_refresh,manual_reset,banked_reset}.cpp` — 10 s refresh, 1 s diagnostic body, 8 s reset probe and `CODEX_BANKED_RESET_INVENTORY_TIMEOUT_MS`; `src/admin/{chart_source_proxy,kernel,gemini_quota}.cpp` — `REMOTE_CHART_TIMEOUT_MS`, `REMOTE_PUSH_TIMEOUT_MS`, 5 s usage and reset diagnostics and `GEMINI_QUOTA_REFRESH_DEADLINE_MS` | Upstream RPC, OAuth, health and management requests. |
| Network and peer processes | `src/codex/{supervisor_transport,supervisor_inventory,supervisor_brief,native_auth_transport}.cpp` — `HANDSHAKE_TIMEOUT_MS`, `DEFAULT_CALL_TIMEOUT_MS`, `OVERALL_DEADLINE_MS`, `BRIEF_DEADLINE_MS`, `CALL_TIMEOUT_MS`; `src/provider/gemini.cpp` — `GeminiCliOptions::timeoutMs` and `--print-timeout 120s`; `src/harmony/tools/router.cpp` — `SHELL_DEFAULT_TIMEOUT_MS`; `src/jev_compaction/{compaction,hybrid}.cpp` — `JEV_TIMEOUT_MS`, `HYBRID_RESIDUE_TIMEOUT_MS` | App-server, CLI, sandbox command and summarizer waits. The supervisor brief's request deadline spans its transcript read and provider call. Redaction itself uses the regex budget. |
| Network and service deadline | `src/embeddings/{handlers,voyage}.cpp`, `include/uos/embeddings/ledger.hpp` — `EMBEDDINGS_TIMEOUT_MS`; `src/cache/scope_experiment.cpp` — `PROMPT_CACHE_SCOPE_EXPERIMENT_{CYCLE,SAMPLE}_DEADLINE_MS` | End-to-end embeddings, including rate-limit and KV waits, and provider sampling. These are service response windows. |
| Local waits and retention | `src/inference_admission.cpp` — `DEFAULT_INFERENCE_ADMISSION_LIMITS.maxQueueWaitMs`; `src/rt/kv_sqlite.cpp` — `BUSY_TIMEOUT`; `include/uos/storage/native_kv.hpp` — 5 s lock retry; `src/{catalog/store,provider/capacity,metered_quota}.cpp` — `CODEX_CATALOG_COLD_WAIT_MS`, `PROVIDER_CAPACITY_COLD_WAIT_MS`, `METERED_QUOTA_COLD_WAIT_MS`; `include/uos/optional_telemetry_queue.hpp` — `maxDrainWaitMs`; `src/sentinel/upstream_capture.cpp` — `SENTINEL_UPSTREAM_MAX_TIMING_MS` | Queue admission, DB-lock and refresh waits, optional telemetry shutdown and replay timestamp validation. Expiry, retry windows, leases and replay timings represent elapsed time. |

The compaction smoke tools also have test and RPC wait deadlines (`src/lib/jev_compaction/jev_compaction_smoke.cpp` and `jev_codex_smoke.cpp`). The audit found no other in-process CPU-work timer that requires a budget conversion.
