# Native gateway coroutine and callback lifetime audit, Area C

## Contract v1

Audit all first-party source and headers for the eight assigned gateway modules. Trace coroutine
closure, parameter, callback, iterator and await-expression lifetime through normal return, errors,
cancellation and late completion. Report defects with failing interleavings and proposed diffs.
Production files, tests, dependencies, workflows and deployments are outside the change scope.

| Surface                                     | Behavior to preserve                                                                        |
| ------------------------------------------- | ------------------------------------------------------------------------------------------- |
| Provider, Codex, DeepSeek and paid fallback | Provider order, quota/accounting settlement, delivery receipts, fail-fast sibling semantics |
| Chat, Harmony and embeddings                | Stream iteration and cancellation, handler/job ownership through completion                 |
| Live and supervisor                         | Callback registration and removal, socket/stream shutdown, delivery and abort ownership     |

Owned paths are this report and `analysis/lifetime/c/` under `cpp/gateway`. Runtime evidence is
limited to credential-free Linux ARM64 local mocks. Each execution uses an empty home and fresh
temporary directory. No real provider traffic. The start ref named in the assignment and the local
`agent-job` checkout resolve to the same source identity.

Acceptance requires every file ticked, matching verdicts in this report and `sites.json`, and a
ranked handoff with counts and proposed diffs. Use the current `PORTING.md` ownership rules,
`docs/CI.md` for sanitizer commands, and the 2026-11-17 clang-tidy report as prior evidence.
Select the five requested clang-tidy checks through `tools/tidy.sh`, supplement with
`rg` and manual caller tracing, and run focused ASan witnesses where cheap. Validate the
report/JSON correspondence and `git diff --check`. Keep all generated artifacts outside
the checkout. Commit after each file, with a 15-minute maximum between checkpoints.

Tool setup is capped at 20 minutes. Use at most four concurrent external build/analyzer workers
and bound each analyzer TU at 90 seconds. Save progress in an external receipt. If setup or a TU
fails, record the outcome and continue manual classification. Dependency availability, clang-tidy
runtime and witness feasibility are open. Finish when the audit and handoff reconcile or record the
precise remaining scope if interrupted.

Audited source commit `6d28e858e7ab8b8b1036db4a4adfdade0465ed40`, tree `6abb54014cf79ba7c64b24be4d28dde250c40ccf`.

## File checklist (290)

- [x] `include/uos/chat/completions.hpp`
- [x] `include/uos/chat/envelope.hpp`
- [x] `include/uos/chat/stream_translation.hpp`
- [x] `include/uos/codex/account_routing.hpp`
- [x] `include/uos/codex/auth.hpp`
- [x] `include/uos/codex/auth_refresh.hpp`
- [x] `include/uos/codex/auth_repair.hpp`
- [x] `include/uos/codex/banked_reset.hpp`
- [x] `include/uos/codex/banked_reset_claim.hpp`
- [x] `include/uos/codex/banked_reset_pool.hpp`
- [x] `include/uos/codex/banked_reset_provider.hpp`
- [x] `include/uos/codex/banked_reset_submission.hpp`
- [x] `include/uos/codex/capacity_routing.hpp`
- [x] `include/uos/codex/dispatch.hpp`
- [x] `include/uos/codex/experiment.hpp`
- [x] `include/uos/codex/external_auth.hpp`
- [x] `include/uos/codex/index.hpp`
- [x] `include/uos/codex/manual_reset.hpp`
- [x] `include/uos/codex/manual_reset_state.hpp`
- [x] `include/uos/codex/native_auth.hpp`
- [x] `include/uos/codex/native_auth_transport.hpp`
- [x] `include/uos/codex/overage_settings.hpp`
- [x] `include/uos/codex/quota.hpp`
- [x] `include/uos/codex/rate_limit_429.hpp`
- [x] `include/uos/codex/reset_settings.hpp`
- [x] `include/uos/codex/responses_error.hpp`
- [x] `include/uos/codex/responses_operations.hpp`
- [x] `include/uos/codex/responses_runners.hpp`
- [x] `include/uos/codex/responses_state.hpp`
- [x] `include/uos/codex/routing_evaluation.hpp`
- [x] `include/uos/codex/routing_mutations.hpp`
- [x] `include/uos/codex/routing_serial.hpp`
- [x] `include/uos/codex/routing_state.hpp`
- [x] `include/uos/codex/session.hpp`
- [x] `include/uos/codex/supervisor.hpp`
- [x] `include/uos/codex/supervisor_brief.hpp`
- [x] `include/uos/codex/supervisor_config.hpp`
- [x] `include/uos/codex/supervisor_follow.hpp`
- [x] `include/uos/codex/supervisor_inventory.hpp`
- [x] `include/uos/codex/supervisor_log.hpp`
- [x] `include/uos/codex/supervisor_secret.hpp`
- [x] `include/uos/codex/supervisor_transport.hpp`
- [x] `include/uos/deepseek/chat_projection.hpp`
- [x] `include/uos/deepseek/context_admission_failure.hpp`
- [x] `include/uos/deepseek/flash_aliases.hpp`
- [x] `include/uos/deepseek/forwarded_payload_policy.hpp`
- [x] `include/uos/deepseek/handlers.hpp`
- [x] `include/uos/deepseek/index.hpp`
- [x] `include/uos/deepseek/responses.hpp`
- [x] `include/uos/deepseek/responses_payload.hpp`
- [x] `include/uos/deepseek/responses_stream.hpp`
- [x] `include/uos/deepseek/stream.hpp`
- [x] `include/uos/deepseek/waterfall.hpp`
- [x] `include/uos/deepseek/waterfall_handler.hpp`
- [x] `include/uos/embeddings/handlers.hpp`
- [x] `include/uos/embeddings/jobs.hpp`
- [x] `include/uos/embeddings/ledger.hpp`
- [x] `include/uos/embeddings/voyage.hpp`
- [x] `include/uos/harmony/adapter.hpp`
- [x] `include/uos/harmony/classifier.hpp`
- [x] `include/uos/harmony/conversation.hpp`
- [x] `include/uos/harmony/parse.hpp`
- [x] `include/uos/harmony/probe_scenarios.hpp`
- [x] `include/uos/harmony/probes.hpp`
- [x] `include/uos/harmony/reliability/context.hpp`
- [x] `include/uos/harmony/reliability/failure.hpp`
- [x] `include/uos/harmony/reliability/feedback.hpp`
- [x] `include/uos/harmony/reliability/harness.hpp`
- [x] `include/uos/harmony/reliability/hash.hpp`
- [x] `include/uos/harmony/reliability/loops.hpp`
- [x] `include/uos/harmony/reliability/retry.hpp`
- [x] `include/uos/harmony/reliability/state.hpp`
- [x] `include/uos/harmony/reliability/surfaces.hpp`
- [x] `include/uos/harmony/reliability/verify.hpp`
- [x] `include/uos/harmony/render.hpp`
- [x] `include/uos/harmony/tools/backend.hpp`
- [x] `include/uos/harmony/tools/fakes.hpp`
- [x] `include/uos/harmony/tools/result.hpp`
- [x] `include/uos/harmony/tools/router.hpp`
- [x] `include/uos/harmony/tools/schemas.hpp`
- [x] `include/uos/harmony/types.hpp`
- [x] `include/uos/live/calls.hpp`
- [x] `include/uos/live/handler.hpp`
- [x] `include/uos/live/routes.hpp`
- [x] `include/uos/live/sideband.hpp`
- [x] `include/uos/live/upstream.hpp`
- [x] `include/uos/paid_fallback/claude_code_route.hpp`
- [x] `include/uos/paid_fallback/health.hpp`
- [x] `include/uos/paid_fallback/index.hpp`
- [x] `include/uos/paid_fallback/ledger_admission.hpp`
- [x] `include/uos/paid_fallback/ledger_backfill.hpp`
- [x] `include/uos/paid_fallback/ledger_settlement.hpp`
- [x] `include/uos/paid_fallback/ledger_state.hpp`
- [x] `include/uos/paid_fallback/ledger_stats.hpp`
- [x] `include/uos/paid_fallback/removed_provider.hpp`
- [x] `include/uos/paid_fallback/rollups.hpp`
- [x] `include/uos/paid_fallback/routing.hpp`
- [x] `include/uos/provider/anthropic_chat_responses.hpp`
- [x] `include/uos/provider/anthropic_chat_stream.hpp`
- [x] `include/uos/provider/anthropic_replay.hpp`
- [x] `include/uos/provider/anthropic_response_wire.hpp`
- [x] `include/uos/provider/anthropic_responses.hpp`
- [x] `include/uos/provider/capacity.hpp`
- [x] `include/uos/provider/capacity_analytics.hpp`
- [x] `include/uos/provider/capacity_contract.hpp`
- [x] `include/uos/provider/capacity_events.hpp`
- [x] `include/uos/provider/capacity_history.hpp`
- [x] `include/uos/provider/capacity_parse.hpp`
- [x] `include/uos/provider/capacity_rollups.hpp`
- [x] `include/uos/provider/cerebras.hpp`
- [x] `include/uos/provider/cerebras_handlers.hpp`
- [x] `include/uos/provider/cerebras_rate_limits.hpp`
- [x] `include/uos/provider/cerebras_responses.hpp`
- [x] `include/uos/provider/claude_capacity_history.hpp`
- [x] `include/uos/provider/claude_capacity_rollups.hpp`
- [x] `include/uos/provider/claude_code.hpp`
- [x] `include/uos/provider/claude_code_access.hpp`
- [x] `include/uos/provider/claude_code_accounts.hpp`
- [x] `include/uos/provider/claude_code_credentials.hpp`
- [x] `include/uos/provider/claude_code_fwd.hpp`
- [x] `include/uos/provider/claude_code_models.hpp`
- [x] `include/uos/provider/claude_code_quota.hpp`
- [x] `include/uos/provider/claude_code_usage.hpp`
- [x] `include/uos/provider/claude_code_usage_fwd.hpp`
- [x] `include/uos/provider/claude_code_usage_refresh.hpp`
- [x] `include/uos/provider/claude_code_usage_schema.hpp`
- [x] `include/uos/provider/health.hpp`
- [x] `include/uos/provider/lithos.hpp`
- [x] `include/uos/provider/lithos_handlers.hpp`
- [x] `include/uos/provider/lithos_models.hpp`
- [x] `include/uos/provider/lithos_rate_limits.hpp`
- [x] `include/uos/provider/lithos_streams.hpp`
- [x] `include/uos/provider/metered.hpp`
- [x] `include/uos/provider/openrouter.hpp`
- [x] `include/uos/provider/openrouter_handlers.hpp`
- [x] `include/uos/provider/openrouter_request_controls.hpp`
- [x] `include/uos/provider/openrouter_streams.hpp`
- [x] `include/uos/provider/openrouter_web_tools.hpp`
- [x] `include/uos/provider/presentation.hpp`
- [x] `include/uos/provider/removed_provider_circuit.hpp`
- [x] `include/uos/provider/removed_provider_telemetry.hpp`
- [x] `include/uos/provider/selection.hpp`
- [x] `include/uos/provider/stream_relay.hpp`
- [x] `include/uos/provider/surplus.hpp`
- [x] `include/uos/provider/surplus_claude_chat_request.hpp`
- [x] `include/uos/provider/surplus_claude_request.hpp`
- [x] `src/chat/completions.cpp`
- [x] `src/chat/envelope.cpp`
- [x] `src/chat/stream_translation.cpp`
- [x] `src/codex/account_routing.cpp`
- [x] `src/codex/auth.cpp`
- [x] `src/codex/auth_refresh.cpp`
- [x] `src/codex/auth_repair.cpp`
- [x] `src/codex/banked_reset.cpp`
- [x] `src/codex/banked_reset_claim.cpp`
- [x] `src/codex/banked_reset_pool.cpp`
- [x] `src/codex/banked_reset_provider.cpp`
- [x] `src/codex/banked_reset_submission.cpp`
- [x] `src/codex/capacity_routing.cpp`
- [x] `src/codex/dispatch.cpp`
- [x] `src/codex/experiment.cpp`
- [x] `src/codex/external_auth.cpp`
- [x] `src/codex/index.cpp`
- [x] `src/codex/manual_reset.cpp`
- [x] `src/codex/manual_reset_state.cpp`
- [x] `src/codex/native_auth.cpp`
- [x] `src/codex/native_auth_transport.cpp`
- [x] `src/codex/overage_settings.cpp`
- [x] `src/codex/quota.cpp`
- [x] `src/codex/rate_limit_429.cpp`
- [x] `src/codex/reset_settings.cpp`
- [x] `src/codex/responses_error.cpp`
- [x] `src/codex/responses_operations.cpp`
- [x] `src/codex/responses_runners.cpp`
- [x] `src/codex/responses_state.cpp`
- [x] `src/codex/routing_evaluation.cpp`
- [x] `src/codex/routing_mutations.cpp`
- [x] `src/codex/routing_serial.cpp`
- [x] `src/codex/routing_state.cpp`
- [x] `src/codex/session.cpp`
- [x] `src/codex/supervisor.cpp`
- [x] `src/codex/supervisor_brief.cpp`
- [x] `src/codex/supervisor_config.cpp`
- [x] `src/codex/supervisor_follow.cpp`
- [x] `src/codex/supervisor_inventory.cpp`
- [x] `src/codex/supervisor_log.cpp`
- [x] `src/codex/supervisor_secret.cpp`
- [x] `src/codex/supervisor_transport.cpp`
- [x] `src/deepseek/chat_projection.cpp`
- [x] `src/deepseek/context_admission_failure.cpp`
- [x] `src/deepseek/flash_aliases.cpp`
- [x] `src/deepseek/forwarded_payload_policy.cpp`
- [x] `src/deepseek/handlers.cpp`
- [x] `src/deepseek/index.cpp`
- [x] `src/deepseek/responses.cpp`
- [x] `src/deepseek/responses_payload.cpp`
- [x] `src/deepseek/responses_stream.cpp`
- [x] `src/deepseek/stream.cpp`
- [x] `src/deepseek/waterfall.cpp`
- [x] `src/deepseek/waterfall_handler.cpp`
- [x] `src/embeddings/handlers.cpp`
- [x] `src/embeddings/jobs.cpp`
- [x] `src/embeddings/ledger.cpp`
- [x] `src/embeddings/voyage.cpp`
- [x] `src/harmony/adapter.cpp`
- [x] `src/harmony/classifier.cpp`
- [x] `src/harmony/conversation.cpp`
- [x] `src/harmony/parse.cpp`
- [x] `src/harmony/probe_scenarios.cpp`
- [x] `src/harmony/probes.cpp`
- [x] `src/harmony/reliability/context.cpp`
- [x] `src/harmony/reliability/failure.cpp`
- [x] `src/harmony/reliability/feedback.cpp`
- [x] `src/harmony/reliability/harness.cpp`
- [x] `src/harmony/reliability/hash.cpp`
- [x] `src/harmony/reliability/loops.cpp`
- [x] `src/harmony/reliability/retry.cpp`
- [x] `src/harmony/reliability/state.cpp`
- [x] `src/harmony/reliability/surfaces.cpp`
- [x] `src/harmony/reliability/verify.cpp`
- [x] `src/harmony/render.cpp`
- [x] `src/harmony/tools/backend.cpp`
- [x] `src/harmony/tools/fakes.cpp`
- [x] `src/harmony/tools/result.cpp`
- [x] `src/harmony/tools/router.cpp`
- [x] `src/harmony/tools/schemas.cpp`
- [x] `src/harmony/types.cpp`
- [x] `src/live/calls.cpp`
- [x] `src/live/handler.cpp`
- [x] `src/live/routes.cpp`
- [x] `src/live/sideband.cpp`
- [x] `src/live/upstream.cpp`
- [x] `src/paid_fallback/claude_code_route.cpp`
- [x] `src/paid_fallback/health.cpp`
- [x] `src/paid_fallback/index.cpp`
- [x] `src/paid_fallback/ledger_admission.cpp`
- [x] `src/paid_fallback/ledger_backfill.cpp`
- [x] `src/paid_fallback/ledger_settlement.cpp`
- [x] `src/paid_fallback/ledger_state.cpp`
- [x] `src/paid_fallback/ledger_stats.cpp`
- [x] `src/paid_fallback/removed_provider.cpp`
- [x] `src/paid_fallback/rollups.cpp`
- [x] `src/paid_fallback/routing.cpp`
- [x] `src/provider/anthropic_chat_responses.cpp`
- [x] `src/provider/anthropic_chat_stream.cpp`
- [x] `src/provider/anthropic_replay.cpp`
- [x] `src/provider/anthropic_response_wire.cpp`
- [x] `src/provider/anthropic_responses.cpp`
- [x] `src/provider/capacity.cpp`
- [x] `src/provider/capacity_analytics.cpp`
- [x] `src/provider/capacity_contract.cpp`
- [x] `src/provider/capacity_events.cpp`
- [x] `src/provider/capacity_history.cpp`
- [x] `src/provider/capacity_parse.cpp`
- [x] `src/provider/capacity_rollups.cpp`
- [x] `src/provider/cerebras.cpp`
- [x] `src/provider/cerebras_handlers.cpp`
- [x] `src/provider/cerebras_rate_limits.cpp`
- [x] `src/provider/cerebras_responses.cpp`
- [x] `src/provider/claude_capacity_history.cpp`
- [x] `src/provider/claude_capacity_rollups.cpp`
- [x] `src/provider/claude_code.cpp`
- [x] `src/provider/claude_code_access.cpp`
- [x] `src/provider/claude_code_accounts.cpp`
- [x] `src/provider/claude_code_credentials.cpp`
- [x] `src/provider/claude_code_models.cpp`
- [x] `src/provider/claude_code_quota.cpp`
- [x] `src/provider/claude_code_usage.cpp`
- [x] `src/provider/claude_code_usage_refresh.cpp`
- [x] `src/provider/claude_code_usage_schema.cpp`
- [x] `src/provider/health.cpp`
- [x] `src/provider/lithos.cpp`
- [x] `src/provider/lithos_handlers.cpp`
- [x] `src/provider/lithos_models.cpp`
- [x] `src/provider/lithos_rate_limits.cpp`
- [x] `src/provider/lithos_streams.cpp`
- [x] `src/provider/metered.cpp`
- [x] `src/provider/openrouter.cpp`
- [x] `src/provider/openrouter_handlers.cpp`
- [x] `src/provider/openrouter_request_controls.cpp`
- [x] `src/provider/openrouter_streams.cpp`
- [x] `src/provider/openrouter_web_tools.cpp`
- [x] `src/provider/presentation.cpp`
- [x] `src/provider/removed_provider_circuit.cpp`
- [x] `src/provider/removed_provider_telemetry.cpp`
- [x] `src/provider/selection.cpp`
- [x] `src/provider/stream_relay.cpp`
- [x] `src/provider/surplus.cpp`
- [x] `src/provider/surplus_claude_chat_request.cpp`
- [x] `src/provider/surplus_claude_request.cpp`

## Audit method and retained evidence

The inventory covers 144 implementations and 146 headers. Each row records a site or one group of
sites with the same ownership proof. Diffs in the final ranked list are maintainer proposals.

The manual review followed task factories and virtual implementations in addition to coroutine
bodies. It checked the runtime implementations of Promise reactions and loop teardown,
`rt::async`, `rt::Fetch`, stream source algorithms, generator operations, and abort listeners.
This distinguishes a closure retained by its adapter from a frame or referent an escaping child
merely borrows. The fail-fast review includes an independently pending sibling.

From `cpp/gateway`, the initial discovery probes were equivalent to these searches. A
coroutine can span several lines or arrive through an interface, so the search output was
supplemented by source and caller review.

```bash
area=({src,include/uos}/{provider,codex,deepseek,paid_fallback,harmony,chat,embeddings,live})
rg -n 'co_(await|return|yield)' "${area[@]}"
rg -n -U '\[[^\]\n]*\]\s*\([^;{}]{0,350}\)\s*(?:mutable\s*)?->\s*(?:rt::)?Task\b' "${area[@]}"
rg -n 'rt::(all|allSettled|race|any|spawn|async)\b|\.then\(|\.catch_\(|\.finally_\(' "${area[@]}"
rg -n 'addEventListener|set(On|Timeout|Interval)|\.pull\s*=|\.cancel\s*=|\[this|\bweak_from_this\b' "${area[@]}"
rg -n -U '\b(?:auto|const auto)\s*(?:&&?\s*)?\[[^\n\]]+\]\s*=\s*co_await' "${area[@]}"
rg -n -U '\(\s*co_await [^;]+\)\s*[.\[]|\b[a-zA-Z_]\w*\(\s*co_await' "${area[@]}"
rg -n 'co_await [^;]*\{' "${area[@]}"
```

Both searches for direct structured bindings and direct use of an awaited result returned
zero matches. The braced-expression search also finds loop bodies that open on the same line.
Those are distinct from an aggregate supplied to a coroutine inside an await expression.
The final check matched ledger ranges against 1,760 await lines, 101 combinator or start
lines, 38 continuation lines and 48 callback registration lines after omitting standalone
comment lines. The same line can match two searches.

### Toolchain and clang-tidy

Environment was Ubuntu 24.04.5 ARM64 with four CPUs. The retained evidence root is
`/var/tmp/cppgw-lifetime-c.O1ER9aAI`. The saved `setup.sh` records the package and source
build commands and verifies archive hashes before extraction. It used the apt.llvm.org Noble
LLVM 21 repository and installed Clang/clang-tidy 21.1.8, the matching sanitizer runtime, mold,
PyYAML and the system development libraries. OpenSSL was 3.0.13, zlib 1.3, Brotli 1.1.0 and
Zstd 1.5.5. The staged ICU was 78.3. All downloaded sources and binaries were staged outside
the checkout.

```sh
# Download and extraction destinations are under the evidence root.
sudo -n apt-get install -y libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev pkg-config python3-yaml
sudo -n apt-get install -y clang-21 clang-tidy-21 libclang-rt-21-dev mold
# https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2
./bootstrap.sh --with-libraries=json,container
./b2 -j2 --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 stage
# https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz
./configure --prefix="$PWD/../sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install
# https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz
../icu/source/configure --prefix="$PWD/../icu78" --disable-tests --disable-samples --disable-extras --disable-static
make -j1
make install
```

Boost JSON and Container static libraries were present. The setup measured 29.22 s for
LLVM/system packages, 96.64 s for Boost, 63.43 s for SQLite, and 268.51 s for ICU. These
component measurements overlap. The SQLite probe inside the fixture boundary printed 3.54.0.

The analyzer used a focused copy of the repository configuration with the five requested
checks, a first-party header filter, and no fix application. The external clang-tidy wrapper
appends `--extra-arg=-isystem<dir>` for the staged Boost, ICU and SQLite headers.
`tidy-c/manifest.json` records the configuration, C++ inputs and exact tool identity.
The selected-source invocation can be reproduced from the repository root.

```yaml
Checks: "-*,cppcoreguidelines-avoid-capturing-lambda-coroutines,cppcoreguidelines-avoid-reference-coroutine-parameters,misc-coroutine-hostile-raii,bugprone-dangling-handle,bugprone-use-after-move"
InheritParentConfig: false
WarningsAsErrors: ""
HeaderFilterRegex: "(^|/)cpp/gateway/(include|src|tests)/"
SystemHeaders: false
FormatStyle: none
CheckOptions:
  bugprone-dangling-handle.HandleClasses: "std::basic_string_view;std::span"
  misc-coroutine-hostile-raii.RAIITypesList: "std::lock_guard;std::scoped_lock"
```

```bash
evidence=/var/tmp/cppgw-lifetime-c.O1ER9aAI
launch=$(mktemp -d "$evidence/launch-tidy.XXXXXXXX")
mkdir "$launch/home" "$launch/tmp"
mapfile -t files < <(cd cpp/gateway && rg --files src/{provider,codex,deepseek,paid_fallback,harmony,chat,embeddings,live} -g '*.cpp' | sort)
arguments=()
for path in "${files[@]}"; do arguments+=(--source "$path"); done
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
    HOME="$launch/home" TMPDIR="$launch/tmp" \
    bash cpp/gateway/tools/tidy.sh --compiler /usr/bin/clang++-21 \
    --clang-tidy "$evidence/bin/clang-tidy-21" --config "$evidence/area-c.clang-tidy" \
    --jobs 3 --timeout 90 --build-dir "$launch/tidy-c" "${arguments[@]}"
```

Retained run `tidy-c/tidy-baseline.json` names commit
`9e335787a113208beb1b7a9e5193c13ece6d927c` and tree
`498811f1a546f663547e1bd9efc2e5057ca8d95a`. Its SHA-256 is
`62f11eae4d2dd996f52b7cdc9075d24fc34f2a12dcf65f66d841726cf114f8e0`.
All 144 selected TUs completed, with zero frontend failures and timeouts, in 279.301 s
using three workers. Maximum TU RSS was 761,036 KiB. Sixteen TUs emitted findings,
47 emissions and 43 unique diagnostics. The scope is all Area C implementation TUs.
The wrapper marks whole-gateway completion false because its database also contains
the unselected modules and tests.

| Check                                                    | Unique diagnostics |
| -------------------------------------------------------- | -----------------: |
| `cppcoreguidelines-avoid-capturing-lambda-coroutines`    |                  9 |
| `cppcoreguidelines-avoid-reference-coroutine-parameters` |                 34 |
| `misc-coroutine-hostile-raii`                            |                  0 |
| `bugprone-dangling-handle`                               |                  0 |
| `bugprone-use-after-move`                                |                  0 |

Every unique diagnostic is covered by a ledger row. The nine visible capturing coroutine
lambdas have owners in their current call chains. The reference warning on the Voyage
eviction helper is covered by the fail-fast KV finding. Several warnings share a line when
different parameters are declared there. The remaining request-path defects came from
manual caller, combinator or callback tracing.

### Focused ASan/UBSan evidence

The portable driver used Clang 21.1.8 with bundled ASan, UBSan and LSan, four build/test workers,
and local fixtures behind real Linux network, mount and PID namespaces. The fixture entry
proved loopback and outside route denial, created per-run empty HOME and fresh TMPDIR, and
passed an allowlisted environment. The four prior-run receipts are incremental builds and
have unchanged native input digests. Cold and interrupted runs did not yield test receipts.

| Driver filter                 | Build s | Test wall s | Elapsed s | Passed cases |
| ----------------------------- | ------: | ----------: | --------: | -----------: |
| `live_sideband_http_test`     |  52.826 |       4.852 |    58.127 |        21/21 |
| `codex_dispatch_differential` |  28.149 |       0.980 |    29.579 |        13/13 |
| `codex_supervisor_test`       |  13.642 |       2.365 |    16.459 |        15/15 |
| `paid_fallback_test`          |  38.778 |       2.888 |    42.113 |        25/25 |

The live receipt is from commit `4a308e48d62f24f6a1b99598e79279a4f5f5816a`
and tree `cc835dd1e8394cec6aa60cb41e992fb80f019192`. The other three are from commit
`947c56a50007becf0be87267b265e93c801eec25` and tree
`ea98c25be2f6662c16ebd329a77bd96fdb9a9d9d`. Native source digest
`4487834eff106fc599066c9688b3f5231e9a6df888d521efe898af1aac6d3aa0`
is shared across the four receipts. The Area C source is identical to the audited tree.

```bash
evidence=/var/tmp/cppgw-lifetime-c.O1ER9aAI
for target in live_sideband_http_test codex_dispatch_differential codex_supervisor_test paid_fallback_test; do
  launch=$(mktemp -d "$evidence/launch-asan.XXXXXXXX")
  mkdir "$launch/home" "$launch/tmp"
  env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
    HOME="$launch/home" TMPDIR="$launch/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=4 \
    UOS_CI_BUILD_DIR="$evidence/asan" UOS_CI_SQLITE_PREFIX="$evidence/sqlite" \
    PKG_CONFIG_PATH="$evidence/icu78/lib/pkgconfig" UOS_CI_CXXFLAGS="-isystem $evidence/boost_1_92_0" \
    UOS_CI_LDFLAGS="-L$evidence/boost_1_92_0/stage/lib -L$evidence/icu78/lib -Wl,-rpath,$evidence/icu78/lib -licudata" \
    bash cpp/gateway/tools/ci-build.sh --config asan "$target"
done
```

The continuation ran five more filters against checkpoint
`a87e8fa107266aa9e3c49f2c9930093d516a4ea8` and tree
`fe235d73fe4d2870e8a80dc8904d4512fe616dc4`. Use the same command above with these
test names plus `.cpp`, add `UOS_CI_TIMEOUT=120` to the environment, and prefix
`bash cpp/gateway/tools/ci-build.sh` with `timeout -k 5s 240s`. Each receipt has the
same native input digest and 369/369 production objects accounted for.

| Driver filter                                     | Build s | Test wall s | Elapsed s | Passed cases |
| ------------------------------------------------- | ------: | ----------: | --------: | -----------: |
| `oss_stream_lifecycle_test.cpp`                   |  35.274 |       2.352 |    38.080 |        20/20 |
| `provider_selection_health_differential_test.cpp` |  18.287 |       0.623 |    19.360 |        10/10 |
| `embeddings_ledger_differential_test.cpp`         |  20.286 |       3.607 |    24.396 |          7/7 |
| `surplus_test.cpp`                                |  11.052 |       0.619 |    12.120 |          6/6 |
| `metered_test.cpp`                                |  15.144 |       0.622 |    16.215 |          8/8 |

Across the nine driver filters, 125 of 125 cases passed with zero sanitizer failures,
build failures or timeouts. The focused suite results apply to those cases. The
individual witnesses below exercise abandoned operations that the passing cases do
not reach.

### Local lifetime witnesses

The off-tree `witness/ownership.cpp` (SHA-256
`d4b1abf4b86ea21be38f3e1957b3e8a355f433d30c7538c079173833efbc75f7`) uses mocked
pending promises and invokes the two Codex wait APIs directly. `witness/build.log`
retains the exact Clang 21 command that compiled this file and linked the sanitized
`codex_dispatch_differential_test` production dependency closure. It operated on the
same native sources as the driver receipts. The executable takes the `retry` argument for
the retry observation. The saved `witness/bin/retry` wrapper supplies that argument. The
Linux namespace helper runs both commands with clean environment and verifies isolation.

```bash
evidence=/var/tmp/cppgw-lifetime-c.O1ER9aAI
for target in ownership retry; do
  scratch=$(mktemp -d /tmp/cppgw-witness.XXXXXXXX)
  parent_namespace=$(readlink /proc/self/ns/net)
  sudo -n unshare --net --mount --pid --fork --mount-proc --propagation private --kill-child=SIGKILL -- \
    /usr/bin/python3 -B cpp/gateway/cmake/ci/isolate.py "$scratch" "$evidence/witness/bin" "$target" \
    "$parent_namespace" "$(id -u)" "$(id -g)" \
    '{"PATH":"/usr/bin:/bin","LANG":"C.UTF-8","ASAN_OPTIONS":"detect_leaks=1:abort_on_error=1","UBSAN_OPTIONS":"halt_on_error=1"}'
  rmdir "$scratch"
done
```

- Shared refresh. A weak observer of the registering signal remains live after the
  scoped Loop is destroyed with shared work pending. LeakSanitizer reported 776 bytes
  in seven allocations and the trace includes `auth_refresh.cpp:539`.
  Receipt `witness/ownership.log` has SHA-256
  `b1be2862fb7bd0d5756acba34a14556dfe4d43cc53e26bf3c6af091ad7fbf7aa`.
- Retry wait. After abort rejects the raced wait, a capture's weak owner count shows
  the injected sleeper closure has been released while its gate is still pending.
  The witness exits successfully after observing the ownership failure and leaves the
  losing task suspended. Receipt `witness/retry.log` has SHA-256
  `e81b7b7647464f575a0677749e1248b6954caeb6aa10111d581a51d0f7956b3a`.

## Per-file verdicts

### `include/uos/chat/completions.hpp`

The stream and buffered entry points have separate ownership of telemetry.

- 14-17 · stream construction · **SAFE** · The synchronous entry point copies the pointed-to context and retains its terminal callback in shared stream state.
- 18-22 · borrowed context · **SAFE** · The buffered caller in envelope.cpp awaits the task with its context live, and the task copies that context before processing events.

### `include/uos/chat/envelope.hpp`

Both public entry points are synchronous task factories despite their Task return types.

- 6-7 · task factories · **SAFE** · The wrappers copy the optional context into the coroutine arguments before returning, and the telemetry dispatcher owns its request callback.

### `include/uos/chat/stream_translation.hpp`

Translation and accumulator methods are synchronous except for the completion recorder.

- 56-75 · container ownership · **SAFE** · Accumulator results own shared call records. Consumers read the exposed calls vector synchronously or copy its size before suspending.
- 83-84 · stream factory · **SAFE** · The replay iterator owns its generator body and prepared state, and the returned cancel function captures copies of the source and iterator.
- 90-92 · borrowed context · **SAFE** · Stream or buffered completion state retains its copied context through the awaited recorder.

### `include/uos/codex/account_routing.hpp`

Routing decisions accept snapshots and keys by value.

- 46-48,57,68-83,90 · owning task parameters · **SAFE** · The election, refresh, selection and slot recheck frames own their account, pool, model and time inputs.

### `include/uos/codex/auth.hpp`

The async API owns response handles, while the shared caches are reviewed at mutation sites in auth.cpp and auth_refresh.cpp.

- 203-208 · shared registries · **SAFE** · The namespace owns the promise maps and uses weak response identities; consumers copy results or handles before awaiting.
- 230,238-244,250 · owning task parameters · **SAFE** · Each probe or lookup coroutine owns its response or scalar lookup inputs.

### `include/uos/codex/auth_refresh.hpp`

Refresh helper interfaces transfer owned task inputs.

- 18,23,30,43-49 · owning task parameters · **SAFE** · Account data, promises and optional signals are copied or moved into the task frames.

### `include/uos/codex/auth_repair.hpp`

Synchronous repair planning, token metadata and JSON conversion declarations. Their reference and view parameters have call-local use.

### `include/uos/codex/banked_reset.hpp`

Reset records own their data and hook callables.

- 84-86,151-166 · task and callback interfaces · **SAFE** · The usage gate owns the store and candidate authorization; dependencies copied into reset operations keep the provider and telemetry/hash functions alive.

### `include/uos/codex/banked_reset_claim.hpp`

The claim API uses owned arguments for the optimistic KV operations.

- 28-29,110,121,124-136,146-148,215-227 · owning task parameters · **SAFE** · Each operation retains KV, its candidate and fence data, and any clock, hash or mutator callable it invokes.

### `include/uos/codex/banked_reset_pool.hpp`

Pool evaluation and shadow read declarations.

- 17-19,27-28 · owning task parameters · **SAFE** · The returned tasks own the optional store override or candidate array and evaluation dependencies.

### `include/uos/codex/banked_reset_provider.hpp`

Reset provider interface and read-only display APIs.

- 163-166 · member task interface · **SAFE** · The built-in upstream implementation synchronously copies its shared binding into free tasks; production callers also retain their provider handle.
- 184,279,282 · owning task parameters · **SAFE** · Display tasks own options including the clock/fetch dependencies, and their abort signal.

### `include/uos/codex/banked_reset_submission.hpp`

Dependency bundles and reset candidates cross this boundary by value.

- 16,24-25,31-32 · owning task parameters · **SAFE** · The KV open and submission/reconciliation frames retain their provider, store override, callbacks and candidates.

### `include/uos/codex/capacity_routing.hpp`

Async selection/persistence consumes owned snapshots; routing transforms are synchronous callables.

- 37-40,54,60,65,69,77,85-93,102,112-113 · owning task parameters · **SAFE** · Pool, observation, account and transform values stay in the receiving frames; a transform reads the supplied state only during invocation.

### `include/uos/codex/dispatch.hpp`

Dispatch interfaces transfer request and callback objects by value; async invocations are traced in the implementation.

- 36-54,74-80,103-121,137-146,155-157,174 · task parameters and callback interfaces · **SAFE** · Dispatch tasks retain their response/auth/request data and owned callback objects; actual callback referents are reviewed at the installation sites.
- 216-220,245-258,278-295 · retry and admission interfaces · **SAFE** · The serial driver owns its callbacks and the model classifiers own response handles; the retry race's branch lifetimes are reviewed in dispatch.cpp.

### `include/uos/codex/experiment.hpp`

Cache-scope experiment API and owned input records.

- 34,65,72-74,91 · owning task parameters · **SAFE** · The experiment frames own session, slot, request and options data across auth/routing work.

### `include/uos/codex/external_auth.hpp`

Synchronous external-auth decision and text/JSON declarations. No suspendable definition or asynchronous registration.

### `include/uos/codex/index.hpp`

Codex Responses entry point overloads.

- 15-16 · owning task parameters · **SAFE** · The forwarded request value and dispatch options become frame-owned inputs.

### `include/uos/codex/manual_reset.hpp`

Manual reset owns its dependency bundle through the operation.

- 23-26,35-37 · task parameters and callbacks · **SAFE** · Reset frames own the hash, dependencies, provider factory and signal used for the awaited operation.

### `include/uos/codex/manual_reset_state.hpp`

Synchronous manual reset state and serialization declarations. No suspendable body or registration.

### `include/uos/codex/native_auth.hpp`

Native credential inputs are owned; installed hooks need an invocation-time owner.

- 22-26,31 · hook storage · **SAFE** · The hook bundle stores callable values. The native-auth implementation copies the bundle for asynchronous use.
- 38-50 · owning task parameters · **SAFE** · Credential, pool, JSON and path arguments are frame-owned, with owned values returned to the caller.

### `include/uos/codex/native_auth_transport.hpp`

Native refresh request interface.

- 36 · owning task parameters · **SAFE** · The refresh task owns the home, email and account identity strings for the duration of its IPC.

### `include/uos/codex/overage_settings.hpp`

Overage usage persistence and pool-loading API.

- 22-24,32 · owning task parameters · **SAFE** · Each store operation owns the KV handle and key, and pool loading owns its auth snapshot.

### `include/uos/codex/quota.hpp`

Synchronous quota parsing and record declarations. No suspendable body or registration.

### `include/uos/codex/rate_limit_429.hpp`

429 classification and routing updater API.

- 16-26 · owning task parameters · **SAFE** · Response and routing account handles are copied or moved to the receiving tasks before suspension.

### `include/uos/codex/reset_settings.hpp`

Usage settings and legacy migration API.

- 45,53 · owning task parameters · **SAFE** · The migration and usage read retain their stores and any account hash in their frames.

### `include/uos/codex/responses_error.hpp`

The synchronous error translation API takes borrowed event views and returns an owned event.

### `include/uos/codex/responses_operations.hpp`

Synchronous installer for dispatch operations.

- 9-11 · callback ownership · **SAFE** · The installer receives an owning handle during registration; index.cpp holds the request context while the registered operations are joined.

### `include/uos/codex/responses_runners.hpp`

Synchronous installer for request runners.

- 9-11 · callback ownership · **SAFE** · The installer receives an owning handle during registration; index.cpp holds the request context while the registered runners are joined.

### `include/uos/codex/responses_state.hpp`

This header defines the request-local context and callback signatures.

- 5-11,133-191,250-285 · callback ownership · **SAFE** · The context owns operation and runner functions; the enclosing fetch frame owns the context throughout the joined serial dispatch and the functions use its nonowning back-pointer.
- 303-305 · owning task parameters · **SAFE** · State creation owns prepared inputs, options and the dispatch coordinator while assembling an independently owned context.

### `include/uos/codex/routing_evaluation.hpp`

Synchronous evaluation and aggregation types and methods. No deferred borrowing.

### `include/uos/codex/routing_mutations.hpp`

Routing mutations own command inputs; fence functions synchronously examine supplied values.

- 18-43,61,75,91-107 · task parameters and fence interfaces · **SAFE** · Mutation frames retain response, account, pool and fence values; the transaction owns the rows read by fence callbacks.

### `include/uos/codex/routing_serial.hpp`

Serial routing APIs use owned snapshots.

- 37-43,57,70,90-92 · owning task parameters · **SAFE** · Evaluation, row read and selection tasks retain their snapshot or KV inputs.
- 77-82 · callback parameter · **SAFE** · Admission owns the action function and awaits its result before releasing its gate.

### `include/uos/codex/routing_state.hpp`

Synchronous value records, comparison and evaluation declarations. No coroutine or asynchronous callback registration.

### `include/uos/codex/session.hpp`

KV session observation interface.

- 41-42 · owning task parameters · **SAFE** · Both overloads copy or move key, client metadata and time into the receiving task.

### `include/uos/codex/supervisor.hpp`

Supervisor HTTP handlers take owned requests or no parameters.

- 9,17 · owning task parameters · **SAFE** · The output handler retains its request handle and sessions needs no caller-owned data.

### `include/uos/codex/supervisor_brief.hpp`

Brief collection exposes two synchronous copying task factories and an owning HTTP task.

- 124-129,159 · task factories · **SAFE** · The collection wrappers copy the connection and source into their implementation Tasks before returning; the handler owns its request.

### `include/uos/codex/supervisor_config.hpp`

The concurrency helper is executable template code; config and poll implementations are reviewed separately.

- 226 · poll interface · **SAFE** · The poll function consumes its signal handle by value; registration ownership is reviewed in supervisor_config.cpp.
- 243-270 · coroutine capture and fail-fast all · **SAFE** · Each rt::async worker owns its closure and shared State across sibling rejection; fixed-size input/result vectors stay alive and call sites supply value-capturing workers or free functions.
- 283-297 · callback and task interface · **SAFE** · Configuration resolution retains the options and the environment reader callable until its reads finish.

### `include/uos/codex/supervisor_follow.hpp`

Follow selection borrows only synchronously; polling owns its inputs.

- 66,81-82 · container and task ownership · **SAFE** · The selection helper finishes before returning, while readFollowUpdate retains a shared revision map and owns source, cursor and signal.

### `include/uos/codex/supervisor_inventory.hpp`

Inventory snapshot declaration.

- 108 · owning task result · **SAFE** · The refresh API takes no borrowed input and yields a copied snapshot from its shared cache.

### `include/uos/codex/supervisor_log.hpp`

Rollout tail reader interface.

- 58 · owning task parameters · **SAFE** · The task frame owns the input record and its path parameters.

### `include/uos/codex/supervisor_secret.hpp`

Synchronous secret validation declarations. No suspendable body or callback registration.

### `include/uos/codex/supervisor_transport.hpp`

The control connection is an owning pair of task factories.

- 25-28,43-44 · task factories and callbacks · **SAFE** · Open owns the socket path and signal; returned call and close callbacks synchronously move an owning state handle into the actual coroutine.

### `include/uos/deepseek/chat_projection.hpp`

Synchronous message projection declarations with call-local JSON borrows.

### `include/uos/deepseek/context_admission_failure.hpp`

Synchronous construction of context-admission failures. No suspendable body or registration.

### `include/uos/deepseek/flash_aliases.hpp`

Catalog aliases expose owned snapshot values. Background publication is reviewed in flash_aliases.cpp.

### `include/uos/deepseek/forwarded_payload_policy.hpp`

Payload projection and validation are synchronous.

- 77-78 · projection callback · **SAFE** · The caller retains the callback and the input object throughout the inline admission projection.

### `include/uos/deepseek/handlers.hpp`

The DeepSeek entry points retain their request payloads, with a borrowed telemetry context.

- 23-38 · borrowed context · **SAFE** · The chat/Responses callers keep the context in the joined handler frame, and the streaming branch copies it into relay state before returning.

### `include/uos/deepseek/index.hpp`

The dispatcher passes projected request data and callback objects as owning values.

- 198-202,291-292 · task and callback interface · **SAFE** · The options frame retains admission/timing/projection hooks; the projected JSON reference is used in a synchronous callback.

### `include/uos/deepseek/responses.hpp`

Synchronous Responses conversion declarations. The results own translated wire values.

### `include/uos/deepseek/responses_payload.hpp`

Synchronous construction of downstream payloads. No suspension or retained callback.

### `include/uos/deepseek/responses_stream.hpp`

A synchronous stateful adapter is returned by value.

- 17-40 · callback bundle · **SAFE** · Each callback owns the shared adapter state; push consumes its JSON argument during the call.

### `include/uos/deepseek/stream.hpp`

Synchronous stream event parsing declarations with owned delta results.

### `include/uos/deepseek/waterfall.hpp`

Planning callbacks are synchronous predicates.

- 38-39 · predicate interface · **SAFE** · The plan builder invokes each callback while its input and current provider string are live.

### `include/uos/deepseek/waterfall_handler.hpp`

The waterfall awaits each provider and paid-tail callback before attempting another hop.

- 16-30 · borrowed arguments and context · **SAFE** · The hop frame holds dispatch data and callback; the paid-tail implementation copies forward or joins the invoked task with its source values live.

### `include/uos/embeddings/handlers.hpp`

Embeddings handler API with optional telemetry.

- 8-9 · borrowed context · **SAFE** · Existing route callers join the handler with a live context, and the implementation copies it into owned telemetry dispatch state.

### `include/uos/embeddings/jobs.hpp`

Job API with a shared store and borrowed request telemetry.

- 6-25 · borrowed context and store ownership · **SAFE** · The create and get entry points are joined by their route, and job-attempt callers keep the telemetry context alive while the job retains its store.

### `include/uos/embeddings/ledger.hpp`

Idempotency and ledger declarations.

- 214-235 · owning task parameters · **SAFE** · Fingerprint, reservation and completion frames own their input vectors, lease, store and response handle.

### `include/uos/embeddings/voyage.hpp`

Embeddings cache/budget operations borrow KV where specified.

- 12-18,31-32 · borrowed store and owned signal · **SAFE** · Handlers and jobs keep the shared KV handle alive until each cache/budget task returns; the timer factory uses a signal value.
- 42-44,45-67 · owning task parameters · **SAFE** · Encryption and fetch frames own their inputs, provider callback bundle and any store handle.

### `include/uos/harmony/adapter.hpp`

HarmonyTransport implements an owning erased transport wrapper.

- 87-107 · callback lifetime · **SAFE** · The outer callable only forwards; keepAlive retains the function and its shared target until the returned coroutine completes.
- 133 · owning task parameters · **SAFE** · The turn task owns options and a transport handle while dispatching and normalizing the response.

### `include/uos/harmony/classifier.hpp`

Synchronous classifier request/projection declarations. No async registration or suspended borrowing.

### `include/uos/harmony/conversation.hpp`

Synchronous conversation construction and JSON values. No deferred borrowing.

### `include/uos/harmony/parse.hpp`

Synchronous wire parsing API with call-local views.

### `include/uos/harmony/probe_scenarios.hpp`

Scenario definitions store owned run functions.

- 17 · callback interface · **SAFE** · The scenario table retains the run functions, and each invocation passes its probe context by value.

### `include/uos/harmony/probes.hpp`

Probe context callbacks forward to task factories.

- 120-131 · callback bundle · **SAFE** · RunRaw invokes synchronously and the underlying probe tasks own their body, conversation and transport.

### `include/uos/harmony/reliability/context.hpp`

Synchronous reliability context records and projection declarations.

### `include/uos/harmony/reliability/failure.hpp`

Synchronous failure mapping and record declarations.

### `include/uos/harmony/reliability/feedback.hpp`

Synchronous feedback formatting declarations.

### `include/uos/harmony/reliability/harness.hpp`

The harness retains its options and event observer.

- 100-101,123 · callback and task interface · **SAFE** · The harness frame owns options; the observer sees each event during a synchronous call with its source value live.

### `include/uos/harmony/reliability/hash.hpp`

Synchronous reliability hashing declarations.

### `include/uos/harmony/reliability/loops.hpp`

Synchronous loop detection state and method declarations.

### `include/uos/harmony/reliability/retry.hpp`

Synchronous retry decisions and option records.

### `include/uos/harmony/reliability/state.hpp`

Synchronous reliability state and transition declarations.

### `include/uos/harmony/reliability/surfaces.hpp`

Synchronous surface selection and policy declarations.

### `include/uos/harmony/reliability/verify.hpp`

Synchronous reliability verification predicates and records.

### `include/uos/harmony/render.hpp`

Synchronous Harmony text rendering declaration. The returned text owns its bytes.

### `include/uos/harmony/tools/backend.hpp`

Backend interfaces include a member task for shell work.

- 81 · member task · **SAFE** · The awaited router frame retains the owning backend bundle until shell work finishes; the task's command and options are owned.

### `include/uos/harmony/tools/fakes.hpp`

The local fake implements the shell backend interface.

- 105 · member task · **SAFE** · The router retains the backend through the await, and the fake's shell inputs are values.

### `include/uos/harmony/tools/result.hpp`

Synchronous tool result types and construction helpers.

### `include/uos/harmony/tools/router.hpp`

Four overloads route owned tool work.

- 30-34 · task factories · **SAFE** · The pointer overload copies or converts the JSON argument synchronously before returning the implementation task, whose backend bundle is owning.

### `include/uos/harmony/tools/schemas.hpp`

Synchronous tool schema declarations.

### `include/uos/harmony/types.hpp`

Harmony value records and synchronous JSON conversions.

### `include/uos/live/calls.hpp`

Call-creation handler API.

- 8 · owning task parameters · **SAFE** · The create frame owns the request and principal identity.

### `include/uos/live/handler.hpp`

Live route and upgrade handler APIs.

- 11-12 · owning task parameters · **SAFE** · Both tasks retain the request and the routing or upgrade inputs by value.

### `include/uos/live/routes.hpp`

Synchronous route matcher. It uses method/path views within the call and returns an owned call ID.

### `include/uos/live/sideband.hpp`

Live sideband handler and upgrade factory API.

- 10-11 · owning task parameters · **SAFE** · The preparation and handler frames retain request, call ID and principal by value; the upgrade callbacks manage their socket ownership separately.

### `include/uos/live/upstream.hpp`

Live upstream helpers and mapping API.

- 36-38 · owning task parameters · **SAFE** · Mapping operations retain their IDs across KV awaits, including detached retirement.

### `include/uos/paid_fallback/claude_code_route.hpp`

The subscription attempt receives its body and options as values.

- 21 · owning task parameters · **SAFE** · The attempt owns the body and options while dispatching; telemetry referents are retained by its joined caller.

### `include/uos/paid_fallback/health.hpp`

Health reads, catalog operations and fallback routing API.

- 23,27-39,69-70,87-88 · task and callback interface · **SAFE** · Each task owns its response, signal or option data; bookkeeping awaits its owned callable before releasing it.

### `include/uos/paid_fallback/index.hpp`

Paid reservation and terminal accounting API.

- 85-86,103-125,156-157 · task and callback interface · **SAFE** · Accounting tasks own their reservations, delivery evidence and billing parameters; the test sweep hook is stored as a value.

### `include/uos/paid_fallback/ledger_admission.hpp`

Ledger admission, release and update declarations.

- 38-39,57-70 · owning task parameters · **SAFE** · The coroutines retain keys, reservation inputs, patches and any store override as values.

### `include/uos/paid_fallback/ledger_backfill.hpp`

Backfill and reconciliation API with a copied shared-work promise.

- 60-61,84-85,101-102,117,124-149 · owning task parameters · **SAFE** · The caller hands over store, options and reconciliation inputs by value; the scheduled-work accessor copies its promise handle.

### `include/uos/paid_fallback/ledger_settlement.hpp`

Settlement and lease APIs.

- 19-33,46-53,76 · owning task parameters · **SAFE** · Settlement/lease tasks retain the store and their request, result, lease and provider-log values throughout awaits.

### `include/uos/paid_fallback/ledger_state.hpp`

KV read and pending-gate APIs.

- 53,61-65,80-86,104-115 · owning task parameters · **SAFE** · Tasks own their store or override and all query keys, time bounds or limit values.

### `include/uos/paid_fallback/ledger_stats.hpp`

Ledger statistics API.

- 118,132,202 · owning task parameters · **SAFE** · The task frames own statistics inputs, options and store handles.

### `include/uos/paid_fallback/removed_provider.hpp`

Removed provider stub and compatibility helpers.

- 14-37,46-59 · synchronous promise factories · **SAFE** · The stub returns settled promises and stores no borrows of caller input; the identity projection is synchronous and rejects rvalues.

### `include/uos/paid_fallback/rollups.hpp`

Terminal rollup write/read API.

- 46-47,56 · owning task parameters · **SAFE** · Rollup frames own their inputs, optional store override and read options.

### `include/uos/paid_fallback/routing.hpp`

Paid waterfall API.

- 17-18 · owning task parameters · **SAFE** · The routing frame owns its request body and options; the joined parent owns any referenced telemetry.

### `include/uos/provider/anthropic_chat_responses.hpp`

Provider Chat normalization owns a response and its options.

- 27 · owning task parameters · **SAFE** · The normalizer retains the response handle and options in its coroutine frame.

### `include/uos/provider/anthropic_chat_stream.hpp`

The public stream factory is synchronous. The body and adapter state are retained by its returned stream.

### `include/uos/provider/anthropic_replay.hpp`

Replay metadata transformation is synchronous.

- 58 · clock callback · **SAFE** · Replay options hold a callable value used by synchronous normalization.

### `include/uos/provider/anthropic_response_wire.hpp`

Synchronous Anthropic wire event and output conversion declarations.

### `include/uos/provider/anthropic_responses.hpp`

Anthropic Messages adapter task interface.

- 36 · owning task parameters · **SAFE** · The normalization task owns its response handle and options.

### `include/uos/provider/capacity.hpp`

Capacity readers, samplers and handlers use owning arguments.

- 75-84,103-104,114-121 · owning task parameters · **SAFE** · Capacity snapshot tasks own stores, request handles and options during reads, refreshes and detached sampling.

### `include/uos/provider/capacity_analytics.hpp`

One remote observation writer and one snapshot reader.

- 70-74 · owning task parameters · **SAFE** · Both tasks own their store handle and writer or query inputs.

### `include/uos/provider/capacity_contract.hpp`

Capacity observation and configuration types.

- 291-300 · callback interface · **SAFE** · The snapshot options own their fetch, clock and lease owner callable objects; invocation lifetimes are traced in capacity.cpp.

### `include/uos/provider/capacity_events.hpp`

Event tasks and sample-trigger callback declarations.

- 76-115 · owning task parameters · **SAFE** · Event records/options and any overridden store are owned across the reads/writes.
- 120-124 · synchronous trigger · **SAFE** · The installed trigger consumes options on the call and dispatches a task constructed with its own values.

### `include/uos/provider/capacity_history.hpp`

Capacity history and recovery APIs.

- 33,59-61,71-72 · owning task parameters · **SAFE** · The reader/merger tasks retain their store and point/event input values.

### `include/uos/provider/capacity_parse.hpp`

Capacity parsing helpers are synchronous; there is one stored snapshot read.

- 31,87 · clock and task interface · **SAFE** · safeNow invokes its callable inline, and the snapshot coroutine owns KV while it awaits.

### `include/uos/provider/capacity_rollups.hpp`

Capacity rollup query API.

- 138 · owning task parameters · **SAFE** · The query owns the store and options across awaits.

### `include/uos/provider/cerebras.hpp`

Cerebras transport copies arguments and owns the provider option objects.

- 81-83,150-151 · task and callback interface · **SAFE** · The task retains the request and dispatch hooks while it performs its single buffered dispatch.

### `include/uos/provider/cerebras_handlers.hpp`

Buffered Cerebras Chat APIs use the route telemetry context while joined.

- 10-24 · borrowed context · **SAFE** · The awaiting route or adapter keeps the UsageContext live through all joined status and completion helpers.

### `include/uos/provider/cerebras_rate_limits.hpp`

Synchronous rate-limit reading and parsing API. No task or registration.

### `include/uos/provider/cerebras_responses.hpp`

Buffered Cerebras Responses API.

- 5-6 · borrowed context · **SAFE** · The route holds its context while awaiting completion and the task owns the request and record values.

### `include/uos/provider/claude_capacity_history.hpp`

Claude capacity recording and reading.

- 28-30 · owning task parameters · **SAFE** · The reader and recorder own their store and inputs.

### `include/uos/provider/claude_capacity_rollups.hpp`

Claude capacity rollup and usage scan API.

- 77,104,121 · owning task parameters · **SAFE** · The read/key tasks own store and query/key inputs.

### `include/uos/provider/claude_code.hpp`

Claude Code transport and workspace interfaces own their option records.

- 205-206,296,300-301 · task and callback interface · **SAFE** · Each dispatch or discovery task retains its option values, including its transport and dispatch callbacks.

### `include/uos/provider/claude_code_access.hpp`

Stores borrow an existing KV while owning the policy payload.

- 24-27 · borrowed store · **SAFE** · The forwarding overloads copy or convert the supplied IDs into the store task, and the admin caller retains its owning store handle through the await.

### `include/uos/provider/claude_code_accounts.hpp`

Claude account load and mutation APIs.

- 60-108 · owning task parameters · **SAFE** · The account tasks retain store, identity, binding, environment lookup and mutation data by value.

### `include/uos/provider/claude_code_credentials.hpp`

Claude credential and workspace store/resolution API.

- 105-140 · owning task parameters · **SAFE** · Async operations own store handles, state and the environment lookup callable used in their frames.

### `include/uos/provider/claude_code_fwd.hpp`

Transport and environment type aliases.

- 16-25 · callback interface · **SAFE** · rt::Fetch supplies the transport callable owner, and the environment lookup is invoked synchronously with a live name.

### `include/uos/provider/claude_code_models.hpp`

Model discovery and store interfaces.

- 51,83-100 · owning task parameters · **SAFE** · The coroutine owns the credential, workspace, options and any KV handle or prepared catalog it receives.

### `include/uos/provider/claude_code_quota.hpp`

Quota state and observation recording API.

- 120-127 · callback registration · **SAFE** · Registration owns the callable in module state and the quota write copies it before invoking the awaited work.
- 228-230,300-310 · owning task parameters · **SAFE** · Quota reads, mutations and clearing tasks retain their store and input/binding values.

### `include/uos/provider/claude_code_usage.hpp`

Usage fetch and its owning options.

- 68,76-77 · owning task parameters · **SAFE** · The fetch frame owns credential and options, including the clock and fetch functions.

### `include/uos/provider/claude_code_usage_fwd.hpp`

Forward declarations for the Claude usage schema. No executable site.

### `include/uos/provider/claude_code_usage_refresh.hpp`

Usage refresh entry point.

- 18-19 · owning task parameters · **SAFE** · The refresh task owns KV, account binding and optional fetch inputs.

### `include/uos/provider/claude_code_usage_schema.hpp`

Synchronous usage schema records and converters.

### `include/uos/provider/health.hpp`

Health API returns shared recorder promises and owns clock callables for reads.

- 75-96,101-123 · task and callback interface · **SAFE** · Each health operation uses owned provider/clock data and returns a copied promise or a task with owning inputs.

### `include/uos/provider/lithos.hpp`

Lithos provider transport API with owned options.

- 124-126,234-235 · task and callback interface · **SAFE** · Request and dispatch hook values stay in the task frame through fetch and retry.

### `include/uos/provider/lithos_handlers.hpp`

Lithos handler interface with borrowed telemetry and an observer seam.

- 12-19 · borrowed context and observer · **SAFE** · The caller awaits the handler while its context lives, and stream construction copies it. The test observer receives a weak controller handle.

### `include/uos/provider/lithos_models.hpp`

Lithos catalog API.

- 17-18 · owning task parameters · **SAFE** · Catalog tasks retain any supplied options as a value.

### `include/uos/provider/lithos_rate_limits.hpp`

Retry wait factory interface.

- 97-98 · callback interface · **SAFE** · The timer wait owns its signal and elapsed callback; its listener cleanup is reviewed in lithos_rate_limits.cpp.

### `include/uos/provider/lithos_streams.hpp`

Synchronous construction of stream adapters.

- 17 · model callback · **SAFE** · The adapter receives the model function as a value and retains it with its stream state.

### `include/uos/provider/metered.hpp`

Metered Models and Responses request declarations.

- 65-66,108,125-126,154-161 · task and callback interface · **SAFE** · Each operation owns its catalog, request and dispatch option values, including hook objects and clock.

### `include/uos/provider/openrouter.hpp`

Ref-taking public funcs are synchronous wrappers for owning tasks.

- 44-49,56-57,67-68 · task factories · **SAFE** · Fetch wrappers copy the body, init or decision record before returning their coroutine; lookup frames own the model string.

### `include/uos/provider/openrouter_handlers.hpp`

Handler injection and borrowed telemetry interfaces.

- 6-15 · borrowed transport inputs · **SAFE** · Handler/dispatch frames retain the callback, body and init until the joined invocation finishes, and the relay copies the context.

### `include/uos/provider/openrouter_request_controls.hpp`

Prompt-cache control API uses a synchronous forwarding wrapper.

- 13 · task factory · **SAFE** · The wrapper copies the JSON into the requestExtensions task before returning.

### `include/uos/provider/openrouter_streams.hpp`

The public stream iterator factory is synchronous. Its generator owns response and stream options.

### `include/uos/provider/openrouter_web_tools.hpp`

Synchronous request projection and web-tool handling declarations. No deferred borrowing.

### `include/uos/provider/presentation.hpp`

Synchronous provider ID/display declarations. Returned literals have static storage.

### `include/uos/provider/removed_provider_circuit.hpp`

Retired circuit compatibility API.

- 21-26 · settled promise factory · **SAFE** · The factories return settled values with no callbacks retaining inputs.

### `include/uos/provider/removed_provider_telemetry.hpp`

Retired telemetry compatibility API.

- 6 · settled promise factory · **SAFE** · The synchronous wrapper returns a resolved promise and retains no reference to the supplied JSON.

### `include/uos/provider/selection.hpp`

Provider selection read/store and hash API.

- 75,123,130,217-218 · owning task parameters · **SAFE** · Lookup and store frames own their store/options/selection data, and hash owns its account string.

### `include/uos/provider/stream_relay.hpp`

Relay factories copy per-request inputs into their state.

- 20-25,32-40 · iterator and adapter callbacks · **SAFE** · The production iterator factories own generator handles and the relay state owns its callback bundle.
- 48-71 · borrowed context copy · **SAFE** · The synchronous factories copy telemetry into relay state and copy or move the stream, signals and cancel callback before returning a response.

### `include/uos/provider/surplus.hpp`

Surplus catalog and Responses transport API.

- 88-89,116-117,124-125 · task and callback interface · **SAFE** · Request/catalog frames own option inputs, including the dispatch hook objects.

### `include/uos/provider/surplus_claude_chat_request.hpp`

Synchronous Chat projection and result type declarations.

### `include/uos/provider/surplus_claude_request.hpp`

Synchronous Anthropic request projection declarations with owned payload results.

### `src/chat/completions.cpp`

Chat SSE and buffered completion use separate state owners. Stream cancellation can overlap a pull, so the member calls and the iterator copies were traced independently of terminal accounting.

- 28-87,131-175,201-205,294-311,338-359 · synchronous projections and container access · **SAFE** · Event and tool references are consumed synchronously and queued tool calls own shared records.
- 91,188,199,205,236,395 · iterator cleanup and spawn · **SAFE** · returnIterator takes an iterator value, including when spawned after controller.close releases the algorithms.
- 179-199,207-254,256-285,364-374 · stream member tasks and registered algorithms · **SAFE** · SourceAlgorithm::runAsync owns the callable capturing shared StreamState during both pull and cancel, including nested joins after controller close. Events and usage retained over suspension are local values and the queue is accessed anew.
- 190,275,318,398 · telemetry awaits · **SAFE** · The await chain keeps the state containing the copied UsageContext alive and the terminal callback is a value.
- 312-335,376-403 · buffered member tasks and borrowed context · **SAFE** · The only caller joins completion with its telemetry live. The buffered frame copies the context before its first suspension and waits for each state method before destroying the state.

### `src/chat/envelope.cpp`

The handler owns its telemetry copy through joined models and preflight, and response construction transfers semantically complete state into the stream.

- 48-69,72-96,120-129,199-214 · parse, model, and command awaits · **SAFE** · Requests and options are owned by the relevant frames. The envelope reference points into handleInternal's own result variant and stays within that frame. Borrowed telemetry is supplied by that same frame.
- 139-155,157-167,175-177,186-197 · borrowed context and deadline cleanup · **SAFE** · handleInternal waits for dispatch and its nested failure paths while the context lives. The cleanup callable owns a deadline copy and clears it on the reached paths.
- 168-174 · probe termination callback and spawn · **SAFE** · The callback owns an upstream response handle and the once flag. Detached probe tasks receive their own response handle.
- 180-189 · capturing coroutine generator · **SAFE** · AsyncGenerator transfers the first-event closure into runGenerator, whose frame owns both the closure and the upstream iterator across yields and return.
- 204-207,215-225 · provider dispatch and handler wrappers · **SAFE** · Handler request tasks are joined and returned stream construction copies the telemetry. The public wrappers copy the context synchronously and the telemetry wrapper owns its callable.

### `src/chat/stream_translation.cpp`

Event reconciliation mutates frame-owned output state synchronously. Replay and cancellation have separate iterator handles.

- 49,75-338,346-352,379-385,394-400 · synchronous closures and accumulator references · **SAFE** · The matcher only examines its live argument synchronously. Results own their tool calls, and no borrow of the accumulator container crosses a suspension.
- 354-368 · capturing coroutine generator · **SAFE** · runGenerator owns the closure and its shared prepared state. The replay buffer is immutable and each yield uses an owned event value.
- 369-374 · capturing cancel coroutine · **SAFE** · The returned cancel closure owns both iterator copies. Live callers invoke it through a SourceAlgorithm-owned StreamState and await it before that state can be released.
- 386-393 · borrowed context recorder · **SAFE** · Stream and buffered callers retain their context owners across the join and this task uses the context only to start the completion write.

### `src/codex/account_routing.cpp`

Election and admission use frame-local snapshots and optimistic KV commits. Concurrent routing changes replace the stored values without mutating those snapshots.

- 70-117,148-192 · account and slot borrows · **SAFE** · Pool, row, slot, and evaluation data come from owned snapshots. Account and slot values needed after awaits are copied.
- 122-143,218-240,264-295,346-390 · election, recheck, and route awaits · **SAFE** · Each frame retains its account, row and pool values and the KV owner. A commit's AtomicOperation remains in scope through completion.
- 194-214 · fail-fast all · **SAFE** · routingAccountIdentity receives a CodexAuthState value, so a pending hash child survives the parent result or rejection independently.
- 297-329 · admission coroutine closure · **SAFE** · withCodexAdmission retains the moved action in its frame and waits for it. The action captures KV and account by value.

### `src/codex/auth.cpp`

Auth pool cache accesses are synchronous or start with a copied handle. Probe outcome maps use weak response identities.

- 192-223,608-661 · probe, health, and transition awaits · **SAFE** · Tasks and health writes receive owned response, account and transition data. The map keeps the promise value until settlement and no iterator into it is saved.
- 333-383 · auth pool load and seed · **SAFE** · The frame owns its KV, row, seed and binding inputs. The local AtomicOperation is kept through the commit.
- 680-704 · cache promise and finalizer · **SAFE** · The function copies the cached pool or in-flight Promise before a wait, and the zero-capture finalizer only accesses the module cache.

### `src/codex/auth_refresh.cpp`

Refresh routing uses owned account snapshots and shares work while aborting only the current wait.

- 89-115,135-181,207-279 · response and refresh persistence awaits · **SAFE** · Response, parsed pool copies and database owners live in the joined frames, and the refresh decisions use scalar generation values.
- 281-420,438-454,460-517 · lease, account, and retry awaits · **SAFE** · Each child receives copies of credentials, lease key and KV owner. Owner and lease views refer to local JSON and are consumed within one iteration.
- 535-548 · abort listener ownership cycle · **DEFECT** · While shared refresh is pending, the signal owns its listener and the listener owns the same signal. Teardown before settlement or abort discards the finalizer without breaking that edge. Capture a weak signal state in the once listener and use the settled signal reason while the state is locked.
- 526-534,538,543-549 · executor and sibling lifetime · **SAFE** · newPromise calls its executor synchronously, and the abort race passes only owning promise values. Its shared-work finalizer owns the exact listener identity to remove on settlement.
- 554-608 · stateless refresh and single-flight finalizer · **SAFE** · Request and auth inputs are frame-owned. The hash-map value is copied before waiting and the registered eraser owns the account ID.

### `src/codex/auth_repair.cpp`

Repair plans and records are built synchronously.

- 25,30-48,286-316 · synchronous view and callbacks · **SAFE** · The result outcome view refers to a static literal, and the sort and expiry predicates finish before their captured or pointed-to inputs leave scope.

### `src/codex/banked_reset.cpp`

The usage gate performs one asynchronous read. Record conversion and observers run synchronously.

- 44-69 · usage gate read · **SAFE** · The coroutine owns KV, account hash, and authorization until readCodexResetUsage settles and the decision is made.
- 381-403 · telemetry invocation · **SAFE** · Observer and metric arguments are consumed synchronously under the caller's live telemetry and field owners.

### `src/codex/banked_reset_claim.cpp`

Hash and ledger transitions preserve their candidate, row and transaction state through sequential waits.

- 69-105,169-191 · hash callback and fence iterator · **SAFE** · The hash callable and candidate are frame values. Each fence reference refers to the private immutable candidate vector during its joined KV read.
- 206-440 · claim and update transaction awaits · **SAFE** · Candidates, KV, rows, mutators and atomic operations are retained by the frames which join their reads and commits. Row alternatives and optional values are local snapshots.
- 462-572,616-683 · submission preparation and renewal · **SAFE** · The row, budget, fences and usage variants are local to each attempt and remain live through sequential read and commit waits.
- 783-820 · record update callbacks · **SAFE** · Each mutator's outcome data is captured by value and the update task owns the callable until after the commit.

### `src/codex/banked_reset_pool.cpp`

Pool evaluation snapshots inputs before concurrent work and keeps single-reader list iteration local.

- 133-185,192-226 · shadow KV read/write and list iterator · **SAFE** · The frame retains KV, its AtomicOperation or list iterator, and local decision data through each sequential suspension.
- 297-313,377-408,490-512 · fail-fast all and provider interface · **SAFE** · Each child owns its candidate and hash, KV, or provider handle. None references the fan-out vector after a sibling can reject.
- 332-357,421-431,440-465,565-603 · hash, duplicate, and credit scan awaits · **SAFE** · The fence and inventory slices refer to immutable frame-owned inputs. Inventory and selection values remain alive through the awaited hash.
- 528-551,634-645,667-737 · pool orchestration awaits · **SAFE** · Each frame owns its candidate, provider dependency and variant results and joins each nested operation before releasing them.

### `src/codex/banked_reset_provider.cpp`

The default provider is a thin synchronous factory around coroutine tasks using an immutable binding.

- 95 · body cancel spawn · **SAFE** · ByteStream::cancel starts from a shared stream handle and transfers operation state to its returned task before the local optional body disappears.
- 150-233,238-245 · bound member calls, fetch, and JSON reads · **SAFE** · The virtual wrapper copies Bound into the free coroutine. Bound retains the Fetch callable, and the request, response and JSON owners survive their awaits.
- 441-494 · inventory diagnostic awaits · **SAFE** · Each display frame keeps its resolved fetcher, signal, response, and parsed result, and the credit loop runs over a local value after the read.

### `src/codex/banked_reset_submission.cpp`

Reset submission and verification keep the provider and its inputs alive from admission through settlement.

- 85-148,192-255,261-405 · provider method and preflight joins · **SAFE** · The joined tasks own their provider/dependency bundle and KV. Credit, response and record references point into local result snapshots.
- 412-429,451-485,488-560,568-610 · submit, verify, and receipt awaits · **SAFE** · The provider owner stays in the submitting frame through the result wait. Intermediate observers are synchronous and swallow their own exceptions.
- 614-622,666-825,828-837 · store selection and claim driver · **SAFE** · Dependency, callback and claimed variant values are owned in the awaited frames. No live cursor or borrow from the shared store is retained.

### `src/codex/capacity_routing.cpp`

Routing caches contain replaceable values. Each async read or transformation works on a local snapshot and refreshes the shared cache synchronously.

- 39-49,74-112,122-209,214-234 · hash, KV transaction, and observation read · **SAFE** · The owned pool and JSON snapshots back slot references. The write task or its caller retains AtomicOperation, and the synchronous transform reads a copied row.
- 238-292,504-546 · fail-fast identity and observation fan-outs · **SAFE** · Each child receives its account or input by value and the private hash helpers own their text. Tuple extraction follows a named await result.
- 303-343 · routing cache normalization · **SAFE** · Normalization receives a value copy and the caller re-reads the module cache after resuming without retaining a cache member pointer.
- 352-436,479-496 · joined borrowing transformations · **SAFE** · All borrowed account, observation, model and change flags are in the waiting parent. updateRoutingState owns the callable and awaits each retry before returning.
- 440-472 · parallel KV reads · **SAFE** · The legacy reader owns KV, and the concrete guarded production get copies native store, key and options into its task. A failed sibling cannot invalidate those owners.

### `src/codex/dispatch.cpp`

Codex transport and retry use the request's owned argument state. The injectable retry sleeper has a separate closure lifetime from the request.

- 89-113,145-211,224-272 · transport, health, and deadline callbacks · **SAFE** · The timer callback owns its controller, ScopeExit clears the timer, and beforeTransport remains frame-owned through the join. Spawned storage or body cancellation takes owned values.
- 338-371 · dispatch coordinator callback chain · **SAFE** · The noncoroutine coordinator callbacks own their state. The outer dispatch frames retain the coordinator while the claim runs, and the then reaction owns the individual generation.
- 380-390,396-481,514-544 · request transform and digest · **SAFE** · The synchronous work stack stops using a frame reference after a push that may reallocate, and the digest coroutine owns the copied cache scope and key.
- 611-618 · joined retry without abort · **SAFE** · The waiter owns the sleep callable until its only child settles.
- 623-635 · retry abort listener ownership cycle · **DEFECT** · While retry is suspended the signal owns its once listener and the listener owns that signal. Destroying the waiting frame before either branch settles skips explicit removal. Put removal in a scope guard so frame teardown also removes the listener.
- 628-635 · retry sleeper closure after race · **DEFECT** · If the supplied retrySleep is a capturing coroutine and abort wins, the waiting frame destroys its copy of sleep. The losing sleeper can resume and use its dead closure. Wrap invocation in an owning rt::async child. The default function and current differential fixture use owning free tasks.
- 639-645,712-791 · probe join and attributed response reads · **SAFE** · Probe promises are copied from the registry before allSettled. The auth loop retains its local pool snapshot through each joined identity hash.
- 807-881 · serial driver and loop-state borrows · **SAFE** · The loop owns both driver and state values through the joined private helper tasks, and each dispatched response is passed by value.

### `src/codex/experiment.cpp`

The cache-scope experiment owns its session, auth pool snapshot and callback options.

- 45-57,69-85,108-117 · auth pool and row reads · **SAFE** · The KV owner and copied row stay in the awaited frame. Slot iteration uses the local copied pool.
- 123-187 · refresh, classification, and compare-and-set · **SAFE** · The selectedAccount pointer refers to the const local selected variant through refresh and commit. The race's refresh child owns auth independently. Listener ownership is reviewed in auth_refresh.cpp.
- 198-238,248-328 · transport, hooks, and probe fan-out · **SAFE** · Option and routing copies stay in the experiment frame. Probe handles are copied before allSettled, and detached health writes receive values.

### `src/codex/external_auth.cpp`

Token shape and external account planning are synchronous. Returned records own their text and no async callback is installed.

### `src/codex/index.cpp`

The two entry points join dispatch and its accounting cleanup.

- 13-38 · serial loop callback borrows · **SAFE** · fetchPreparedCodexResponses holds shared ctx across the serial loop join. The loop's raw pointer predicates are synchronous and its other installed operations are joined.
- 44-60 · request coordinator cleanup · **SAFE** · Both options and coordinator are frame-owned and cancellation is awaited before the frame returns or rethrows.

### `src/codex/manual_reset.cpp`

Manual reset keeps request, billing proof and selection data in local snapshots. Its injectable KV dependency needs separate treatment under fail-fast reads.

- 47-78,89-120 · target scan, usage callback, and intent reads · **SAFE** · The account references are into the frame's immutable parsed pool. The copied fetch callable, dependencies and KV live through their nested joins, while a spawned body cancellation owns its handle.
- 88,161-162,199-201 · KV receiver lifetime after fail-fast all · **DEFECT** · readEntry is a forwarding function and releases its KV copy before the returned virtual get finishes. If an injected KV's coroutine get loses to a rejected sibling and the caller supplied the last owner, the pending method can resume with a dead receiver. Make readEntry an owning coroutine. Guarded production KV and MemoryKv take their own inner store ownership.
- 129-153,155-184,192-208,220-229 · validated promotion and reset joins · **SAFE** · The target, intent, observation and operation handles belong to the parent frames, and every nested step after fan-out is joined.
- 209-219 · durable fence predicate closures · **SAFE** · Predicates copy target, selection, intent and clock into the candidate and are only invoked synchronously with row values.

### `src/codex/manual_reset_state.cpp`

Manual reset state comparison, JSON conversion and key construction are synchronous. Each literal-backed view is consumed in its loop iteration.

### `src/codex/native_auth.cpp`

Native binding carries immutable auth snapshots, exact KV rows, and owned copies of optional hook callables.

- 73-93,153-178,180-235,244-291 · document, hash, and binding awaits · **SAFE** · Document text and auth snapshots belong to each frame. The installed readAuth callable is copied before invocation, pool iterators are consumed or copied before the next await, and KV stays live through each transaction.
- 318-420 · generation, binding, and external adoption · **SAFE** · The frame owns the candidate pool and generation inputs. Persist/copy and hash operations retain their local records through joined waits.
- 424-480 · native refresh hook and socket join · **SAFE** · The account context and hook copy stay live until refresh settles, and the transport entry point takes owned strings.

### `src/codex/native_auth_transport.cpp`

The registered native WebSocket outlives its RPC callbacks through the connection and clears listeners on close or loop teardown.

- 63-76,144-193 · socket open, file, and RPC awaits · **SAFE** · The coroutine owns RefreshState and its strings. Open/error callbacks own the promise resolvers. Pointers into initialized and account payloads have frame-owned optional storage.
- 85-139 · pending-call iteration and timer · **SAFE** · Resolve and reject enqueue reactions, so the synchronous pending scan cannot be invalidated by a resumed caller. Timers are cleared on reply, send failure, socket failure or Loop teardown.
- 196-222 · socket listener cycle · **SAFE** · The concrete socket clears persistent callbacks in emitClose, and WebSocketRegistry clears the same callbacks at loop shutdown when no close is emitted. Refresh normally terminates the socket after success or error.

### `src/codex/overage_settings.cpp`

The refresh builds fresh overage rows before replacing shared state.

- 42-56 · KV read and write · **SAFE** · Each task retains KV and its key/value arguments until the operation finishes.
- 64-95 · capturing coroutine and nested fail-fast joins · **SAFE** · rt::async owns the captured account and KV independently of the parent. Both nested hash tasks take owning inputs and the final map mutation uses copied output rows.

### `src/codex/quota.cpp`

Header classification and redaction are synchronous. Header iteration uses keys owned for the loop and each suffix view refers to static storage.

### `src/codex/rate_limit_429.cpp`

Quota classification and probe updates use owned response and account state.

- 16-25,28-65 · routing transformations and classifier awaits · **SAFE** · The account values remain in joined parent frames through updateRoutingState's calls. The parse result is a frame-owned value and response decoding takes its own handle.

### `src/codex/reset_settings.cpp`

Native migration reads normally own their backing store. The public shared-KV override also accepts virtual methods that use their receiver after suspension.

- 59-67,108-112 · injectable KV reads in fail-fast all · **DEFECT** · The read promises run virtual get methods but the parent holds the only guaranteed KV owner. A rejecting sibling can unwind that owner while an injected coroutine get still uses its receiver. Wrap each parallel read in a coroutine with a shared KV parameter.
- 68-98,113-145 · owned hashes and sequential migration writes · **SAFE** · Hash children receive owned strings. Account iteration, rows and the AtomicOperation are owned by the frame through the joined reads and commits.

### `src/codex/responses_error.cpp`

All error event projection is synchronous. Output item iteration and key/value pointers refer to live local or caller-owned JSON for the duration of the call.

### `src/codex/responses_operations.cpp`

Per-request callbacks lock a weak context before returning an owning coroutine. The dispatch loop serializes mutable request state.

- 191-218,253-387,453-476 · admission, proof, and banked candidate awaits · **SAFE** · Coroutine arguments own context and routing state, and fences snapshot account or row values before installation. Local KV remains live through each read.
- 483-506 · candidate map references across two awaits · **SAFE** · The only production caller is the serial runner waiting for reportHealthyFallback. The other map mutators run in separate, sequentially joined phases of that same request, so this loop has exclusive access to this context's map.
- 511-599,600-714,718-763 · cohort, inventory, and overage awaits · **SAFE** · Loops borrow from local candidate, observation and selection snapshots. Options, provider handles, fences and context stay owned through their joins.
- 767-853,867-907,944-988 · transport callbacks, spawn, and auth refresh · **SAFE** · Installed preTransport/onDispatch callbacks travel by value in the awaited transport. Health children receive their own response and error data. The auth race's refreshing child takes auth by value.
- 993-1055 · weak context dispatch table · **SAFE** · Context owns the callbacks without owning itself through them. Each noncoroutine wrapper locks before handing a strong context value to the async implementation.

### `src/codex/responses_runners.cpp`

Runner coroutines carry a strong context while request-local state is driven serially.

- 86-250,253-413 · reset, fence, and inference attempt awaits · **SAFE** · All attempt parameters are values, including shared credential attempts. The external pre-dispatch fence owns its context and candidate across its joined invocation.
- 416-463,465-503 · account entries, terminal outcome, and retry · **SAFE** · The account iteration uses a vector copy. Retry receives a copied sleeper and its own signal. The losing sleeper lifetime is reviewed in dispatch.cpp.
- 505-625,627-682,713-771 · retry, response authorization, and reselection awaits · **SAFE** · Retry and read results are local snapshots, and the forwarding failure helper immediately constructs a task that owns ctx. The selected variant is only borrowed for synchronous tests or copying scalars into an awaited call.
- 774-811 · weak context runner table · **SAFE** · Stored forwarders lock weak state synchronously and transfer a strong context value to each returned task.

### `src/codex/responses_state.cpp`

Creation reads and packages one request-local state. State reset and decision helpers are synchronous.

- 13-94 · response context construction and callbacks · **SAFE** · The input, strong pool, route result and coordinator live in the frame. The stored reload callback owns its config or is copied from options.

### `src/codex/routing_evaluation.cpp`

Routing classification and proof folds are synchronous. The borrowed pool, account and map elements are consumed during each call.

### `src/codex/routing_mutations.cpp`

Route mutations use owned snapshots and joined persistence.

- 33-49,83-154,172-215,329-395 · fence iteration, CAS, and proof awaits · **SAFE** · The private fence vector and KV/atomic operation remain in the waiting frames. Optional row/probe data is copied before any later mutation.
- 222-237,244-288,291-308 · joined routing transform closures · **SAFE** · updateRoutingState owns the callable through each retry while the joined parent retains captured account and identity referents.

### `src/codex/routing_serial.cpp`

The serial admission lock chains copied promises and invokes a frame-owned action. Slot identity resolution is independently owned.

- 96-159,232-263 · strong row, commit, and selected pool awaits · **SAFE** · Row and pool snapshots plus the transaction owner remain local. The account hash loop uses the unchanged by-value pool.
- 177-228 · fail-fast account identity children · **SAFE** · Each child copies the particular auth record from the slot pool and can outlive a sibling rejection without a borrow into that vector.
- 266-285,295-325 · admission lock and coroutine closure · **SAFE** · withCodexAdmission keeps both the action and KV in its own frame through the wait and release. The action closure owns its account inputs and every result is joined.

### `src/codex/routing_state.cpp`

KV key constants and routing row serialization are synchronous. Reference-taking converters do their work within the call.

### `src/codex/session.cpp`

Session telemetry uses per-observation hashes and a local durable update.

- 200-269 · session, key, and rate update awaits · **SAFE** · Inputs and hashes are frame-owned, the row vector remains local across the write, and the AtomicOperation and KV survive the commit.

### `src/codex/supervisor.cpp`

The admin follow handler spawns one owning polling task and weakly observes it from the stream and request signal.

- 209-259,292-307 · stream start, detached poll, cancel and abort · **SAFE** · The poll frame owns FollowState and the revisions handle. Controller holds a weak stream handle, and the abort and cancel callbacks lock weak state only for their synchronous use.
- 263-290 · config and inventory awaits · **SAFE** · Config and inventory snapshots are frame-owned. A matching configured source is copied before the inventory suspension.

### `src/codex/supervisor_brief.cpp`

Transcript and brief generation use independent owned transcript, connection, and request records.

- 204-209,270-327,330-350 · connection tasks and enrichment iteration · **SAFE** · The frames retain connection and callable owners. Target references come from a private immutable vector and lookup views refer to locally owned RPC payloads.
- 440-501,561-606,637-666,854-879 · transcript, brief, and connection teardown · **SAFE** · Source, config, response, and turn vectors are frame-owned. The connection stays live through collection errors and the awaited close.
- 764-770 · borrowed adapter inputs · **SAFE** · Both reference-taking entry points are regular factories that copy connection/source into the child without suspending first.

### `src/codex/supervisor_config.cpp`

Supervisor configuration copies its env reader before the file-read boundary.

- 104-119 · poll timer and abort listener · **SAFE** · The timer completion removes the listener and abort clears the timer. Loop::clearAllTimers clears the timer callback on teardown, breaking the callback ownership chain.
- 199-250 · config read and parser await · **SAFE** · Options, env function, path and file contents stay in the frame. The environment reader is invoked synchronously.

### `src/codex/supervisor_follow.cpp`

Each poll owns its revisions state and goes through one joined connection read and close.

- 143-199 · RPC and revisions map ownership · **SAFE** · The connection and shared revisions are task parameters. The awaited RPCs consume local result values and revisions are accessed synchronously at the end.

### `src/codex/supervisor_inventory.cpp`

Inventory sampling keeps source records, connection hooks and per-source projections independent.

- 99-126,193-233,314-373,430-446 · SQLite and socket reads · **SAFE** · Path, database and connection handles belong to their calling frames. SQL statements and payload projections are consumed synchronously.
- 580-590,632-708 · sample reads, fan-out and join · **SAFE** · Each worker is held by rt::async and mapWithConcurrency owns shared worker state. probeOne copies the thread ID and client into each task. Projection maps and callback are local.
- 711-774,785-797,842-856 · sampling ownership and cache finalizer · **SAFE** · Source, connection, timeout and copied in-flight promise survive waits. Close is joined, and cache reactions only access module storage.

### `src/codex/supervisor_log.cpp`

Rollout tail reads use owned input and a local fd guard across file checks and bounded reads.

- 154-194 · file guard across microtasks · **SAFE** · The read frame owns file/path handles and byte buffers. Its fd guard releases the descriptor on every exit and is unrelated to serialized shared state.

### `src/codex/supervisor_secret.cpp`

Secret detection and redaction are synchronous.

- 600,716,755 · synchronous redaction callbacks · **SAFE** · Regex captures refer to static pattern storage. Local accumulation callbacks are invoked before their output collections leave scope.

### `src/codex/supervisor_transport.cpp`

Socket events now use weak connection state. The separate per-call abort callback still creates a cycle while an RPC is pending.

- 62-70,154-179,181-263 · transport awaits and socket listeners · **SAFE** · Connection and call frames own the shared state. Socket message/close/error callbacks use weak state, and initialization failure settles pending calls and terminates the socket.
- 88-101,127-150 · pending map and normal cleanup · **SAFE** · Settlement copies the call record before erase, clears its timer, removes the matching listener and only then settles its promise.
- 134-140 · per-call abort ownership cycle · **DEFECT** · While the call is suspended, pending cleanup owns its signal and listener, and the listener owns the same ConnectionState. Loop teardown drops the timer and socket callbacks without settling that call, leaving the pending cleanup cycle alive. Use weak state in the abort callable while callConnection owns the state.

### `src/deepseek/chat_projection.cpp`

Projection and normalization are synchronous. The system message iterator is used only while its local vector is live.

### `src/deepseek/context_admission_failure.cpp`

Failure marking and recognition are synchronous. Response identity tracking uses weak keys.

### `src/deepseek/flash_aliases.cpp`

Catalog warming is detached while alias calculation uses copied catalog and model results.

- 104-107 · detached catalog fetches · **SAFE** · Each spawned task obtains or owns its fetch state and receives no local view or closure from the warming function.

### `src/deepseek/forwarded_payload_policy.cpp`

Payload and context limits are parsed and applied synchronously. The profile view is consumed within the lookup call.

### `src/deepseek/handlers.cpp`

Chat and Responses handlers join upstream fetch and buffered projection. The streaming adapters copy state before returning.

- 77-129,204-224 · error, projected-body callback, and preheader awaits · **SAFE** · The joined handler retains its telemetry owner, and projected JSON is consumed synchronously by a callable owning the diagnostic destination.
- 186-196,226-262,269-323 · stream adapter, buffered read and usage record · **SAFE** · The relay copies telemetry for streaming, and the joined frame owns each response, payload, options and request input for buffered work.

### `src/deepseek/index.cpp`

DeepSeek preparation, validation and envelope checks operate synchronously on the current payload. Fetch transfers argument copies across transport.

- 620-696 · deadline, admission and fetch callbacks · **SAFE** · Options and fetcher own their callables across the joins. The deadline callback owns a controller copy and is cleared by ScopeExit, and projected body observation is synchronous.

### `src/deepseek/responses.cpp`

Responses request projection and model policy checks are synchronous. Reasoning and model views are consumed within the owning record or table's lifetime.

### `src/deepseek/responses_payload.cpp`

Responses payload construction is synchronous. Field lookups return copies or are used immediately against the live JSON record.

### `src/deepseek/responses_stream.cpp`

The projection API exposes synchronous translator operations.

- 352-355 · translator callback bundle · **SAFE** · Each returned function owns shared translator state and that state does not retain the function bundle.

### `src/deepseek/stream.cpp`

The generator owns its reader session and watchdog. The individual read race uses a weak signal listener.

- 129-151 · reader race and abort registration · **SAFE** · The executor is synchronous, its installed callback locks a weak signal, and reader.readPromise retains the pending read independently of the winning branch.
- 157-234,250-287 · read session, timer, and generator · **SAFE** · The factory copies options and starts an owning generator task. Each read owns session, yielded frames belong to the task, and the watchdog captures a controller value. Finish clears the timer and releases the reader.

### `src/deepseek/waterfall.cpp`

Provider waterfall order, status classification and callback decisions are synchronous. The enabled/configured predicates execute during plan construction.

### `src/deepseek/waterfall_handler.cpp`

Waterfall hops are joined serially, and the replay pump owns its iterator independently of the processing frame.

- 33-52,81-95,104-129 · dispatch callback, borrowed telemetry and hop iteration · **SAFE** · The dispatcher owns its paid tail and all child calls are joined by frames retaining the input and context. The plan vector is immutable during iteration.
- 54-78 · stream replay and detached iterator operations · **SAFE** · The pump owns its iterator and has a weak controller. Stream cancel invokes the synchronous return_ factory before the captured iterator is released, handing the queued request state to the detached task.

### `src/embeddings/handlers.cpp`

The buffered handler joins member operations while its request and telemetry owners are live. Its optional KV seam admits implementations with a borrowed async receiver.

- 80-130,150-231,237-342 · request-state members and retries · **SAFE** · handleEmbeddingsRequest owns the RequestState object. Nested tasks are joined serially, and Voyage parameters and chunk slices are copied before sending.
- 137-140 · cache fan-out virtual receiver · **DEFECT** · One cache get can reject while another injected KV get is still awaiting. The request unwinds the last shared store owner, leaving the pending virtual coroutine with a dead receiver. Wrap each get in an owning child as jobs.cpp does.
- 343-398 · handler context, hash fan-out and callback · **SAFE** · The terminal route retains its context during entry, runTelemetry owns its copy, and the telemetry wrapper retains the callback. Each hash child owns its input text.

### `src/embeddings/jobs.cpp`

Jobs keep their attempt state in the request coroutine and pass owned KV into pending reads and writes.

- 38-60,134-144,212-234 · cache and input fail-fast fan-outs · **SAFE** · Each cacheEntry child takes a shared KV and key by value, including children whose results are overtaken by a sibling's failure.
- 83-106 · budget borrow, commit and allSettled · **SAFE** · The job retains its shared KV while the budget helper runs. AtomicOperation is local, and the input delete batch joins both successful and rejected operations.
- 146-211,237-302 · job member awaits and state mutation · **SAFE** · The attempt frame owns JobState and joins its members. Chunk inputs and response results are local values, and nested updates preserve their transaction owners.
- 305-385,387-448 · create/get, encrypt writes and telemetry · **SAFE** · Each input write owns KV, key components and plaintext input. The handler awaits job execution and the telemetry adapters retain their own context copies.
- 451-498 · job attempt and public entry joins · **SAFE** · The parent retains the optional borrowed telemetry during joined operation. Job state and atomic operations are frame-owned, and wrappers retain request arguments by value.

### `src/embeddings/ledger.cpp`

Ledger records, chunk assembly and transaction state are per invocation. The replay fetch batch needs ownership for each virtual KV read.

- 161-168 · idempotency replay fan-out receiver · **DEFECT** · A rejected chunk read can lead to an unavailable response while another injected virtual get remains pending. When the acquisition unwinds the last shared KV owner, that coroutine can resume with a dead receiver. Keep the store in each child frame.
- 156-160,169-180,197-347,534-618,621-714 · response hash, acquisition and settlement joins · **SAFE** · Named results, owned lease and params, per-attempt rows and local atomic handles retain their resources through joined reads and writes. Hash children own the combined text.

### `src/embeddings/voyage.cpp`

Cache and budget code borrow KV within joined request and job frames, with a separate issue at the cache eviction fan-out.

- 55-124,144-175,246-289 · borrowed KV, list iterator and atomic waits · **SAFE** · The handler or job retains KV through the awaited cache and budget operation, and the list and transaction handles are local to each frame.
- 126-136 · eviction fan-out receiver · **DEFECT** · If one eviction index get rejects, the cache writer returns failure without joining a pending sibling. An injected KV coroutine can resume after its receiver's last owner leaves the request. Propagate a shared store into the eviction reader children.
- 186-188,194-197 · timer-driven completion · **SAFE** · Sleep captures its resolver, and the abortable sleep timer owns State and removes the listener during ordinary completion.
- 198-207 · retry sleep abort ownership cycle · **DEFECT** · The signal retains the once callback and that callback retains the same signal. When loop teardown clears the pending timer before it fires, the listener cycle remains. Use weak signal state or rely on once to remove the listener before it runs.
- 435-467,503-545 · encryption, key read, and upstream fetch · **SAFE** · Input and request options are frame-owned, the fetch hook is awaited through completion, and ScopeExit clears the streaming-independent request timer.

### `src/harmony/adapter.cpp`

The Harmony transport makes its callable owner explicit, including when a consumer races it with cancellation.

- 256-277 · transport callable keep-alive · **SAFE** · The member wrapper synchronously transfers a shared function owner into keepAlive alongside the returned task. The Cerebras forwarder owns options and returns a value-taking task.
- 280-299 · transport and buffered parse awaits · **SAFE** · The frame owns adapter options, response and JSON values. The parse recovery callback has no captures.

### `src/harmony/classifier.cpp`

Classifier request and verdict conversion are synchronous and consume each view while its owning response is live.

### `src/harmony/conversation.cpp`

Conversation transforms are synchronous. They return owned copies and do not suspend while iterating turns.

### `src/harmony/parse.cpp`

The Harmony parser consumes text views synchronously and returns owning parse records.

### `src/harmony/probe_scenarios.cpp`

Probe scenarios sequence each model step while owning the probe context and its callables.

- 47-153,170-176 · scenario joins and callback factory · **SAFE** · The scenario frame owns its context, options, tool calls and conversation at each wait. The registered scenario wrapper is synchronous and copies its index into the returned task.

### `src/harmony/probes.cpp`

Probe transport and result projections own their per-call state.

- 112-170,179-203 · probe invocation, JSON reads and result values · **SAFE** · Each task owns a context and request arguments, and each parse uses a local response. The zero-capture catch reaction owns no referents.
- 241-249 · published probe callback bundle · **SAFE** · The returned functions own a shared ContextState which contains transport and clock values, without a back-reference to the bundle.

### `src/harmony/reliability/context.cpp`

Transcript sizing, compaction and rendering are synchronous. Indexes and views are used while the conversation is live.

### `src/harmony/reliability/failure.cpp`

Reliability classification is synchronous and returns owning results.

### `src/harmony/reliability/feedback.cpp`

Tool argument validation and feedback projection use synchronous calls with live JSON arguments.

### `src/harmony/reliability/harness.cpp`

The harness joins private borrowing helpers and uses owning race branches for cancellation.

- 117-135 · run cancellation listener and race · **SAFE** · ScopeExit removes the listener and the listener owns only a resolver. Both race inputs own their pending state.
- 218-298 · transport, body parse, and borrowed retry parameters · **SAFE** · The joined frame retains run, opts and built. HarmonyTransport::keepAlive and Response::json separately own the work that may outlive an aborted wait.
- 321-340,369-414 · tool annotations, retry sleep and loop references · **SAFE** · The sequential caller owns run, options, validation data, tool calls and analysis through the join. Tool routing takes an owning backend handle.
- 465-479,511-519 · turn and run joins · **SAFE** · runReliabilityHarness owns run and options, and runHarnessTurn owns the built request and normalized result until both children complete.

### `src/harmony/reliability/hash.cpp`

Reliability hashes are calculated synchronously from live input views.

### `src/harmony/reliability/loops.cpp`

Loop detection and ordered-map observation execute synchronously. Returned flags own their inspection results.

### `src/harmony/reliability/retry.cpp`

Retry policy and ledger inspection are synchronous. Entry copies and scalar delay values travel outside the function.

### `src/harmony/reliability/state.cpp`

Tool observation reducers and state normalization are synchronous. Returned state owns the values read from sources.

### `src/harmony/reliability/surfaces.cpp`

Provider surface assertions and request descriptions are synchronous. The returned records own their inspected fields.

### `src/harmony/reliability/verify.cpp`

Verification and final-answer checks evaluate synchronous snapshots. No callback or container reference crosses a wait.

### `src/harmony/render.cpp`

Rendering runs synchronously over input turns and static option literals.

### `src/harmony/tools/backend.cpp`

Backend path normalization operates synchronously on the supplied text.

### `src/harmony/tools/fakes.cpp`

The fake workspace performs synchronous in-memory operations.

- 162-165 · fake backend task factory · **SAFE** · execShell calls shell_ before returning and transfers only the result value to fulfilledTask.

### `src/harmony/tools/result.cpp`

Tool result shaping and clipping are synchronous. Results own their strings.

### `src/harmony/tools/router.cpp`

The tool router owns its backend and validated argument snapshots.

- 68-87,163-200 · shell receiver and tool dispatch await · **SAFE** · execShell retains a shared workspace while the virtual shell task runs, and runToolImpl awaits it with its other inputs live.
- 203-217 · borrowed JSON task factories · **SAFE** · The pointer overloads copy optional JSON into runToolImpl before returning its task.

### `src/harmony/tools/schemas.cpp`

Schema parsing and argument validation are synchronous and return owned data.

### `src/harmony/types.cpp`

Harmony wire and tool type conversion is synchronous with static discriminator views.

### `src/live/calls.cpp`

Call creation buffers and validates local multipart data, then joins account selection, transport, and mapping persistence.

- 35-89 · request and multipart awaits · **SAFE** · The Request, optional File and parsed form live in their frames. A nested read or cancellation copies its stream owner before suspending.
- 93-128,133-148,150-181 · account, response and telemetry waits · **SAFE** · Account and response results are local value snapshots and the mapping write owns all its identifiers.

### `src/live/handler.cpp`

HTTP and upgrade handlers own the incoming Request and principal snapshots.

- 27-45,47-65 · authentication, principal, and upgrade waits · **SAFE** · Auth/route decisions and the presentation callback are local values. UpgradeResult owns the returned accept function separately.

### `src/live/routes.cpp`

Route matching is synchronous and returns owned route fields.

### `src/live/sideband.cpp`

Sideband ownership spans both WebSockets and the delayed mapping retirement before close.

- 28-33,88-120 · constructor await and downstream handlers · **SAFE** · The bridge frame owns State through installation and the downstream registrations retain it afterwards. A close while the constructor is pending sets settled before the frame resumes.
- 59-87,121-142 · socket callbacks and deferred close · **SAFE** · State is retained while either socket can deliver a callback. Both concrete sockets clear listener edges at close or WebSocketRegistry teardown. The retirement task owns its call ID and the then callbacks own the state.
- 147-165,168-193 · account and upgrade transfer · **SAFE** · Lookups operate on frame-local data. The synchronous accept callable copies socket and routing strings into the detached bridge task.

### `src/live/upstream.cpp`

Mapping operations own identifiers and retain KV through each joined operation.

- 65-91 · mapping reads and writes · **SAFE** · The returned store and entry are local values. Retiring state is looked up anew after an await instead of retaining a set iterator.
- 93-109 · retirement guard and retries · **SAFE** · ScopeExit borrows the same frame's callId, whose parameter storage outlives the guard. Each delete retains its KV owner through completion.

### `src/paid_fallback/claude_code_route.cpp`

Pool work copies subscription data before forwarding and retains one shared lifetime per request.

- 53-127 · capability check, settlement hook, and pool fan-out · **SAFE** · Each loadPoolState child owns a store/account copy. The joined bookkeeping frame owns each callback and its result/health snapshots.
- 135-203 · before-dispatch and transport callbacks · **SAFE** · Fetch options own captured state and the handler waits for the fetch to settle with its telemetry referent live.
- 206-293 · selection and one request lifetime · **SAFE** · The roster or pool view remains frame-local. Joined dispatch, outcome settlement and pool advancement receive owned request/state copies.

### `src/paid_fallback/health.cpp`

The lifecycle shares one recorded flag and sends owning values into deferred bookkeeping.

- 63-116,182-185 · bookkeeping spawn and fail-fast outcomes · **SAFE** · Detached bookkeeping takes the callable and operation string by value. Settlement and health children own their reservation/provider inputs and can finish independently.
- 199-242,244-266 · classification and lifecycle callbacks · **SAFE** · Response classifiers own their handles and the lifecycle functions own shared state without a back-reference to themselves.
- 270-285,358-401 · catalog fan-out, cache and background refresh · **SAFE** · Each refresh owns its options and signal. The routing lambda reads only local catalog values when invoked synchronously.
- 142-150,319-356 · temporary Surplus transport · **SAFE** · The frame copies the supplied telemetry and the dispatch callbacks capture independent context and marker copies through the joined fetch.

### `src/paid_fallback/index.cpp`

Policy and settlement tasks operate on owned request and reservation records.

- 165-203,213 · policy lookup, retry, and terminal-sweep spawn · **SAFE** · KV and policy snapshots stay in their frames. The detached reconciliation entry point takes no caller-owned references.
- 246-315,322-373 · pricing and reservation awaits · **SAFE** · Pricing, policy, and admission input data are held as values and model/catalog pointers refer to local snapshots.
- 376-484 · reservation settlement and bookkeeping awaits · **SAFE** · Provider response, reservation, and usage data are frame-owned and all durable steps in each function are joined.

### `src/paid_fallback/ledger_admission.cpp`

Admission and release use CAS snapshots and module-owned KV; explicit override deletion uses joined member operations.

- 91-142,144-197,201-266 · iterator, read batches, and commit ownership · **SAFE** · Admission and release read the globally retained production KV, whose guarded get also retains native storage independently. Transaction and tuple/value owners remain live across their waits.
- 271-320,324-386 · deletion, retry, and request update ownership · **SAFE** · The override store, row and transaction handles remain in the joined frames. The deletion scans retain individual iterator owners through next.

### `src/paid_fallback/ledger_backfill.cpp`

Backfill and reconciliation use serialized leases and owned scan state. One read batch accepts an externally owned store.

- 119-450 · cursor, scan, and transaction ownership · **SAFE** · Frames retain the shared KV and local list/transaction handles. Progress is shared by value and row and cursor references point into private snapshots.
- 451-457 · backfill predicate KV fan-out receiver · **DEFECT** · A rejecting state or cursor get can unwind the backfill while a sibling using an injected KV receiver is still pending. A caller that transferred the last shared KV owner leaves that sibling with a dead receiver. Wrap each of the three virtual reads in an owning child.
- 458-527,530-594,597-648 · predicate, lease, and reconciliation awaits · **SAFE** · Scans and settlement join frame-owned candidates, stores and log maps. deferAll itself starts a child with a KV owner for each entry. Nested virtual read fan-outs are reviewed in ledger_settlement.cpp.
- 652-792,794-806 · backfill driver and single-flight callbacks · **SAFE** · The worker owns KV and progress through its leases. The scheduled reactions have no captures and pass the resolved KV by value to the worker.
- 808-1074 · public reconciliation and queue awaits · **SAFE** · Keys, leases, override handles, and row lists remain in the frame or become child values. Iteration uses live local snapshots and each queue/commit call is joined.

### `src/paid_fallback/ledger_settlement.cpp`

Settlement keeps transaction values and cursor rows local. Directly injected store receivers need ownership in each fail-fast read child.

- 99-223 · leased scan, preparation, and commit awaits · **SAFE** · KV, pending rows, and transaction handles are coroutine parameters or locals alive through each awaited operation.
- 225-260,388-445 · request-bound settlement against global KV · **SAFE** · The production store has a module owner and each guarded get also retains an inner native store. Durable settlement inputs are copied before the joins.
- 266-271,322-327 · deferral and settlement KV fan-outs · **DEFECT** · A rejecting virtual read can end deferral or settlement while a sibling coroutine get from an injected store is pending. After the last public caller releases KV the sibling can use its dead receiver. Give each parallel read its own store owner.
- 272-296,299-314,328-386 · row selection, provider log, and settlement CAS · **SAFE** · Tuple alternatives refer to named local await results, and rollup and commit inputs are owned by their respective child frames.

### `src/paid_fallback/ledger_state.cpp`

The ledger view functions retain a store and each reader/queue receiver through joined operations.

- 64-128 · store, pending iterator, and enqueue owner · **SAFE** · The frame retains KV and the list handle while stepping. The borrowed KvEnqueue interface is obtained from the KV it owns through send completion.
- 129-219,221-320 · ledger projections and cursor storage · **SAFE** · Each scan owns its iterator and accumulators. Awaited row values and readable request/pending snapshots remain local.

### `src/paid_fallback/ledger_stats.cpp`

Stats merges keep their transform closure and snapshot data in the writer frame.

- 86-107,246-295 · joined merge closure and commit · **SAFE** · The transform owns scalar/string captures and consumes each local row synchronously. KV and the AtomicOperation live through each commit.
- 346-430 · growth scan and iterator · **SAFE** · Growth reads retain the list iterator, store handle and result accumulators while stepping.

### `src/paid_fallback/removed_provider.cpp`

Retired provider compatibility has synchronous implementation and immutable literals.

### `src/paid_fallback/rollups.cpp`

Rollup writes and listing use shared KV and independent local cursors.

- 42-75,76-100 · CAS and listing ownership · **SAFE** · The frame keeps KV, the AtomicOperation and its list iterator alive through awaited commits or reads.

### `src/paid_fallback/routing.cpp`

The waterfall keeps one borrowed telemetry referent through serial provider attempts. Callback and accounting operations retain their required owners.

- 38-118 · primary hooks, telemetry and cancellation · **SAFE** · The request's waiting frame retains context and the owned fetch options retain both hooks, which are used during joined dispatch. Bookkeeping owns reservation records.
- 120-200 · paid attempts, deadline, and provider iteration · **SAFE** · The provider reference refers to a private immutable vector and the task inputs are copied. The deadline callback is guarded and the provider fetch is joined before returning.
- 214-278 · delivery and admission handler joins · **SAFE** · The response recorder owns its reservation and response copies. Synchronous rejectAdmission ends with the frame that owns the other routing snapshots and borrows the live caller context.

### `src/provider/anthropic_chat_responses.cpp`

Claude Chat projection hands owned frame and stream state to the generator and to its buffered reader.

- 353-445 · generator inputs, iterator and event vectors · **SAFE** · The generator body owns response/options, and the inner iterator and emitted byte buffers stay live through yield. The reference-capturing projections are synchronous, local helpers.
- 446-518 · translated stream callbacks and cancellation · **SAFE** · Pull and cancel tasks take shared state by value and the source algorithms retain their closures across a join. Controller is weak and reader buffers are owned.
- 519 · direct await of aggregate body-read options · **DEFECT** · The bounded-reader helper is a coroutine taking an owning aggregate by value. Passing the aggregate in the awaited expression violates PORTING.md's GCC 12.2 rule. Construct a named task before awaiting it.

### `src/provider/anthropic_chat_stream.cpp`

Framing and response bytes live in the generator, with owned watchdog and reader cancellation state.

- 114-138 · read/abort race · **SAFE** · The executor is synchronous, the installed listener locks weak signal state and reader.readPromise owns its I/O state even when it loses the race.
- 156-220 · generator, watchdog and cancellation · **SAFE** · The generator owns its reader and input response. Its chunk borrow points into the local read result across the yield. Abort and timeout callbacks copy their handles and normal cleanup or timer-queue teardown drops the ownership edges.

### `src/provider/anthropic_replay.cpp`

Replay validation, identity and cache maintenance are synchronous. Borrowed JSON blocks and cache iterators are consumed in the corresponding call.

### `src/provider/anthropic_response_wire.cpp`

The Responses writer's callable bundle is synchronous.

- 231-345 · writer and output projection state · **SAFE** · The writer callables own their state and synchronously copy event or tool values, without a state-to-callable back-reference.

### `src/provider/anthropic_responses.cpp`

Native Messages parsing uses a per-stream generator and weak abort signal access. The normalizer has direct aggregate awaits requiring task naming.

- 551-575 · read and abort race · **SAFE** · The synchronous setup installs a weak signal listener and independent resolvers. Both branches retain operation state after the race settles.
- 644-719 · watchdog, frame borrow, and reader cancellation · **SAFE** · ReaderState and the native read result remain live through yield. The callback receives its own reader/cause state, and cleanup or timer shutdown releases the timer edge.
- 721-771,805,820-852,864-876 · translation generator and source algorithms · **SAFE** · Frame and translated buffer values stay local. Pull and cancel own the shared translation state, and detached body cancel owns stream operation state.
- 779,857,880 · direct awaits of by-value aggregates · **DEFECT** · The bounded-body reads and native error normalization await coroutine calls with owning option aggregates directly in the expression. GCC 12.2's documented pattern can miscompile their destruction. Name each task before awaiting.

### `src/provider/capacity.cpp`

Capacity refresh uses value snapshots, leases and independently owned sampling work.

- 55-127,203-275 · KV options, Codex fetch, and Claude paired reads · **SAFE** · Each task owns account, fetch, context and option data. Both quota/health branches and their reaction callbacks carry their own inputs.
- 283-327,346-417 · account and provider fail-fast fan-outs · **SAFE** · Each slot or provider task receives its own account, options, fetch and signal values. The result tuple is named and remains live through routing observation recording.
- 539-705,723-769 · lease, comparison and history scan waits · **SAFE** · The parent retains KV and transaction state. Comparison gets join through allSettled, and fail-fast history children themselves take KV ownership.
- 772-777 · borrowed CapacityReads · **SAFE** · Both refresh and persisted-view callers wait while their local CapacityReads snapshot lives, and the nested merge receives copied history/events.
- 781-834,854-871,914-1012,1014-1148 · refresh publication and admin view awaits · **SAFE** · Each child owns its rows, input handles and option snapshots. View assembly keeps the shared context, and each parallel child owns the KV and inputs it uses.

### `src/provider/capacity_analytics.cpp`

Capacity analytics read and write via local snapshots.

- 139-187 · CAS and remote row iteration · **SAFE** · KV, the AtomicOperation and list cursor remain in the waiting frames, and sorting follows the scan.

### `src/provider/capacity_contract.cpp`

Contract parsing and JSON conversion are synchronous. Variant visitor references are confined to each call.

### `src/provider/capacity_events.cpp`

Event persistence and scans retain their store and cursor owners. Event sampling receives a copied timestamp.

- 54-64,79-144,152-204 · KV write and list awaits · **SAFE** · The task owns KV, the candidate event and any iterator or AtomicOperation through its awaited use.
- 212-221 · event-triggered deferred sampling · **SAFE** · The reaction owns its timestamp and returns a task with owned sampling options. The rejection reaction is stateless.
- 296-451 · record and event-list driver awaits · **SAFE** · Each list owns its cursor and result vector. Historical backfill writes and separate root reads keep their argument copies and store live.

### `src/provider/capacity_history.cpp`

History scan and recovery use copied snapshots.

- 183-205,320-339,357-385 · row scan, event persistence and recovery · **SAFE** · Store, iterator, snapshots and event lists remain local across the joined operations, and source pointers are used only against live local data.

### `src/provider/capacity_parse.cpp`

Capacity schema consumers return owned rows. Only the final stored snapshot lookup suspends.

- 424-435 · snapshot read · **SAFE** · The frame owns KV and the received row, with field views consumed synchronously by the local parser.

### `src/provider/capacity_rollups.cpp`

Rollup transforms consume local snapshots synchronously and sorted listing owns its cursor.

- 325-349 · rollup list iteration · **SAFE** · The store and unique iterator are frame values through next and sorting uses the collected copies.

### `src/provider/cerebras.cpp`

Cerebras Chat wire normalization is synchronous. Its request owns options through the preheader send.

- 655-733 · fetch callback, admission and deadline · **SAFE** · The options and fetcher stay live during awaited dispatch. The timer captures its controller and is cleared by ScopeExit.

### `src/provider/cerebras_handlers.cpp`

Chat reads the bounded complete upstream body before returning a buffered or replay response.

- 98-148,153-215 · buffered handler, context, and callback borrows · **SAFE** · The envelope joins the handler with live telemetry, fetch options retain its callbacks, and the captured bytes/results and replay data are owned values.

### `src/provider/cerebras_rate_limits.cpp`

Rate-limit mapping is synchronous and returns owning values.

### `src/provider/cerebras_responses.cpp`

Responses uses a joined buffered Chat result with synchronous projection.

- 91-117,119-170 · borrowed telemetry and buffered dispatch · **SAFE** · The route joins dispatch while its context remains alive, and upstream response, bytes and translated values belong to their frames.

### `src/provider/claude_capacity_history.cpp`

Capacity history has separate owning per-rollup reads and a local list cursor.

- 229-259,413-427 · hash and rollup-key arithmetic · **SAFE** · The event, window selector, KV and point are owned values. The local projection sees only the frame-owned event.
- 493-527 · fail-fast rollup fan-outs and commit · **SAFE** · Each reader owns KV and the point/meter input, so a failed sibling does not release its receiver. AtomicOperation joins its commit.
- 530-586 · history iterator and close · **SAFE** · The owning cursor and frame-local points remain live across next or return, and sorting takes place after iteration.

### `src/provider/claude_capacity_rollups.cpp`

Capacity and usage lists use local cursors and typed point snapshots.

- 236-267,269-278,355-397 · list iteration, keying and cursor cleanup · **SAFE** · KV, list cursor, each row and the hash task's point stay live through next, return and joined validation.

### `src/provider/claude_code.cpp`

Claude Messages transport owns fetch callbacks and request state. Its preheader abort listener has a teardown cycle.

- 100-119 · Claude abort listener ownership cycle · **DEFECT** · The signal owns an abort callback that retains the same signal. If the transport and abort are both pending when the wait is destroyed, the frame drops dispose without calling it. Capture weak signal state or install a removal guard before suspension.
- 137-158,213-289 · preheader transport, fetch branch and error body · **SAFE** · The provider options and hooks are frame values. rt::Fetch::keepAlive owns the transport callable if abort wins. The running timer owns its controller and normal paths clear it.
- 426-496,498-624 · workspace discovery, retries and normalization · **SAFE** · The source frame owns workspace, native request, credentials and attempt snapshots. Before-dispatch closures remain live through joined attempts, and the returned normalizer stream has its own options.

### `src/provider/claude_code_access.cpp`

Access policy helpers consume environment/row inputs synchronously and join their writes.

- 32-41,59-86 · borrowed store and owning arguments · **SAFE** · The storePolicy coroutine borrows KV from an awaited admin caller. Public forwarders copy allowed key IDs synchronously, and their result tasks keep the JSON values.

### `src/provider/claude_code_accounts.cpp`

Account roster state is snapshotted at each read and mutation.

- 62-71,111-131,173-214 · credential hash and account update snapshots · **SAFE** · The reference-taking fingerprint is a synchronous task factory that creates owned digest input. The roster and account update frames retain their copied records.
- 263-372,375-441 · KV read, account generation, and CAS waits · **SAFE** · Frames retain KV, typed rows and transaction owners. Borrowed account data is consumed synchronously or copied into each child operation.

### `src/provider/claude_code_credentials.cpp`

Credential and workspace lookup accepts owned store and environment callables.

- 160-216,222-248,279-319 · credential and workspace KV joins · **SAFE** · The frames retain KV and local roster, state and environment owners. The recursive reload copies the KV handle and no pointer into the prior result crosses the recursion.

### `src/provider/claude_code_models.cpp`

Catalog fetch keeps the active task and its cache object alive independently of registry replacement.

- 123-166,188-249 · bounded page read, credential and workspace resolution · **SAFE** · Response, reader, scoped account and payload values live in each joined frame. The identity hash owns its material.
- 252-290,353-399,403-425 · single-flight cache and publication · **SAFE** · Refresh owns a shared cache object and re-finds registry membership after suspension. The caller waits on a copied promise, and Loop teardown clears pending reactions and their frame owners.
- 301-351 · catalog pagination and fetch · **SAFE** · The discovery frame owns the fetch callable and parsed page, and each body cancel is joined under its live handle.

### `src/provider/claude_code_quota.cpp`

Quota rows and health observations keep independent store and snapshot owners.

- 400-425 · accepted observation recorder · **SAFE** · The joined recorder receives KV and the observation by value and the function pointer is copied before invoking.
- 544-555,624-715 · quota CAS, retry, and completion · **SAFE** · Each state and row is a local snapshot. Receiver, keys and AtomicOperation stay live through joined reads, recursion and writes.

### `src/provider/claude_code_usage.cpp`

Usage fetch keeps the credential, fetch function, options, and response in its task frame.

- 27-41,128-196 · usage request and buffered read · **SAFE** · Parser input and response handles are frame-owned. Usage hooks are copied from owned options and the fetch is joined.

### `src/provider/claude_code_usage_refresh.cpp`

The work and cleanup closures are owned correctly. Two KV await expressions contain the same direct aggregate temporary.

- 22-34,36-72 · usage refresh task and capturing work coroutine · **SAFE** · rt::async retains the work closure and its KV, binding, clock and options captures through the refreshed usage write.
- 35,75 · braced read options in co_await · **DEFECT** · Each virtual get is awaited with an inline KvGetOptions aggregate. The port forbids that form in its GCC 12 comparison profile, even with this trivial value. Name the options and get task before the await.
- 74,76-83 · cleanup coroutine closure · **SAFE** · withFinally owns the cleanup callback throughout the final read/commit, including when the main work rejects.

### `src/provider/claude_code_usage_schema.cpp`

Usage and rate-limit schemas parse and serialize synchronously. Field views do not escape their source rows.

### `src/provider/health.cpp`

Provider observations are serialized per identity. Clock injection and the injectable KV boundary each have a distinct owner hazard.

- 120-151,173-180,292-331 · queued writes and promise reaction ownership · **SAFE** · The continuation and persistence frame own transition data, KV and transaction. The finalizer re-finds its queue entry, and loop teardown clears the registered promise reactions.
- 158-172 · reentrant clock callback and queue entry · **DEFECT** · With a pending distinct transition, an injected clock can synchronously clear the transition map via the test reset hook. The call then reads current->tail through a freed node. Re-find the entry after invoking the clock.
- 254-266 · KV receiver in fail-fast read fan-out · **DEFECT** · One virtual get can reject and drop the reader's KV owner while a sibling waits. If the fixture replaces the global KV and the sibling borrows its receiver, resumption reads a dead object. Give each read an owning child; the production guard and native store retain their own receiver state.
- 333-369 · forwarded view reads and binding lifetime · **SAFE** · Factories copy their inputs before returning. The Claude reader retains its binding and local KV while waiting on the owning health-view and account-entry lookups.

### `src/provider/lithos.cpp`

Wire normalization uses synchronous views. The stream generator and transport own separate read and dispatch state.

- 405-459,607-615 · borrowed parser projections · **SAFE** · Each message field pointer and frame substring is consumed in the synchronous caller before the source object or buffer changes.
- 636-666 · read/abort race callbacks · **SAFE** · The installed listener captures only a Promise rejector and a weak signal. The racing read uses reader-owned request state, and normal and error returns remove the listener.
- 680-711,722-756,975-982 · generator session, pending read and detached finish · **SAFE** · The generator and each pending nextFrame own the stream session. The constructor callback forwards copies into the frame, and reader.cancel obtains its own stream state before the reader lock is released.
- 809-876 · admission and fetch hooks, deadline callback · **SAFE** · The by-value options hold the hooks through their joined calls. Fetch retains its callable, and the deadline timer owns its controller until the frame guard clears it.

### `src/provider/lithos_handlers.cpp`

Immediate dispatch joins the caller. A streamed retry copies telemetry and keeps the cancellation controller in the pending task.

- 58-110,123-151,184-222 · joined dispatch and diagnostics · **SAFE** · The caller retains telemetry while the local error, admission, wait and HTTP body tasks run. The elapsed time callback owns its shared result.
- 224-250,255-269 · pending retry and cancellation · **SAFE** · resumeLithosDispatch takes an owned input with a copied UsageContext and controller. The stored raw context aliases that owned copy and the returned cancellation callback locks the controller weakly.
- 280-315,337-350,357-399 · handler waits and deferred stream transfer · **SAFE** · The handlers and joined buffered reader retain their arguments. Stream construction copies telemetry and models, and the pending reaction and getter share ownership of the chosen model.

### `src/provider/lithos_models.cpp`

The catalog coalesces through a module-owned promise and publishes an owning snapshot.

- 32-52 · catalog fetch and buffered decode · **SAFE** · The task owns options, timeout/signal handles, fetch function, key and response while waiting. The data view is used synchronously against the named JSON.
- 60-78 · coalesced promise and publication guard · **SAFE** · wait() copies the promise state before the registry can change, and the scope guard captures the exact promise identity rather than a reference into the optional.

### `src/provider/lithos_rate_limits.cpp`

Rate-limit parsing and failover-window lookups finish synchronously. The retry promise links a timer and once listener by weak back-references.

- 197-227 · failover-window map references · **SAFE** · Lookups and writes consume map entries synchronously without an intervening suspension or callback.
- 255-292 · rate-limit retry timer, listener, and executor · **SAFE** · newPromise runs the borrowing executor inline. The timer owns the wait state and signal, and the once listener keeps weak back-references and its own elapsed callback and rejector.

### `src/provider/lithos_streams.cpp`

The adapter forwards owned frames through the shared relay and SSE keepalive wrappers.

- 20-33 · generator body and frame forwarding · **SAFE** · The generator closure forwards its copied inputs into an owning task. forAwait retains the iterable and callback, and the callback returns an owned provider frame.
- 44-78,110-125 · provider adapter and deferred model callback · **SAFE** · The framing adapter owns the model getter, uses it synchronously and returns a generator owning the resolved model. The relay copies telemetry and pending source state before returning.

### `src/provider/metered.cpp`

Metered transport and metadata use owning fetch and body tasks. Its abort race retains the registered signal, and the coalescing cell can survive its abandoned loop.

- 193-230 · abort race listener ownership cycle · **DEFECT** · A pending catalog or billing request registers a listener whose own signal and finish captures keep the registering signal alive. Loop teardown can discard both operation reactions and the timeout before either branch removes the listener. Use weak signal ownership inside the listener and its finish helper.
- 243-274,334-365,462-487,611-661,777-884 · provider fetch, admission and body waits · **SAFE** · Frames own the request data, hooks, body and response handles. rt::Fetch and Response::json retain the underlying operations if their abort race settles first. The header timer is scoped to dispatch.
- 684-707,712-717 · coalescing cell after an abandoned wait · **DEFECT** · Destroying a fixture Loop with an unresolved coalesced discovery destroys its waiting frame before the explicit global clear. A later Loop in the same process can join that old Promise with no producer. Use an identity-checked scope guard as in lithos_models.cpp.
- 731-771 · pricing fail-fast fan-out · **SAFE** · Each fetchMetadataJson child owns its fetcher, URL, signal and error strings, and settled envelope references are to the local result vector.

### `src/provider/openrouter.cpp`

Catalog and transport requests own their parameters, and the public reference-taking entry points are synchronous forwarders.

- 18-32,177-185 · model lookup refresh and rejection continuation · **SAFE** · The child owns the model string and a resolver function pointer. The rejection reaction has no captures, and warm resolution uses fresh snapshots.
- 34-79,81-147 · dispatch hooks, deadline and body tasks · **SAFE** · Options own the hooks through invocation. Fetch and body operations retain their own state, and detached error-body cancellation keeps its stream state and a stateless rejection reaction.
- 151-175,187-207 · catalog snapshots and reference-taking factories · **SAFE** · Catalog scans retain a shared snapshot during synchronous iteration. The reference-taking entry points construct owned body and options arguments before returning the child task.

### `src/provider/openrouter_handlers.cpp`

Both handlers await dispatch and buffered reads in the request lifetime. The stream adapters take over copied telemetry.

- 25-60 · borrowed JSON walk · **SAFE** · Every pending pointer refers to the immutable local JSON root and is consumed before the synchronous helper returns.
- 74-97,125-134,157-166 · transport closure and telemetry borrow · **SAFE** · The dispatch frame owns the attempt callable through its awaited result. Handler locals and the request owner's UsageContext survive dispatch and the buffered capture, and stream factories copy telemetry.
- 110-140,142-176 · buffered body, status views and usage joins · **SAFE** · The handlers retain request snapshots and buffered payloads. The status view refers to the local payload, and bounded reads use a named task.

### `src/provider/openrouter_request_controls.cpp`

Extension selection copies its source body at the public API boundary.

- 16-30 · reference-taking task factory and catalog waits · **SAFE** · The synchronous wrapper copies the body into the task. Provider selection and model resolution return value snapshots and the model string is a frame local.

### `src/provider/openrouter_streams.cpp`

OpenRouter Chat owns its parser session and Responses passes owned telemetry into the shared proxy.

- 112-135 · read/abort race listener · **SAFE** · The executor runs inline. The signal owns a callback with a weak signal reference, and the reader request and rejector stay valid when their branch loses.
- 137-173,194-213,258-264 · stream session, watchdog and yielded frame · **SAFE** · The generator and each pending nextFrame own the session. The timer owns a controller copy, and detached cancellation owns its stream operation and a stateless rejection callback.
- 218-243,266-312 · relay and proxy callbacks · **SAFE** · Stream construction copies the framing adapter and telemetry. Proxy callbacks keep shared context and owned request ID and signal values for their synchronous invocations.

### `src/provider/openrouter_web_tools.cpp`

Tool projection uses synchronous views of live JSON and static tool names, then returns owning JSON.

### `src/provider/presentation.cpp`

Presentation tables and JSON conversion are synchronous and return copies.

### `src/provider/removed_provider_circuit.cpp`

The retired circuit returns settled promises with owned values and registers no asynchronous callbacks.

### `src/provider/removed_provider_telemetry.cpp`

The retired telemetry hook returns an already settled promise without retaining its argument.

### `src/provider/selection.cpp`

Provider selection reads and writes snapshots. Its in-flight token needs the same cleanup on frame destruction as on settlement.

- 82-96,102-107,181-204,258-262 · store, hash and selection snapshot ownership · **SAFE** · The frame owns KV through the joined child or virtual task. Hash and selection reads do not keep shared map entries across suspension.
- 82-98,264-277 · selection cache retains an abandoned load · **DEFECT** · Destroying a fixture Loop while KV is pending destroys the load frame without executing its global reset. A later Loop can reuse the stranded Promise. Move the token-checked reset into a frame scope guard.
- 149-176,238-252 · selection views and synchronous predicate · **SAFE** · Views are consumed while their caller-owned strings or KV values remain live. The predicate never leaves selectedCatalogEntryProviders.

### `src/provider/stream_relay.cpp`

The relay copies telemetry, retains the upstream until late cancellation is handled, and preserves the existing loop-shutdown cleanup for the late-abort callback.

- 33-43,100-122 · refusal buffer and detached body cancel · **SAFE** · The refusal frame owns its response and parsed JSON. The relay copies context and signals, and cancelPromise retains the detached body operation.
- 62-82,125-142 · late downstream abort and pending response · **SAFE** · The abort listener owns PendingCancellation and only a weak signal. The loop registry releases any callback-owned cycle at shutdown, and the pending upstream reaction owns relay state until settlement or loop teardown.
- 158-256,323-435 · member coroutines and awaited children · **SAFE** · SourceAlgorithm retains the state-capturing pull closure while the member pulls run and join their children. They use named iterator and response copies, so cancellation can reset the relay's copies independently.
- 259-270,437-458,465-474 · cancel closure and detached generator return · **SAFE** · The stream owns the source closures, cancellation is synchronous, and iterator return takes a separate generator state handle before spawning.

### `src/provider/surplus.cpp`

The Surplus stream projection owns its event and body state. Its header abort race and catalog coalescing cell repeat the Metered teardown hazards.

- 238-281,618-649,713-802 · transport, admission, and normalization owners · **SAFE** · The catalog and request tasks own options, signal and response snapshots, Fetch retains its callable if the race is lost, and the native response normalizer copies stream options.
- 318-354 · abort race listener ownership cycle · **DEFECT** · If the preheader fetch remains pending when its loop stops, the signal retains its abort listener and both signal and finish captures retain that signal. Loop teardown discards the timer and reactions without invoking finish. Make the listener and finish use weak signal state.
- 466-553 · generator yields, pull and cancel ownership · **SAFE** · The body owns its source iterator and part state, and each next result and vector live through yield. Pull and cancel receive a shared state parameter and their generator operations own their state.
- 675-698,703-708 · coalescing cell after an abandoned wait · **DEFECT** · Fixture loop destruction can destroy the suite's unfinished discovery waiter before the coalescing cell is reset. A later loop can join an old Promise with no producer. Use an identity-checked scope guard.

### `src/provider/surplus_claude_chat_request.cpp`

Native Chat projection, replay lookups and cache policy transforms are synchronous. Returned values own JSON and tool names.

- 357-405,512-545 · cache block projections · **SAFE** · The temporary pointer lists refer into the caller's private message and tool containers, which stay structurally unchanged while the selected inner object is updated.

### `src/provider/surplus_claude_request.cpp`

Native Messages projection copies JSON and names. Its reference and view parameters are synchronous.

- 452-505,632-672 · cache block and token bounds borrows · **SAFE** · Cache pointers refer to frame-local inputs while the synchronous call runs, and the local catalog token value stays in scope until its numeric value is copied.

## Outside Area C

- `src/kv.cpp:53-58,94-98`. In the injected opener seam, a capturing coroutine may
  remain suspended when `setKvForTest` replaces the function it came from. The next
  use of its captures uses a destroyed closure. Retain a copy of the selected opener
  beside the joined task. The production opener is a free function.
- The prior admin-kernel fan-out UAF and HTTP transport, WebSocket and relay cleanup
  findings are historical context. The assigned source contains the admin-kernel and
  relay ownership changes. This audit's relay test covers the late-abort contract.
  [Prior clang-tidy finding](cppgw-clang-tidy-audit-20261117.md) ·
  [Runtime sanitizer report](cppgw-asan-rt-20261117.md) ·
  [Provider sanitizer report](cppgw-asan-providers-20261117.md)

## Verification and handoff

Changed paths are this report, `analysis/lifetime/c/sites.json` and
`analysis/lifetime/c/checkpoint.py`. The checker reconciles the on-disk inventory with
the audited Git tree, the checked file list, valid source anchors, and byte-identical
verdict rows in the two artifacts. Proposed diffs were checked with `git apply --check`
against the audited source. An external `git archive` of the audited `include`, `src`
and `third_party` paths took the six diffs in rank order. On that copy, Clang 21 with
the ASan driver's recorded flags and `-fsyntax-only` accepted all 19 edited production
translation units in 25.439 s on four workers. The Voyage signature proposal still
requires migrating its direct cache-writer fixture callers to pass shared owners.
Maintainer application, functional regressions and the GCC 12 comparison remain open.

```sh
python3 -B cpp/gateway/analysis/lifetime/c/checkpoint.py verify --complete
python3 -B -c 'from pathlib import Path; p = Path("cpp/gateway/analysis/lifetime/c/checkpoint.py"); compile(p.read_text(), str(p), "exec")'
git diff --check 6d28e858e7ab8b8b1036db4a4adfdade0465ed40 HEAD
```

The report and JSON use the locked Prettier 3.9.6 under Bun 1.4.0. The
`tools/lint/package.json` and its frozen `bun.lock` were copied to staging under the
evidence root, where dependencies were installed with
`bun --no-env-file install --frozen-lockfile --ignore-scripts`. The formatter used the
checkout's `.prettierrc` and `.prettierignore` with only the two changed formatted paths.
Downloads and build artifacts stayed under `/var/tmp`.

Fetching `origin/development` succeeded. The task branch started from the assigned
port ref, with 220 base commits outside the fetched development history. GitHub
write authentication was unavailable for
`GIT_TERMINAL_PROMPT=0 git push --dry-run origin HEAD:refs/heads/audit/cppgw-lifetime-c-20261118`.
The audit commits are on `agent-job` for integration by the job owner. Development
ancestry verification and remote integration are pending.

## Ranked defects and counts

| Verdict   |    Rows |
| --------- | ------: |
| SAFE      |     368 |
| DEFECT    |      23 |
| UNCLEAR   |       0 |
| **Total** | **391** |

| Defect class                                            | Rows |
| ------------------------------------------------------- | ---: |
| Memory safety at injected coroutine, KV or clock seams  |   10 |
| Retained callback cycles at teardown                    |    7 |
| Abandoned shared-work cells surviving loop teardown     |    3 |
| GCC 12.2 comparison-profile await-expression violations |    3 |

The rank starts with request-path memory safety. The 23 rows are grouped below by
required repair, with separate anchors in the file ledger. The ownership and leak
witnesses identify two Codex rows. Other proposed repairs follow from the source and
caller ownership paths.

### 1. Retry task loses its callable on abort

`src/codex/dispatch.cpp:628-635`. The Responses retry helper accepts an injected sleep callable.
The abort branch rejects while a sleep coroutine returned by that callable remains suspended.
The waiter destroys its callable, so a later use of a capture in the sleep coroutine can access freed state.

The default sleep and the current differential fixture use owning free tasks. The isolated
weak-owner witness confirms early release with a capturing sleeper. The proposed change keeps
the original race and transfers the callable into its independently retained child.

```diff
--- a/cpp/gateway/src/codex/dispatch.cpp
+++ b/cpp/gateway/src/codex/dispatch.cpp
@@ -626,5 +626,6 @@
         if (abortSignal.aborted()) onAbort();
       });
-      rt::Promise<void> sleeping(sleep(milliseconds));
+      auto sleepTask = rt::async([sleep = std::move(sleep), milliseconds] { return sleep(milliseconds); });
+      rt::Promise<void> sleeping(std::move(sleepTask));
       std::vector<rt::Promise<void>> racers{sleeping, aborted};
       rt::Promise<void> raced = rt::race(std::move(racers));
```

### 2. Fail-fast KV children borrow their receivers

Eight rows cover concurrent reads in embeddings request cache, idempotency replay and eviction,
Codex manual reset and reset settings, paid-ledger backfill and settlement, and provider health.
One virtual read rejects while a sibling is still suspended. The caller can release the last
owner or replace the injected global store, leaving a pending virtual coroutine with a dead receiver.

The production GuardedKv and SQLite/MemoryKv implementations retain their own underlying state.
The exposed injection and override interfaces accept coroutine implementations. The proposals
give each started read an owner, preserving propagation of the first rejection and leaving
other reads free to settle. Voyage's writer signature transfers an owner on the same calls.
The direct cache writer fixture callers would also pass real shared owners when that API changes.

```diff
--- a/cpp/gateway/src/embeddings/handlers.cpp
+++ b/cpp/gateway/src/embeddings/handlers.cpp
@@ -137,5 +137,8 @@
         const auto unique = rt::arrayFrom(buckets.values());
         std::vector<rt::Promise<rt::KvEntryMaybe<>>> pending;
-        for (const auto& item : unique) pending.emplace_back(kv->get(embeddingsCacheKey(parsed.profile.cache_profile_key, item.hash)));
+        for (const auto& item : unique) {
+          const auto key = embeddingsCacheKey(parsed.profile.cache_profile_key, item.hash);
+          pending.emplace_back(rt::async([store = kv, key] { return store->get(key); }));
+        }
         const auto entries = co_await rt::all(std::move(pending)).wait();
         for (std::size_t i = 0; i < unique.size(); ++i) {
@@ -261,5 +264,5 @@
           for (auto idx : items[i].indices) vectorsByIndex[idx] = vectors[i];
           if (kv) {
-            auto task = writeEmbeddingsCacheEntryBestEffort(*kv, parsed.profile.cache_profile_key, items[i].hash, vectors[i], rt::dateNow(), deadlineMs);
+            auto task = writeEmbeddingsCacheEntryBestEffort(kv, parsed.profile.cache_profile_key, items[i].hash, vectors[i], rt::dateNow(), deadlineMs);
             const auto ignored = co_await jsAwait(std::move(task));
             (void)ignored;
--- a/cpp/gateway/src/embeddings/ledger.cpp
+++ b/cpp/gateway/src/embeddings/ledger.cpp
@@ -164,5 +164,5 @@
       for (double index = 0; index < *record.response_chunk_count; index += 1) {
         rt::KvKey chunkKey{lease.responseKeyPrefix, responseGeneration, index};
-        reads.push_back(rt::Promise<rt::KvEntryMaybe<>>(lease.kv->get(std::move(chunkKey))));
+        reads.emplace_back(rt::async([store = lease.kv, key = std::move(chunkKey)] { return store->get(key); }));
       }
       const std::vector<rt::KvEntryMaybe<>> chunks = co_await rt::all(std::move(reads)).wait();
--- a/cpp/gateway/include/uos/embeddings/voyage.hpp
+++ b/cpp/gateway/include/uos/embeddings/voyage.hpp
@@ -6,9 +6,10 @@
 #include "uos/rt/json.hpp"
 #include "uos/upstream_wire.hpp"
+#include <memory>
 #include <utility>

 namespace uos::embeddings::voyage {
   struct EmbeddingsCacheWriteResult { bool isNew; };
-  rt::Task<EmbeddingsCacheWriteResult> writeEmbeddingsCacheEntryBestEffort(rt::Kv& kv, std::string cacheProfileKey, std::string hash,
+  rt::Task<EmbeddingsCacheWriteResult> writeEmbeddingsCacheEntryBestEffort(std::shared_ptr<rt::Kv> kv, std::string cacheProfileKey, std::string hash,
                                                                        std::vector<double> embedding, double createdAtMs, double deadlineMs);
   std::string resolveEmbeddingsJobTokenSeed(const std::string& jobId, const std::optional<std::string>& authToken,
--- a/cpp/gateway/src/embeddings/jobs.cpp
+++ b/cpp/gateway/src/embeddings/jobs.cpp
@@ -295,5 +295,5 @@
         }
         for (std::size_t i = 0; i < chunk.size(); ++i) {
-          auto task = writeEmbeddingsCacheEntryBestEffort(*params.kv, currentJob.cache_profile_key, chunk[i].hash, upstream.vectors[i], rt::dateNow(), params.deadlineMs);
+          auto task = writeEmbeddingsCacheEntryBestEffort(params.kv, currentJob.cache_profile_key, chunk[i].hash, upstream.vectors[i], rt::dateNow(), params.deadlineMs);
           const auto ignored = co_await jsAwait(std::move(task));
           (void)ignored;
--- a/cpp/gateway/src/embeddings/voyage.cpp
+++ b/cpp/gateway/src/embeddings/voyage.cpp
@@ -124,14 +124,17 @@
       co_return CacheEvictionOutcome{commit.ok, false};
     }
-    rt::Task<EmbeddingsCacheEvictResult> evictOldestEmbeddingsCacheEntries(rt::Kv& kv, double count) {
-      const auto candidates = co_await jsAwait(listEmbeddingsCacheEvictionCandidates(kv, count));
+    rt::Task<EmbeddingsCacheEvictResult> evictOldestEmbeddingsCacheEntries(std::shared_ptr<rt::Kv> kv, double count) {
+      const auto candidates = co_await jsAwait(listEmbeddingsCacheEvictionCandidates(*kv, count));
       if (candidates.empty()) co_return EmbeddingsCacheEvictResult{0, 0};
       std::vector<rt::Promise<rt::KvEntryMaybe<>>> pending;
-      for (const auto& candidate : candidates) pending.emplace_back(kv.get(embeddingsCacheIndexByHashKey(candidate.cacheProfileKey, candidate.hash)));
+      for (const auto& candidate : candidates) {
+        const auto key = embeddingsCacheIndexByHashKey(candidate.cacheProfileKey, candidate.hash);
+        pending.emplace_back(rt::async([kv, key] { return kv->get(key); }));
+      }
       const auto byHashEntries = co_await rt::all(std::move(pending)).wait();
       double evictedEmbeddings = 0;
       double deletedStaleIndexKeys = 0;
       for (std::size_t i = 0; i < candidates.size(); ++i) {
-        auto task = evictEmbeddingsCacheCandidate(kv, candidates[i], byHashEntries[i]);
+        auto task = evictEmbeddingsCacheCandidate(*kv, candidates[i], byHashEntries[i]);
         const auto outcome = co_await jsAwait(std::move(task));
         if (outcome.evicted) ++evictedEmbeddings;
@@ -142,5 +145,5 @@
   }

-  rt::Task<EmbeddingsCacheWriteResult> writeEmbeddingsCacheEntryBestEffort(rt::Kv& kv, std::string cacheProfileKey, std::string hash,
+  rt::Task<EmbeddingsCacheWriteResult> writeEmbeddingsCacheEntryBestEffort(std::shared_ptr<rt::Kv> kv, std::string cacheProfileKey, std::string hash,
                                                                        std::vector<double> embedding, double createdAtMs, double deadlineMs) {
     double evictBatch = ledger::EMBEDDINGS_CACHE_EVICT_BATCH;
@@ -149,5 +152,5 @@
       rt::Reason error;
       try {
-        const auto result = co_await jsAwait(writeEmbeddingsCacheEntry(kv, cacheProfileKey, hash, embedding, createdAtMs));
+        const auto result = co_await jsAwait(writeEmbeddingsCacheEntry(*kv, cacheProfileKey, hash, embedding, createdAtMs));
         co_return result;
       } catch (...) { error = std::current_exception(); }
--- a/cpp/gateway/src/codex/manual_reset.cpp
+++ b/cpp/gateway/src/codex/manual_reset.cpp
@@ -86,5 +86,10 @@
       return rt::Math::max(exhausted.empty() ? std::span<const double>(deadlines) : std::span<const double>(exhausted));
     }
-    rt::Task<rt::KvEntryMaybe<>> readEntry(std::shared_ptr<rt::Kv> kv, rt::KvKey key) { return kv->get(std::move(key), {.consistency = rt::KvConsistencyLevel::strong}); }
+    rt::Task<rt::KvEntryMaybe<>> readEntry(std::shared_ptr<rt::Kv> kv, rt::KvKey key) {
+      const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
+      auto task = kv->get(std::move(key), strong);
+      auto entry = co_await std::move(task);
+      co_return entry;
+    }
     rt::Task<rt::KvEntryMaybe<>> readManualDailyBudget(std::shared_ptr<rt::Kv> kv, std::string hash, double now) {
       rt::Promise reading(readEntry(kv, banked_reset::codexResetAccountDailyKey(hash, day(now)))); const auto budget = co_await reading.wait();
--- a/cpp/gateway/src/codex/reset_settings.cpp
+++ b/cpp/gateway/src/codex/reset_settings.cpp
@@ -15,4 +15,10 @@
   namespace {
     rt::KvGetOptions strongRead() { return rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}; }
+
+    rt::Task<rt::KvEntryMaybe<>> readWithStore(std::shared_ptr<rt::Kv> kv, rt::KvKey key) {
+      auto task = kv->get(std::move(key), strongRead());
+      auto entry = co_await std::move(task);
+      co_return entry;
+    }

     // A stored row as the TS reads it (`unknown`): JSON-shaped parts as they are, an undefined member
@@ -62,6 +68,6 @@
     if (!marker.value.isNull()) co_return std::string("done");

-    rt::Promise<rt::KvEntryMaybe<>> legacyRead(kv->get(CODEX_BANKED_RESET_USAGE_LEGACY_KEY, strongRead()));
-    rt::Promise<rt::KvEntryMaybe<>> poolRead(kv->get(auth::CODEX_AUTH_POOL_KV_KEY, strongRead()));
+    rt::Promise<rt::KvEntryMaybe<>> legacyRead(readWithStore(kv, CODEX_BANKED_RESET_USAGE_LEGACY_KEY));
+    rt::Promise<rt::KvEntryMaybe<>> poolRead(readWithStore(kv, auth::CODEX_AUTH_POOL_KV_KEY));
     auto reads = rt::all(std::move(legacyRead), std::move(poolRead));
     const std::tuple<rt::KvEntryMaybe<>, rt::KvEntryMaybe<>> both = co_await reads.wait();
@@ -107,6 +113,6 @@
      */
     rt::Task<void> ensureLegacyGlobalOptOutMigratedForAccount(std::shared_ptr<rt::Kv> kv, std::string accountIdHash) {
-      rt::Promise<rt::KvEntryMaybe<>> markerRead(kv->get(CODEX_RESET_USAGE_MIGRATION_KEY, strongRead()));
-      rt::Promise<rt::KvEntryMaybe<>> legacyRead(kv->get(CODEX_BANKED_RESET_USAGE_LEGACY_KEY, strongRead()));
+      rt::Promise<rt::KvEntryMaybe<>> markerRead(readWithStore(kv, CODEX_RESET_USAGE_MIGRATION_KEY));
+      rt::Promise<rt::KvEntryMaybe<>> legacyRead(readWithStore(kv, CODEX_BANKED_RESET_USAGE_LEGACY_KEY));
       auto reads = rt::all(std::move(markerRead), std::move(legacyRead));
       const std::tuple<rt::KvEntryMaybe<>, rt::KvEntryMaybe<>> both = co_await reads.wait();
--- a/cpp/gateway/src/paid_fallback/ledger_backfill.cpp
+++ b/cpp/gateway/src/paid_fallback/ledger_backfill.cpp
@@ -451,7 +451,10 @@
     rt::Task<bool> paidFallbackBackfillNeedsRun(std::shared_ptr<rt::Kv> kv) {
       const rt::KvGetOptions strong = strongRead();
-      const rt::Promise<rt::KvEntryMaybe<>> stateRead(kv->get(native_paid::paidFallbackBackfillStateV3Key(), strong));
-      const rt::Promise<rt::KvEntryMaybe<>> requestCursorRead(kv->get(native_paid::paidFallbackBackfillCursorV3Key(), strong));
-      const rt::Promise<rt::KvEntryMaybe<>> windowCursorRead(kv->get(native_paid::paidFallbackBackfillWindowCursorV3Key(), strong));
+      const auto read = [kv, strong](rt::KvKey key) {
+        return rt::async([kv, strong, key = std::move(key)] { return kv->get(key, strong); });
+      };
+      const rt::Promise<rt::KvEntryMaybe<>> stateRead(read(native_paid::paidFallbackBackfillStateV3Key()));
+      const rt::Promise<rt::KvEntryMaybe<>> requestCursorRead(read(native_paid::paidFallbackBackfillCursorV3Key()));
+      const rt::Promise<rt::KvEntryMaybe<>> windowCursorRead(read(native_paid::paidFallbackBackfillWindowCursorV3Key()));
       auto reads = rt::all(stateRead, requestCursorRead, windowCursorRead);
       auto readEntries = co_await reads.wait();
--- a/cpp/gateway/src/paid_fallback/ledger_settlement.cpp
+++ b/cpp/gateway/src/paid_fallback/ledger_settlement.cpp
@@ -33,4 +33,10 @@
     using rt::record::Reader;
     using rt::record::Writer;
+
+    rt::Task<rt::KvEntryMaybe<>> readWithStore(std::shared_ptr<rt::Kv> kv, rt::KvKey key, rt::KvGetOptions options) {
+      auto task = kv->get(std::move(key), options);
+      auto entry = co_await std::move(task);
+      co_return entry;
+    }
     namespace contracts = authority::native_paid_contracts;
     namespace native_paid = authority::native_paid;
@@ -266,6 +272,6 @@
     const rt::KvGetOptions strong = strongRead();
     for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
-      const rt::Promise<rt::KvEntryMaybe<>> requestRead(kv->get(requestKey, strong));
-      const rt::Promise<rt::KvEntryMaybe<>> pendingRead(kv->get(pendingKey, strong));
+      const rt::Promise<rt::KvEntryMaybe<>> requestRead(readWithStore(kv, requestKey, strong));
+      const rt::Promise<rt::KvEntryMaybe<>> pendingRead(readWithStore(kv, pendingKey, strong));
       auto reads = rt::all(requestRead, pendingRead);
       auto readEntries = co_await reads.wait();
@@ -321,7 +327,7 @@
     const rt::KvGetOptions strong = strongRead();
     for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
-      const rt::Promise<rt::KvEntryMaybe<>> requestRead(kv->get(requestKey, strong));
-      const rt::Promise<rt::KvEntryMaybe<>> pendingRead(kv->get(pendingKey, strong));
-      const rt::Promise<rt::KvEntryMaybe<>> gateRead(kv->get(gateKey, strong));
+      const rt::Promise<rt::KvEntryMaybe<>> requestRead(readWithStore(kv, requestKey, strong));
+      const rt::Promise<rt::KvEntryMaybe<>> pendingRead(readWithStore(kv, pendingKey, strong));
+      const rt::Promise<rt::KvEntryMaybe<>> gateRead(readWithStore(kv, gateKey, strong));
       auto reads = rt::all(requestRead, pendingRead, gateRead);
       auto readEntries = co_await reads.wait();
--- a/cpp/gateway/src/provider/health.cpp
+++ b/cpp/gateway/src/provider/health.cpp
@@ -257,5 +257,8 @@
       if (!kv) co_return unknownView();
       std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
-      for (const std::string_view record : PROVIDER_RECORDS) reads.push_back(rt::Promise<rt::KvEntryMaybe<>>(kv->get(providerHealthKey(provider, identity, record))));
+      for (const std::string_view record : PROVIDER_RECORDS) {
+        const auto key = providerHealthKey(provider, identity, record);
+        reads.emplace_back(rt::async([kv, key] { return kv->get(key); }));
+      }
       auto everything = rt::all(std::move(reads));
       const std::vector<rt::KvEntryMaybe<>> entries = co_await everything.wait();
```

### 3. Reentrant clock invalidates a provider transition

`src/provider/health.cpp:158-172`. A distinct transition is queued with an injected clock.
The clock calls the test reset while the new transition holds its old map-node pointer.
The enqueue path then reads `current->tail` from the erased node. The production clock is
`rt::dateNow`. Re-find the current tail after the externally callable clock returns.

```diff
--- a/cpp/gateway/src/provider/health.cpp
+++ b/cpp/gateway/src/provider/health.cpp
@@ -170,5 +170,6 @@
       // Identical cold-burst observations share this promise, while a different
       // event or the first available correlation ID is appended in arrival order.
-      const rt::Promise<void> previous = current ? current->tail : rt::Promise<void>::resolve();
+      const auto latest = providerTransitionQueues().find(queueKey);
+      const rt::Promise<void> previous = latest != providerTransitionQueues().end() ? latest->second.tail : rt::Promise<void>::resolve();
       const rt::Promise<void> tail = previous.catch_([](const rt::Reason&) {}).then([provider, identity, event, status, observedAtMs, normalizedRequestId, binding]() {
         return persistProviderHealth(provider, identity, event, status, observedAtMs, normalizedRequestId, binding);
```

### 4. Seven callback ownership cycles survive abandoned work

| Site                                     | Interleaving                                                                                                                                                                                  |
| ---------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `codex/auth_refresh.cpp:535-548`         | A shared refresh remains pending and the loop drops its completion reactions. The registered listener still owns the abort signal that owns it. LSan measured 776 bytes in seven allocations. |
| `codex/dispatch.cpp:623-635`             | Retry waiting ends by frame destruction before the sleeper or abort promise settles. The explicit removal is skipped and the listener retains its own registering signal.                     |
| `codex/supervisor_transport.cpp:134-140` | Teardown drops socket and timer callbacks while a call is pending. ConnectionState's pending cleanup retains a listener which retains that same ConnectionState.                              |
| `embeddings/voyage.cpp:198-207`          | Loop teardown drops a pending retry timer. The once listener still forms a strong cycle with its signal until an abort which may never run.                                                   |
| `provider/claude_code.cpp:100-119`       | The frame is destroyed while both transport and abort are pending. The dispose function is discarded before it can break the listener's strong signal cycle.                                  |
| `provider/metered.cpp:193-230`           | A catalog or billing read and its abort branch remain pending at teardown. The installed listener and its finish closure both retain the signal that owns the listener.                       |
| `provider/surplus.cpp:318-354`           | The preheader transport and its abort branch remain pending while the loop destroys the timer and reactions. The listener's two strong paths to the registering signal remain.                |

Keep late callbacks intact while their owners are live. Weak back-references or a frame cleanup
break the cycles without cancelling shared work or changing the first settlement.

```diff
--- a/cpp/gateway/src/codex/auth_refresh.cpp
+++ b/cpp/gateway/src/codex/auth_refresh.cpp
@@ -537,7 +537,11 @@
     const std::weak_ptr<rt::EventListener> listenerHandle = listenerSlot;
     rt::Promise<T> abortion = rt::newPromise<T>([&](auto, rt::Reject reject) {
-      *listenerSlot = rt::EventListener([abortSignal, listenerHandle, reject]() {
-        if (const std::shared_ptr<rt::EventListener> listener = listenerHandle.lock()) abortSignal.removeEventListener("abort", *listener);
-        reject(abortReasonAsError(abortSignal.reason()));
+      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = abortSignal.state();
+      *listenerSlot = rt::EventListener([weakSignal, listenerHandle, reject]() {
+        const auto state = weakSignal.lock();
+        if (!state) return;
+        const rt::AbortSignal active(state);
+        if (const auto listener = listenerHandle.lock()) active.removeEventListener("abort", *listener);
+        reject(abortReasonAsError(active.reason()));
       });
       abortSignal.addEventListener("abort", *listenerSlot, rt::AddEventListenerOptions{.once = true});
--- a/cpp/gateway/src/codex/dispatch.cpp
+++ b/cpp/gateway/src/codex/dispatch.cpp
@@ -618,4 +618,5 @@
     }
     rt::EventListener onAbort;
+    const auto removeListener = rt::scopeExit([&] { signal->removeEventListener("abort", onAbort); });
     rt::Reason failure;
     try {
@@ -633,5 +634,4 @@
       failure = std::current_exception();
     }
-    signal->removeEventListener("abort", onAbort);
     if (failure) std::rethrow_exception(failure);
   }
--- a/cpp/gateway/src/codex/supervisor_transport.cpp
+++ b/cpp/gateway/src/codex/supervisor_transport.cpp
@@ -132,5 +132,7 @@
         auto self = shared_from_this();
         return rt::newPromise<SupervisorCallResult>([&](const rt::Resolve<SupervisorCallResult>& resolve, const rt::Reject& reject) {
-          const rt::EventListener onAbort([self, id] { self->settle(id, rt::makeReason(rt::JsError("app-server call aborted"))); });
+          const rt::EventListener onAbort([weak = weak_from_this(), id] {
+            if (const auto state = weak.lock()) state->settle(id, rt::makeReason(rt::JsError("app-server call aborted")));
+          });
           const rt::Timeout timer = rt::setTimeout([self, id] { self->settle(id, rt::makeReason(rt::JsError("app-server call timed out"))); }, DEFAULT_CALL_TIMEOUT_MS);
           std::function<void()> cleanup = [callSignal, onAbort] {
--- a/cpp/gateway/src/embeddings/voyage.cpp
+++ b/cpp/gateway/src/embeddings/voyage.cpp
@@ -197,8 +197,7 @@
       }, ms);
       const std::weak_ptr<State> weak = state;
-      state->onAbort = rt::EventListener([signal, weak, resolve] {
+      state->onAbort = rt::EventListener([weak, resolve] {
         if (const auto current = weak.lock()) {
           rt::clearTimeout(current->timeout);
-          signal.removeEventListener("abort", current->onAbort);
           resolve(false);
         }
--- a/cpp/gateway/src/provider/claude_code.cpp
+++ b/cpp/gateway/src/provider/claude_code.cpp
@@ -102,5 +102,8 @@
       std::function<void()> dispose = [] {};
       rt::Promise<rt::Response> aborted = rt::newPromise<rt::Response>([&](const rt::Resolve<rt::Response>&, const rt::Reject& reject) {
-        const rt::EventListener onAbort([reject, signal] { reject(abortReasonError(signal)); });
+        const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal.state();
+        const rt::EventListener onAbort([reject, weakSignal] {
+          if (const auto active = weakSignal.lock()) reject(abortReasonError(rt::AbortSignal(active)));
+        });
         signal.addEventListener("abort", onAbort, {.once = true});
         dispose = [signal, onAbort] { signal.removeEventListener("abort", onAbort); };
--- a/cpp/gateway/src/provider/metered.cpp
+++ b/cpp/gateway/src/provider/metered.cpp
@@ -204,15 +204,17 @@
       const rt::Reject reject = resolvers.reject;
       auto race = std::make_shared<AbortRace>();
-      const auto finish = [signal](AbortRace& state) {
+      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal.state();
+      const auto finish = [weakSignal](AbortRace& state) {
         if (state.settled) return false;
         state.settled = true;
-        signal.removeEventListener("abort", state.onAbort);
+        if (const auto active = weakSignal.lock()) rt::AbortSignal(active).removeEventListener("abort", state.onAbort);
         return true;
       };
       const std::weak_ptr<AbortRace> weakRace = race;
-      race->onAbort = rt::EventListener([weakRace, signal, finish, reject]() {
+      race->onAbort = rt::EventListener([weakRace, weakSignal, finish, reject]() {
         const std::shared_ptr<AbortRace> state = weakRace.lock();
-        if (!state || !finish(*state)) return;
-        reject(asRejectionReason(reasonOr(signal.reason(), rt::makeReason(rt::DOMException("Aborted", "AbortError")))));
+        const auto active = weakSignal.lock();
+        if (!state || !active || !finish(*state)) return;
+        reject(asRejectionReason(reasonOr(rt::AbortSignal(active).reason(), rt::makeReason(rt::DOMException("Aborted", "AbortError")))));
       });
       if (signal.aborted()) {
--- a/cpp/gateway/src/provider/surplus.cpp
+++ b/cpp/gateway/src/provider/surplus.cpp
@@ -328,15 +328,17 @@
       const rt::Reject reject = resolvers.reject;
       auto race = std::make_shared<AbortRace>();
-      const auto finish = [signal](AbortRace& state) {
+      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal.state();
+      const auto finish = [weakSignal](AbortRace& state) {
         if (state.settled) return false;
         state.settled = true;
-        signal.removeEventListener("abort", state.onAbort);
+        if (const auto active = weakSignal.lock()) rt::AbortSignal(active).removeEventListener("abort", state.onAbort);
         return true;
       };
       const std::weak_ptr<AbortRace> weakRace = race;
-      race->onAbort = rt::EventListener([weakRace, signal, finish, reject]() {
+      race->onAbort = rt::EventListener([weakRace, weakSignal, finish, reject]() {
         const std::shared_ptr<AbortRace> state = weakRace.lock();
-        if (!state || !finish(*state)) return;
-        reject(abortRejectionReason(signal.reason()));
+        const auto active = weakSignal.lock();
+        if (!state || !active || !finish(*state)) return;
+        reject(abortRejectionReason(rt::AbortSignal(active).reason()));
       });
       if (signal.aborted()) {
```

### 5. A coalescing cell outlives its producer

`provider/metered.cpp:684-707`, `provider/selection.cpp:82-98,264-277` and
`provider/surplus.cpp:675-698`. A scoped fixture Loop is destroyed with a shared lookup
still waiting. Its coroutine frame is destroyed before the explicit cache reset.
The same process can later reuse the still-pending promise with no producer to settle it.

Clear the matching entry from a frame guard, using the existing Lithos pattern and preserving
the selection generation token. This applies to a process that reuses the APIs across loops.

```diff
--- a/cpp/gateway/src/provider/metered.cpp
+++ b/cpp/gateway/src/provider/metered.cpp
@@ -696,13 +696,8 @@
     }
     meteredModelsFetchInFlight = request;
-    std::optional<MeteredModelsSnapshot> result;
-    rt::Reason failure;
-    try {
-      result = co_await request.wait();
-    } catch (...) {
-      failure = std::current_exception();
-    }
-    if (meteredModelsFetchInFlight && *meteredModelsFetchInFlight == request) meteredModelsFetchInFlight.reset();
-    if (failure) std::rethrow_exception(failure);
+    const auto cleanup = rt::scopeExit([request] {
+      if (meteredModelsFetchInFlight && *meteredModelsFetchInFlight == request) meteredModelsFetchInFlight.reset();
+    });
+    auto result = co_await request.wait();
     co_return result;
   }
--- a/cpp/gateway/src/provider/selection.cpp
+++ b/cpp/gateway/src/provider/selection.cpp
@@ -82,4 +82,7 @@
     rt::Task<std::optional<ProviderSelection>> loadSelectionIntoCache(double nowMs, std::optional<ProviderSelection> stale, double cacheGenerationAtLoad,
                                                                       double loadToken) {
+      const auto cleanup = rt::scopeExit([loadToken] {
+        if (selectionLoadToken == loadToken) selectionLoadInFlight.reset();
+      });
       std::optional<ProviderSelection> result;
       try {
@@ -95,5 +98,4 @@
         result = std::move(stale);
       }
-      if (selectionLoadToken == loadToken) selectionLoadInFlight.reset();
       co_return result;
     }
--- a/cpp/gateway/src/provider/surplus.cpp
+++ b/cpp/gateway/src/provider/surplus.cpp
@@ -687,13 +687,8 @@
     }
     surplusModelsFetchInFlight = request;
-    std::optional<SurplusModelsSnapshot> result;
-    rt::Reason failure;
-    try {
-      result = co_await request.wait();
-    } catch (...) {
-      failure = std::current_exception();
-    }
-    if (surplusModelsFetchInFlight && *surplusModelsFetchInFlight == request) surplusModelsFetchInFlight.reset();
-    if (failure) std::rethrow_exception(failure);
+    const auto cleanup = rt::scopeExit([request] {
+      if (surplusModelsFetchInFlight && *surplusModelsFetchInFlight == request) surplusModelsFetchInFlight.reset();
+    });
+    auto result = co_await request.wait();
     co_return result;
   }
```

### 6. Await expressions violate the GCC 12 comparison rule

The Claude Chat and Messages normalizers pass owning aggregates by value in the await
expression. The usage refresh includes two braced KV option temporaries in awaited calls.
In the documented GCC 12.2 failure pattern, suspension crosses an incorrectly managed temporary
and resumption can destroy it twice. This is a portability finding based on `PORTING.md`.
The Clang 21 run does not establish failure for those exact expressions under GCC 12.2.
Build the options and the task before suspending.

```diff
--- a/cpp/gateway/src/provider/anthropic_chat_responses.cpp
+++ b/cpp/gateway/src/provider/anthropic_chat_responses.cpp
@@ -517,5 +517,6 @@
     readOptions.maxBytes = MAX_RESPONSES_SSE_EVENT_BYTES;
     readOptions.timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS;
-    const bounded_response_body::BoundedResponseBody captured = co_await bounded_response_body::readBoundedResponseBody(response, std::move(readOptions));
+    auto reading = bounded_response_body::readBoundedResponseBody(response, std::move(readOptions));
+    const bounded_response_body::BoundedResponseBody captured = co_await std::move(reading);
     if (options.signal && options.signal->aborted()) {
       if (rt::isError(options.signal->reason())) rt::rethrow(options.signal->reason());
--- a/cpp/gateway/src/provider/anthropic_responses.cpp
+++ b/cpp/gateway/src/provider/anthropic_responses.cpp
@@ -777,5 +777,6 @@
       readOptions.signal = options.signal;
       const rt::Response cloned = response.clone();
-      const bounded_response_body::BoundedResponseBody body = co_await bounded_response_body::readBoundedResponseBody(cloned, std::move(readOptions));
+      auto reading = bounded_response_body::readBoundedResponseBody(cloned, std::move(readOptions));
+      const bounded_response_body::BoundedResponseBody body = co_await std::move(reading);
       if (options.signal && options.signal->aborted()) rt::rethrow(abortError(*options.signal));
       if (!body.complete) co_return response;
@@ -855,5 +856,6 @@
   rt::Task<rt::Response> normalizeAnthropicResponsesResponse(rt::Response response, AnthropicResponsesOptions options) {
     if (!response.ok()) {
-      rt::Response normalized = co_await normalizeMessagesHttpError(response, options);
+      auto normalizing = normalizeMessagesHttpError(response, options);
+      rt::Response normalized = co_await std::move(normalizing);
       co_return normalized;
     }
@@ -878,5 +880,6 @@
     readOptions.maxBytes = MAX_RESPONSES_SSE_EVENT_BYTES;
     readOptions.timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS;
-    const bounded_response_body::BoundedResponseBody body = co_await bounded_response_body::readBoundedResponseBody(response, std::move(readOptions));
+    auto reading = bounded_response_body::readBoundedResponseBody(response, std::move(readOptions));
+    const bounded_response_body::BoundedResponseBody body = co_await std::move(reading);
     if (options.signal && options.signal->aborted()) rt::rethrow(abortError(*options.signal));
     if (!body.complete) throw ResponsesStreamError("Anthropic Messages body was incomplete or exceeded its bound.", {.cause = nullptr, .kind = "read_error"});
--- a/cpp/gateway/src/provider/claude_code_usage_refresh.cpp
+++ b/cpp/gateway/src/provider/claude_code_usage_refresh.cpp
@@ -33,5 +33,7 @@
     if (quota && quota->usage && claudeCodeUsageIsFresh(quota, nowMs)) co_return ClaudeCodeUsageRefreshFresh{*quota->usage};
     const rt::KvKey key{"uos_ai", "claude_code_usage", "v1", "lease", binding.account_id, binding.account_cohort_id};
-    const auto entry = co_await kv->get(key, {.consistency = rt::KvConsistencyLevel::strong});
+    const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
+    auto reading = kv->get(key, strong);
+    const auto entry = co_await std::move(reading);
     const auto lease = rt::storedJsonView(entry.value);
     if (entry.value.truthy() && rt::toNumber(rt::get(lease, "until_ms")) > nowMs) co_return ClaudeCodeUsageRefreshBusy{};
@@ -73,5 +75,7 @@
     };
     auto cleanup = [kv, key, owner]() -> rt::Task<void> {
-      const auto current = co_await kv->get(key, {.consistency = rt::KvConsistencyLevel::strong});
+      const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
+      auto reading = kv->get(key, strong);
+      const auto current = co_await std::move(reading);
       if (rt::getString(rt::storedJsonView(current.value), "owner") == owner) {
         auto deletion = kv->atomic(); deletion->check(current).delete_(key); co_await deletion->commit();
```

### Handoff summary

Reviewed 290 of 290 Area C files. The ledger records 368 SAFE, 23 DEFECT and
0 UNCLEAR. Clang-tidy completed 144 of 144 selected translation units. Nine focused
ASan/UBSan targets passed all 125 fixture cases. The separate Codex witnesses
observed a released retry sleeper closure and a 776-byte shared-refresh leak.
The commits contain the report, ledger and audit checker. GitHub write
authentication was unavailable, so remote integration is pending.
