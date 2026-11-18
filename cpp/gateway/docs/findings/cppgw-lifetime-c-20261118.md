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
- [ ] `include/uos/deepseek/waterfall_handler.hpp`
- [ ] `include/uos/embeddings/handlers.hpp`
- [ ] `include/uos/embeddings/jobs.hpp`
- [ ] `include/uos/embeddings/ledger.hpp`
- [ ] `include/uos/embeddings/voyage.hpp`
- [ ] `include/uos/harmony/adapter.hpp`
- [ ] `include/uos/harmony/classifier.hpp`
- [ ] `include/uos/harmony/conversation.hpp`
- [ ] `include/uos/harmony/parse.hpp`
- [ ] `include/uos/harmony/probe_scenarios.hpp`
- [ ] `include/uos/harmony/probes.hpp`
- [ ] `include/uos/harmony/reliability/context.hpp`
- [ ] `include/uos/harmony/reliability/failure.hpp`
- [ ] `include/uos/harmony/reliability/feedback.hpp`
- [ ] `include/uos/harmony/reliability/harness.hpp`
- [ ] `include/uos/harmony/reliability/hash.hpp`
- [ ] `include/uos/harmony/reliability/loops.hpp`
- [ ] `include/uos/harmony/reliability/retry.hpp`
- [ ] `include/uos/harmony/reliability/state.hpp`
- [ ] `include/uos/harmony/reliability/surfaces.hpp`
- [ ] `include/uos/harmony/reliability/verify.hpp`
- [ ] `include/uos/harmony/render.hpp`
- [ ] `include/uos/harmony/tools/backend.hpp`
- [ ] `include/uos/harmony/tools/fakes.hpp`
- [ ] `include/uos/harmony/tools/result.hpp`
- [ ] `include/uos/harmony/tools/router.hpp`
- [ ] `include/uos/harmony/tools/schemas.hpp`
- [ ] `include/uos/harmony/types.hpp`
- [ ] `include/uos/live/calls.hpp`
- [ ] `include/uos/live/handler.hpp`
- [ ] `include/uos/live/routes.hpp`
- [ ] `include/uos/live/sideband.hpp`
- [ ] `include/uos/live/upstream.hpp`
- [ ] `include/uos/paid_fallback/claude_code_route.hpp`
- [ ] `include/uos/paid_fallback/health.hpp`
- [ ] `include/uos/paid_fallback/index.hpp`
- [ ] `include/uos/paid_fallback/ledger_admission.hpp`
- [ ] `include/uos/paid_fallback/ledger_backfill.hpp`
- [ ] `include/uos/paid_fallback/ledger_settlement.hpp`
- [ ] `include/uos/paid_fallback/ledger_state.hpp`
- [ ] `include/uos/paid_fallback/ledger_stats.hpp`
- [ ] `include/uos/paid_fallback/removed_provider.hpp`
- [ ] `include/uos/paid_fallback/rollups.hpp`
- [ ] `include/uos/paid_fallback/routing.hpp`
- [ ] `include/uos/provider/anthropic_chat_responses.hpp`
- [ ] `include/uos/provider/anthropic_chat_stream.hpp`
- [ ] `include/uos/provider/anthropic_replay.hpp`
- [ ] `include/uos/provider/anthropic_response_wire.hpp`
- [ ] `include/uos/provider/anthropic_responses.hpp`
- [ ] `include/uos/provider/capacity.hpp`
- [ ] `include/uos/provider/capacity_analytics.hpp`
- [ ] `include/uos/provider/capacity_contract.hpp`
- [ ] `include/uos/provider/capacity_events.hpp`
- [ ] `include/uos/provider/capacity_history.hpp`
- [ ] `include/uos/provider/capacity_parse.hpp`
- [ ] `include/uos/provider/capacity_rollups.hpp`
- [ ] `include/uos/provider/cerebras.hpp`
- [ ] `include/uos/provider/cerebras_handlers.hpp`
- [ ] `include/uos/provider/cerebras_rate_limits.hpp`
- [ ] `include/uos/provider/cerebras_responses.hpp`
- [ ] `include/uos/provider/claude_capacity_history.hpp`
- [ ] `include/uos/provider/claude_capacity_rollups.hpp`
- [ ] `include/uos/provider/claude_code.hpp`
- [ ] `include/uos/provider/claude_code_access.hpp`
- [ ] `include/uos/provider/claude_code_accounts.hpp`
- [ ] `include/uos/provider/claude_code_credentials.hpp`
- [ ] `include/uos/provider/claude_code_fwd.hpp`
- [ ] `include/uos/provider/claude_code_models.hpp`
- [ ] `include/uos/provider/claude_code_quota.hpp`
- [ ] `include/uos/provider/claude_code_usage.hpp`
- [ ] `include/uos/provider/claude_code_usage_fwd.hpp`
- [ ] `include/uos/provider/claude_code_usage_refresh.hpp`
- [ ] `include/uos/provider/claude_code_usage_schema.hpp`
- [ ] `include/uos/provider/health.hpp`
- [ ] `include/uos/provider/lithos.hpp`
- [ ] `include/uos/provider/lithos_handlers.hpp`
- [ ] `include/uos/provider/lithos_models.hpp`
- [ ] `include/uos/provider/lithos_rate_limits.hpp`
- [ ] `include/uos/provider/lithos_streams.hpp`
- [ ] `include/uos/provider/metered.hpp`
- [ ] `include/uos/provider/openrouter.hpp`
- [ ] `include/uos/provider/openrouter_handlers.hpp`
- [ ] `include/uos/provider/openrouter_request_controls.hpp`
- [ ] `include/uos/provider/openrouter_streams.hpp`
- [ ] `include/uos/provider/openrouter_web_tools.hpp`
- [ ] `include/uos/provider/presentation.hpp`
- [ ] `include/uos/provider/removed_provider_circuit.hpp`
- [ ] `include/uos/provider/removed_provider_telemetry.hpp`
- [ ] `include/uos/provider/selection.hpp`
- [ ] `include/uos/provider/stream_relay.hpp`
- [ ] `include/uos/provider/surplus.hpp`
- [ ] `include/uos/provider/surplus_claude_chat_request.hpp`
- [ ] `include/uos/provider/surplus_claude_request.hpp`
- [ ] `src/chat/completions.cpp`
- [ ] `src/chat/envelope.cpp`
- [ ] `src/chat/stream_translation.cpp`
- [ ] `src/codex/account_routing.cpp`
- [ ] `src/codex/auth.cpp`
- [ ] `src/codex/auth_refresh.cpp`
- [ ] `src/codex/auth_repair.cpp`
- [ ] `src/codex/banked_reset.cpp`
- [ ] `src/codex/banked_reset_claim.cpp`
- [ ] `src/codex/banked_reset_pool.cpp`
- [ ] `src/codex/banked_reset_provider.cpp`
- [ ] `src/codex/banked_reset_submission.cpp`
- [ ] `src/codex/capacity_routing.cpp`
- [ ] `src/codex/dispatch.cpp`
- [ ] `src/codex/experiment.cpp`
- [ ] `src/codex/external_auth.cpp`
- [ ] `src/codex/index.cpp`
- [ ] `src/codex/manual_reset.cpp`
- [ ] `src/codex/manual_reset_state.cpp`
- [ ] `src/codex/native_auth.cpp`
- [ ] `src/codex/native_auth_transport.cpp`
- [ ] `src/codex/overage_settings.cpp`
- [ ] `src/codex/quota.cpp`
- [ ] `src/codex/rate_limit_429.cpp`
- [ ] `src/codex/reset_settings.cpp`
- [ ] `src/codex/responses_error.cpp`
- [ ] `src/codex/responses_operations.cpp`
- [ ] `src/codex/responses_runners.cpp`
- [ ] `src/codex/responses_state.cpp`
- [ ] `src/codex/routing_evaluation.cpp`
- [ ] `src/codex/routing_mutations.cpp`
- [ ] `src/codex/routing_serial.cpp`
- [ ] `src/codex/routing_state.cpp`
- [ ] `src/codex/session.cpp`
- [ ] `src/codex/supervisor.cpp`
- [ ] `src/codex/supervisor_brief.cpp`
- [ ] `src/codex/supervisor_config.cpp`
- [ ] `src/codex/supervisor_follow.cpp`
- [ ] `src/codex/supervisor_inventory.cpp`
- [ ] `src/codex/supervisor_log.cpp`
- [ ] `src/codex/supervisor_secret.cpp`
- [ ] `src/codex/supervisor_transport.cpp`
- [ ] `src/deepseek/chat_projection.cpp`
- [ ] `src/deepseek/context_admission_failure.cpp`
- [ ] `src/deepseek/flash_aliases.cpp`
- [ ] `src/deepseek/forwarded_payload_policy.cpp`
- [ ] `src/deepseek/handlers.cpp`
- [ ] `src/deepseek/index.cpp`
- [ ] `src/deepseek/responses.cpp`
- [ ] `src/deepseek/responses_payload.cpp`
- [ ] `src/deepseek/responses_stream.cpp`
- [ ] `src/deepseek/stream.cpp`
- [ ] `src/deepseek/waterfall.cpp`
- [ ] `src/deepseek/waterfall_handler.cpp`
- [ ] `src/embeddings/handlers.cpp`
- [ ] `src/embeddings/jobs.cpp`
- [ ] `src/embeddings/ledger.cpp`
- [ ] `src/embeddings/voyage.cpp`
- [ ] `src/harmony/adapter.cpp`
- [ ] `src/harmony/classifier.cpp`
- [ ] `src/harmony/conversation.cpp`
- [ ] `src/harmony/parse.cpp`
- [ ] `src/harmony/probe_scenarios.cpp`
- [ ] `src/harmony/probes.cpp`
- [ ] `src/harmony/reliability/context.cpp`
- [ ] `src/harmony/reliability/failure.cpp`
- [ ] `src/harmony/reliability/feedback.cpp`
- [ ] `src/harmony/reliability/harness.cpp`
- [ ] `src/harmony/reliability/hash.cpp`
- [ ] `src/harmony/reliability/loops.cpp`
- [ ] `src/harmony/reliability/retry.cpp`
- [ ] `src/harmony/reliability/state.cpp`
- [ ] `src/harmony/reliability/surfaces.cpp`
- [ ] `src/harmony/reliability/verify.cpp`
- [ ] `src/harmony/render.cpp`
- [ ] `src/harmony/tools/backend.cpp`
- [ ] `src/harmony/tools/fakes.cpp`
- [ ] `src/harmony/tools/result.cpp`
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
