# Native gateway coroutine and callback lifetime audit, Area C

## Contract v1

Audit all first-party source and headers for the eight assigned gateway modules. Trace coroutine
closure, parameter, callback, iterator and await-expression lifetime through normal return, errors,
cancellation and late completion. Report defects with failing interleavings and proposed diffs.
Production files, tests, dependencies, workflows and deployments are outside the change scope.

| Surface | Behavior to preserve |
| --- | --- |
| Provider, Codex, DeepSeek and paid fallback | Provider order, quota/accounting settlement, delivery receipts, fail-fast sibling semantics |
| Chat, Harmony and embeddings | Stream iteration and cancellation, handler/job ownership through completion |
| Live and supervisor | Callback registration and removal, socket/stream shutdown, delivery and abort ownership |

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
- [ ] `src/harmony/tools/router.cpp`
- [ ] `src/harmony/tools/schemas.cpp`
- [ ] `src/harmony/types.cpp`
- [ ] `src/live/calls.cpp`
- [ ] `src/live/handler.cpp`
- [ ] `src/live/routes.cpp`
- [ ] `src/live/sideband.cpp`
- [ ] `src/live/upstream.cpp`
- [ ] `src/paid_fallback/claude_code_route.cpp`
- [ ] `src/paid_fallback/health.cpp`
- [ ] `src/paid_fallback/index.cpp`
- [ ] `src/paid_fallback/ledger_admission.cpp`
- [ ] `src/paid_fallback/ledger_backfill.cpp`
- [ ] `src/paid_fallback/ledger_settlement.cpp`
- [ ] `src/paid_fallback/ledger_state.cpp`
- [ ] `src/paid_fallback/ledger_stats.cpp`
- [ ] `src/paid_fallback/removed_provider.cpp`
- [ ] `src/paid_fallback/rollups.cpp`
- [ ] `src/paid_fallback/routing.cpp`
- [ ] `src/provider/anthropic_chat_responses.cpp`
- [ ] `src/provider/anthropic_chat_stream.cpp`
- [ ] `src/provider/anthropic_replay.cpp`
- [ ] `src/provider/anthropic_response_wire.cpp`
- [ ] `src/provider/anthropic_responses.cpp`
- [ ] `src/provider/capacity.cpp`
- [ ] `src/provider/capacity_analytics.cpp`
- [ ] `src/provider/capacity_contract.cpp`
- [ ] `src/provider/capacity_events.cpp`
- [ ] `src/provider/capacity_history.cpp`
- [ ] `src/provider/capacity_parse.cpp`
- [ ] `src/provider/capacity_rollups.cpp`
- [ ] `src/provider/cerebras.cpp`
- [ ] `src/provider/cerebras_handlers.cpp`
- [ ] `src/provider/cerebras_rate_limits.cpp`
- [ ] `src/provider/cerebras_responses.cpp`
- [ ] `src/provider/claude_capacity_history.cpp`
- [ ] `src/provider/claude_capacity_rollups.cpp`
- [ ] `src/provider/claude_code.cpp`
- [ ] `src/provider/claude_code_access.cpp`
- [ ] `src/provider/claude_code_accounts.cpp`
- [ ] `src/provider/claude_code_credentials.cpp`
- [ ] `src/provider/claude_code_models.cpp`
- [ ] `src/provider/claude_code_quota.cpp`
- [ ] `src/provider/claude_code_usage.cpp`
- [ ] `src/provider/claude_code_usage_refresh.cpp`
- [ ] `src/provider/claude_code_usage_schema.cpp`
- [ ] `src/provider/health.cpp`
- [ ] `src/provider/lithos.cpp`
- [ ] `src/provider/lithos_handlers.cpp`
- [ ] `src/provider/lithos_models.cpp`
- [ ] `src/provider/lithos_rate_limits.cpp`
- [ ] `src/provider/lithos_streams.cpp`
- [ ] `src/provider/metered.cpp`
- [ ] `src/provider/openrouter.cpp`
- [ ] `src/provider/openrouter_handlers.cpp`
- [ ] `src/provider/openrouter_request_controls.cpp`
- [ ] `src/provider/openrouter_streams.cpp`
- [ ] `src/provider/openrouter_web_tools.cpp`
- [ ] `src/provider/presentation.cpp`
- [ ] `src/provider/removed_provider_circuit.cpp`
- [ ] `src/provider/removed_provider_telemetry.cpp`
- [ ] `src/provider/selection.cpp`
- [ ] `src/provider/stream_relay.cpp`
- [ ] `src/provider/surplus.cpp`
- [ ] `src/provider/surplus_claude_chat_request.cpp`
- [ ] `src/provider/surplus_claude_request.cpp`

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
- 538,543-549 · executor and sibling lifetime · **SAFE** · newPromise calls its executor synchronously, and the abort race passes only owning promise values. Its shared-work finalizer owns the exact listener identity to remove on settlement.
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
- 332-357,440-465,565-603 · hash, duplicate, and credit scan awaits · **SAFE** · The fence and inventory slices refer to immutable frame-owned inputs. Inventory and selection values remain alive through the awaited hash.
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
- 511-594,600-714,718-763 · cohort, inventory, and overage awaits · **SAFE** · Loops borrow from local candidate, observation and selection snapshots. Options, provider handles, fences and context stay owned through their joins.
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
