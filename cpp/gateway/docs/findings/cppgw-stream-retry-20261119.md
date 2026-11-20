# Codex retries before stream output

- [x] Trace the request, stream, routing and settlement paths.
- [x] Implement and commit bounded retries and telemetry.
- [x] Add and commit scripted regressions and the TypeScript deviation.
- [x] Record release and sanitizer results for the committed tree.

After a restart, read this checklist and `git log --oneline` before resuming. The starting commit is `f92b14a9` on `agent-jobs/cppgw-cpp-only-20261119-44610457899-1`.

## Request path before the fix

- `runResponsesRoute` enters the Responses handler within one inference admission. `runOrdinaryResponsesTail` calls `fetchAndPreparePrimaryResponses`, which goes through `fetchResponsesWithPaidFallback` to `fetchCodexResponses`. Native Codex CLI requests can receive SSE headers before dispatch. The SSE headers and transport keepalives carry no upstream response ID or sequence number. [Route](../../src/handler/terminal_route.cpp) · [Responses handler](../../src/responses_handler.cpp) · [Attempt preparation](../../src/responses_attempts.cpp) · [Early headers](../../src/responses_native_headers.cpp)
- `readResponsesStream` frames and validates the Codex SSE body. `prepareResponsesStreamForCommit` buffers the preamble until terminal, answer-bearing output, or eligible reasoning progress that allows early release. `responsesEventSemanticKind` recognises text, refusals and usable tool output, including output carried by a terminal event. A failure after only lifecycle events can reach `deliverPreparedResponses`, which recorded its release as commitment. This accounts for the coincident commitment and terminal timings in the production logs. EOF before a terminal raises `premature_eof`. Once released, `createOwnedResponsesStream` relays events and handles interrupted bodies. [SSE parser](../../src/responses_stream.cpp) · [Commit and relay](../../src/responses_failover_stream.cpp) · [Delivery](../../src/responses_handler.cpp)
- Chat Completions uses the same waterfall, Codex transport, SSE reader and semantic predicate. `dispatchAndPreflightChatCompletions` owns its separate preflight and hands the result to the Chat Completions translator. [Chat preflight](../../src/chat/envelope.cpp) · [Translation](../../src/chat/stream_translation.cpp)
- `fetchPreparedCodexResponses` runs serial admission. `fetchAttempt` fences the selected account and logs one `codex_attempt` with the slot, phase and status or error class. The runners handle refresh, rotation on classified invalidity or quota exhaustion, short abortable 429 waits, and the fenced reset and overage phases. Only authoritative full-pool unavailability can send the request to a paid tier. A transient error alone leaves the durable active account in place. The serial-admission and short-429 limits govern those phases. A stream arriving with HTTP 200 currently leaves that machinery before its first failure event can be classified. [Codex entry](../../src/codex/index.cpp) · [Dispatch and retry wait](../../src/codex/dispatch.cpp) · [Attempt operations and health](../../src/codex/responses_operations.cpp) · [Account runners](../../src/codex/responses_runners.cpp) · [429 classification](../../src/codex/rate_limit_429.cpp) · [Waterfall](../../src/paid_fallback/routing.cpp)
- `ResponseTelemetryState` holds attempted providers, stream timing, terminal kind and usage. Responses records terminal usage via `recordResponsesTerminal`. The terminal wrapper combines the selected semantic result with actual socket delivery and settles the request quota and any paid lifecycle once. Dispatch already carries one reservation through the Codex attempt coordinator. [Telemetry](../../src/openai_telemetry.cpp) · [Terminal usage](../../src/chat/stream_translation.cpp) · [Admission and settlement](../../src/handler/terminal_route.cpp) · [Terminal log](../../src/handler/terminal_log.cpp)

## Event sequence and accounting

The dispatcher inspects the subscription stream with the existing bounded semantic preflight. It discards a retryable attempt's buffered events, then delivers the selected attempt's original preamble, response ID, sequence numbers and terminal. Early native SSE headers can still carry keepalive comments. The existing release on reasoning progress ends eligibility for a retry because those progress frames can reach the client. Chat Completions translates the selected stream through its normal adapter. [Stream preflight](../../src/responses_failover_stream.cpp) · [Shared routing](../../src/paid_fallback/routing.cpp) · [Chat preflight](../../src/chat/envelope.cpp)

Up to three extra attempts run through serial admission with the same serialized request, conversation identity and dispatch coordinator. Each retry checks the current account and credential fences. Transient stream failures do not move the active subscription or establish the evidence needed for paid fallback. The retry wait uses the existing abort signal, jittered backoff starting at 250 ms and any longer structured retry hint. The post-reset inference and overage probe retain their one-attempt limits. [Codex dispatcher](../../src/codex/index.cpp) · [Attempt operations](../../src/codex/responses_operations.cpp)

A discarded attempt releases its upstream reader and any claimed account probe. It records the failure's HTTP-equivalent health classification and one `codex_attempt` log with the original HTTP status. The final semantic result and actual client delivery still govern the one terminal settlement. The API-key dispatch debit is idempotent across attempts and stays consumed once transport has begun. A cancellation releases the pending kernel quota reservation. The request log records transported in-stream retries and the parsed failure event names and codes. The process logs each new safe event name once, up to a fixed limit. [Dispatch accounting](../../src/codex/dispatch.cpp) · [Terminal settlement](../../src/handler/terminal_route.cpp) · [Telemetry](../../src/openai_telemetry.cpp)

## Verification

The runs used Ubuntu 26.04.1 ARM64, Clang 21.1.8, Ubuntu Boost 1.90 and ICU 78.2. SQLite 3.54.0 was built with the workflow's pinned archive hash and `CFLAGS='-O2 -DSQLITE_DQS=3'`. The CI driver verified the loaded SQLite version inside the fixture boundary. Each test used a fresh home and temp filesystem, a credential-free environment, and a network namespace with loopback mocks. [CI recipe](../CI.md) · [Workflow](../../../../.github/workflows/cpp-gateway-ci.yml)

The tested commit is `96e34f0719206f520ca4dc195c5f610db6b55273`, tree `8771022f42f103d0f529938143aa6882129a9a77`, based on `f92b14a97818ff81bf95cbfb2642e8ff13b4ab13`. The focused quota fixture covers the two-attempt completion, the four-attempt bound, deterministic errors, post-output failure, EOF with and without a preamble, a nested rate-limit failure and retry hint in Chat Completions, cancellation during backoff, and expiry of the original deadline. The loopback fixture opens the SSE headers for a native Codex request before the upstream's delayed error and validates the selected attempt's response ID and completion on that connection. [Scripted scenarios](../../tests/kv_budget_part1_test.cpp) · [Loopback scenario](../../tests/codex_serial_routing_http_test.cpp)

The focused commands and link-selected runs used this environment from the repository root. The temporary selector passes to the standard CI driver the tests whose source or resolved link list intersects the changed C++ files from `git diff --name-only f92b14a9 HEAD -- cpp/gateway`.

```sh
scratch=/tmp/cppgw-stream-retry-ci
run_gate() {
  env -i PATH=/usr/local/bin:/usr/bin:/bin LC_ALL=C HOME="$scratch/$cfg/home" TMPDIR="$scratch/$cfg/tmp" \
    CC="/usr/bin/env CCACHE_DIR=$CCACHE_DIR /usr/bin/ccache /usr/bin/clang" \
    CXX="/usr/bin/env CCACHE_DIR=$CCACHE_DIR /usr/bin/ccache /usr/bin/clang++" \
    UOS_CI_JOBS="$jobs" UOS_CI_BUILD_DIR="$scratch/build" "$@"
}
for cfg in release asan; do
  case "$cfg" in release) jobs=2 ;; asan) jobs=4 ;; esac
  run_gate bash cpp/gateway/tools/ci-build.sh --config "$cfg" kv_budget_part1_test
  run_gate bash cpp/gateway/tools/ci-build.sh --config "$cfg" codex_serial_routing_http_test
  run_gate python3 -B "$scratch/select-linked.py" f92b14a9 --config "$cfg"
done
cfg=asan
jobs=1
run_gate bash cpp/gateway/tools/ci-build.sh --config "$cfg" codex_serial_routing_http_test
run_gate bash cpp/gateway/tools/ci-build.sh --config "$cfg" jev_hybrid_test
run_gate python3 -B "$scratch/select-linked.py" f92b14a9 --config "$cfg"
python3 -B cpp/gateway/tools/links.py --check
git diff --check f92b14a9..HEAD
```

### Results

Both final linked runs selected the same 168 binaries and passed all 2,496 cases. The focused runs passed 32 quota and stream cases and two loopback routing cases in each configuration. The registration gate resolved all 449 native tests.

| Gate | Binaries passed | Cases passed | Driver wall time |
| --- | ---: | ---: | ---: |
| Release linked | 168 | 2,496 | 589.618 s |
| ASan, UBSan and LSan linked, one worker | 168 | 2,496 | 304.260 s |
| ASan, UBSan and LSan linked, four workers | 166 | 2,494 | 985.233 s |

The four-worker sanitizer run reported `Invalid JSON` in the existing serial Codex routing case and `stack overflow` in the existing hybrid compaction guard case. Both binaries passed their individual one-worker reruns and the complete one-worker rerun on the same source. The driver recorded unchanged inputs and source digest `887a27a350e42944f8214116d8cfd3f62740da99e2b6398586b02163d9b92e73` for all three linked runs.

| Receipt under `/tmp/cppgw-stream-retry-ci` | SHA-256 |
| --- | --- |
| `build/env-abd48aa9c474/release/receipt.json` | `fa2bcfedd5487ac9627e6168486d3b9623b02308a688170db1c155df760cd10e` |
| `build/env-4c7593decdc1/asan/receipt.json` | `5c39e1bb5c4d00d8a011e0488e12a75dabd4e999efe43d6892a8a9d3931bf511` |
| `asan/linked-final.json`, four workers | `2470700343b84ce914c2dc666a10e18a92e72175fa7a5b11a63b4a621d694d63` |

### Committed review

The base ref `origin/agent-jobs/cppgw-cpp-only-20261119-44610457899-1` was fetched before review. The native Codex CLI 0.170.0 used `gpt-6.2-sol` for the exact base, tested head and tree above. It ran with an empty home and a private network namespace.

```sh
base=origin/agent-jobs/cppgw-cpp-only-20261119-44610457899-1
sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
  env -i PATH=/usr/local/bin:/usr/bin:/bin LC_ALL=C HOME="$scratch/review-final/home" TMPDIR="$scratch/review-final/tmp" \
  timeout -k 5s 35s "$review_bin" -c 'model="gpt-6.2-sol"' -c 'review_model="gpt-6.2-sol"' -c 'sandbox_mode="read-only"' \
  review --base "$base"
```

The isolated command reported `failed to connect to Responses WebSocket stage=Unknown` and timed out with exit 124 before producing review findings.
