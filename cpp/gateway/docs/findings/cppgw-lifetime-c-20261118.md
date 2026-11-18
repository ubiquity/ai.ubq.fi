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
