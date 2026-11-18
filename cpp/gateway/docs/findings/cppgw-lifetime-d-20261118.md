# Gateway coroutine and callback lifetime audit, area D

## Contract v1

Audit the C++20 sources directly under `src/`, the twelve assigned trees and the corresponding `include/uos/` headers. The baseline is commit `6d28e858e7ab8b8b1036db4a4adfdade0465ed40`, tree `6abb54014cf79ba7c64b24be4d28dde250c40ccf`, from `agent-jobs/cppgw-integrate-x1-20261118-44302040764-1`. Paths and line numbers below are relative to `cpp/gateway/`.

The owned deliverables are this report, `analysis/lifetime/d/sites.json`, and audit scripts in that directory. Source fixes, production and test edits, TypeScript, workflow changes, deployment, credential handling and live provider probes are outside this audit. Rejected and selected promises retain the existing no-sibling-cancellation semantics. Use only local or in-memory fixtures with a clean environment, empty home and fresh temp directory. Put toolchains, dependencies and outputs outside the checkout.

| Surface | Ownership traced |
| --- | --- |
| HTTP and inference handlers | Request and response frames, detached jobs, stream and delivery callbacks, and shutdown |
| Admin, auth, catalog and Sentinel | Awaited versus escaping KV or fetch work, shared caches, abort and replay callbacks |
| Authority and KV conversion | Local and RPC modes, queued work, SQLite and file handles, conversion streams |
| Models, compaction and other helpers | Owning arguments, views, iterators, generator and callback holders |

Read the coroutine rules in `PORTING.md`, the portable driver in `docs/CI.md`, and the prior kernel-failure report in `docs/findings/cppgw-clang-tidy-audit-20261117.md`. Use the five requested clang-tidy checks, lexical finders, and direct reading. A grep or tidy hit is a candidate. A site is a distinct lifetime operation or a repeated sequence with identical ownership in one function. Header declarations record the interface owners where relevant. Fixes and out-of-area findings are reported for dispatch.

Acceptance uses a file-by-file commit of the checklist, Markdown verdicts and JSON. Reconcile both ledgers, verify all source lines against the baseline, and check `git diff --check`. The changed surface is audit Markdown, JSON and scripts. Use changed-file formatting and script checks for those deliverables. Select a small sanitizer control if a confirmed defect has a cheap local witness. The CI driver and its ASan shards are the native build reference. A full application build has no bearing on an audit-only source tree identical to the baseline.

The initial unknowns are current ownership after the integration refactors, availability of the pinned dependencies on this runner, and the cost of the Clang pass. Bound setup at 20 minutes. Pilot tooling and retain per-unit logs, with at most four workers and per-unit cutoffs. Continue reading if a translation unit fails or exceeds its cutoff. Each committed file is a restart checkpoint. Stop when the inventory and ranked findings reconcile, or leave the precise pending files and measured limits in the committed report.

## File checklist

286 first-party files.

- [x] `src/admin/api_key_audit.cpp`
- [x] `src/admin/api_key_mutations.cpp`
- [x] `src/admin/api_keys.cpp`
- [x] `src/admin/capacity_observation_sync.cpp`
- [x] `src/admin/chart_source_proxy.cpp`
- [x] `src/admin/claude_code.cpp`
- [x] `src/admin/codex.cpp`
- [x] `src/admin/codex_reset.cpp`
- [x] `src/admin/defaults.cpp`
- [x] `src/admin/error_log.cpp`
- [x] `src/admin/index.cpp`
- [x] `src/admin/kernel.cpp`
- [x] `src/admin/sentinel_incident.cpp`
- [x] `src/agent_messages.cpp`
- [ ] `src/analytics.cpp`
- [ ] `src/api_key_policy.cpp`
- [ ] `src/api_keys.cpp`
- [ ] `src/auth/index.cpp`
- [ ] `src/auth/local_admin.cpp`
- [ ] `src/auth/local_development_key.cpp`
- [ ] `src/auth/passkeys.cpp`
- [ ] `src/auth/passport.cpp`
- [ ] `src/auth/passport_sessions.cpp`
- [ ] `src/auth/relay.cpp`
- [ ] `src/authority/admission_state.cpp`
- [ ] `src/authority/authority_rpc.cpp`
- [ ] `src/authority/contracts.cpp`
- [ ] `src/authority/credential_publication.cpp`
- [ ] `src/authority/delivery_outbox.cpp`
- [ ] `src/authority/durable_store.cpp`
- [ ] `src/authority/native_api_key.cpp`
- [ ] `src/authority/native_api_key_audit.cpp`
- [ ] `src/authority/native_api_key_policy.cpp`
- [ ] `src/authority/native_authority.cpp`
- [ ] `src/authority/native_authority_api.cpp`
- [ ] `src/authority/native_authority_codex.cpp`
- [ ] `src/authority/native_authority_contracts.cpp`
- [ ] `src/authority/native_authority_kernel.cpp`
- [ ] `src/authority/native_authority_paid.cpp`
- [ ] `src/authority/native_authority_principals.cpp`
- [ ] `src/authority/native_authority_rpc.cpp`
- [ ] `src/authority/native_authority_runtime.cpp`
- [ ] `src/authority/native_authority_sql.cpp`
- [ ] `src/authority/native_codex.cpp`
- [ ] `src/authority/native_codex_429.cpp`
- [ ] `src/authority/native_codex_capacity.cpp`
- [ ] `src/authority/native_codex_contracts.cpp`
- [ ] `src/authority/native_codex_evaluation.cpp`
- [ ] `src/authority/native_codex_mutations.cpp`
- [ ] `src/authority/native_contracts.cpp`
- [ ] `src/authority/native_kernel.cpp`
- [ ] `src/authority/native_kernel_contracts.cpp`
- [ ] `src/authority/native_paid.cpp`
- [ ] `src/authority/native_paid_contracts.cpp`
- [ ] `src/authority/native_paid_policy.cpp`
- [ ] `src/authority/native_principal_contracts.cpp`
- [ ] `src/authority/native_runtime_contracts.cpp`
- [ ] `src/bounded_response_body.cpp`
- [ ] `src/catalog/index.cpp`
- [ ] `src/catalog/models.cpp`
- [ ] `src/catalog/promotion.cpp`
- [ ] `src/catalog/store.cpp`
- [ ] `src/catalog/types.cpp`
- [ ] `src/config.cpp`
- [ ] `src/debug_routing.cpp`
- [ ] `src/decisions/handlers.cpp`
- [ ] `src/defaults.cpp`
- [ ] `src/handler/admission.cpp`
- [ ] `src/handler/http.cpp`
- [ ] `src/handler/index.cpp`
- [ ] `src/handler/serve_handler.cpp`
- [ ] `src/handler/terminal_log.cpp`
- [ ] `src/handler/terminal_route.cpp`
- [ ] `src/health.cpp`
- [ ] `src/http.cpp`
- [ ] `src/images.cpp`
- [ ] `src/inference_admission.cpp`
- [ ] `src/inference_deadline.cpp`
- [ ] `src/input_normalization.cpp`
- [ ] `src/jev_compaction/compaction.cpp`
- [ ] `src/jev_compaction/hybrid.cpp`
- [ ] `src/kv.cpp`
- [ ] `src/kv_conversion/cli.cpp`
- [ ] `src/kv_conversion/convert.cpp`
- [ ] `src/kv_conversion/cursors.cpp`
- [ ] `src/kv_conversion/legacy_codec.cpp`
- [ ] `src/kv_conversion/native_schema.cpp`
- [ ] `src/kv_conversion/profile_json.cpp`
- [ ] `src/kv_conversion/proof.cpp`
- [ ] `src/kv_conversion/typed_value.cpp`
- [ ] `src/lib/jev_compaction/client.cpp`
- [ ] `src/lib/jev_compaction/codex_items.cpp`
- [ ] `src/lib/jev_compaction/compact.cpp`
- [ ] `src/lib/jev_compaction/jev_codex_smoke.cpp`
- [ ] `src/lib/jev_compaction/jev_compaction_smoke.cpp`
- [ ] `src/lib/jev_compaction/request.cpp`
- [ ] `src/lib/jev_compaction/state.cpp`
- [ ] `src/lib/jev_compaction/types.cpp`
- [ ] `src/metered_quota.cpp`
- [ ] `src/models/catalog.cpp`
- [ ] `src/models/codex_models.cpp`
- [ ] `src/models/codex_models_availability.cpp`
- [ ] `src/models/codex_models_fetch.cpp`
- [ ] `src/models/codex_models_whitelist.cpp`
- [ ] `src/models/metadata.cpp`
- [ ] `src/models/openrouter_models.cpp`
- [ ] `src/openai.cpp`
- [ ] `src/openai_schema.cpp`
- [ ] `src/openai_telemetry.cpp`
- [ ] `src/optional_telemetry_queue.cpp`
- [ ] `src/quota_projection.cpp`
- [ ] `src/recent_model_context.cpp`
- [ ] `src/release.cpp`
- [ ] `src/request.cpp`
- [ ] `src/request_policy.cpp`
- [ ] `src/responses_attempts.cpp`
- [ ] `src/responses_buffered.cpp`
- [ ] `src/responses_failover_stream.cpp`
- [ ] `src/responses_handler.cpp`
- [ ] `src/responses_native_headers.cpp`
- [ ] `src/responses_request.cpp`
- [ ] `src/responses_stream.cpp`
- [ ] `src/runtime_config.cpp`
- [ ] `src/sentinel/incident_outbox.cpp`
- [ ] `src/sentinel/replay_admin.cpp`
- [ ] `src/sentinel/replay_capture.cpp`
- [ ] `src/sentinel/replay_envelope.cpp`
- [ ] `src/sentinel/replay_limits.cpp`
- [ ] `src/sentinel/replay_model.cpp`
- [ ] `src/sentinel/replay_observation.cpp`
- [ ] `src/sentinel/replay_read.cpp`
- [ ] `src/sentinel/replay_retention.cpp`
- [ ] `src/sentinel/replay_retention_bootstrap.cpp`
- [ ] `src/sentinel/replay_retention_metadata.cpp`
- [ ] `src/sentinel/replay_retention_schema.cpp`
- [ ] `src/sentinel/replay_store.cpp`
- [ ] `src/sentinel/upstream_capture.cpp`
- [ ] `src/serve.cpp`
- [ ] `src/static.cpp`
- [ ] `src/systemone/handlers.cpp`
- [ ] `src/types.cpp`
- [ ] `src/upstream_wire.cpp`
- [ ] `src/utils.cpp`
- [ ] `include/uos/admin/api_key_audit.hpp`
- [ ] `include/uos/admin/api_key_mutations.hpp`
- [ ] `include/uos/admin/api_keys.hpp`
- [ ] `include/uos/admin/capacity_observation_sync.hpp`
- [ ] `include/uos/admin/chart_source_proxy.hpp`
- [ ] `include/uos/admin/claude_code.hpp`
- [ ] `include/uos/admin/codex.hpp`
- [ ] `include/uos/admin/codex_reset.hpp`
- [ ] `include/uos/admin/defaults.hpp`
- [ ] `include/uos/admin/error_log.hpp`
- [ ] `include/uos/admin/index.hpp`
- [ ] `include/uos/admin/kernel.hpp`
- [ ] `include/uos/admin/sentinel_incident.hpp`
- [ ] `include/uos/agent_messages.hpp`
- [ ] `include/uos/analytics.hpp`
- [ ] `include/uos/api_key_policy.hpp`
- [ ] `include/uos/api_keys.hpp`
- [ ] `include/uos/auth/index.hpp`
- [ ] `include/uos/auth/local_admin.hpp`
- [ ] `include/uos/auth/local_development_key.hpp`
- [ ] `include/uos/auth/passkeys.hpp`
- [ ] `include/uos/auth/passport.hpp`
- [ ] `include/uos/auth/passport_sessions.hpp`
- [ ] `include/uos/auth/relay.hpp`
- [ ] `include/uos/authority/admission_state.hpp`
- [ ] `include/uos/authority/authority_rpc.hpp`
- [ ] `include/uos/authority/contracts.hpp`
- [ ] `include/uos/authority/credential_publication.hpp`
- [ ] `include/uos/authority/delivery_outbox.hpp`
- [ ] `include/uos/authority/durable_store.hpp`
- [ ] `include/uos/authority/native_api_key.hpp`
- [ ] `include/uos/authority/native_api_key_audit.hpp`
- [ ] `include/uos/authority/native_api_key_policy.hpp`
- [ ] `include/uos/authority/native_authority.hpp`
- [ ] `include/uos/authority/native_authority_api.hpp`
- [ ] `include/uos/authority/native_authority_codex.hpp`
- [ ] `include/uos/authority/native_authority_contracts.hpp`
- [ ] `include/uos/authority/native_authority_kernel.hpp`
- [ ] `include/uos/authority/native_authority_paid.hpp`
- [ ] `include/uos/authority/native_authority_principals.hpp`
- [ ] `include/uos/authority/native_authority_rpc.hpp`
- [ ] `include/uos/authority/native_authority_runtime.hpp`
- [ ] `include/uos/authority/native_authority_sql.hpp`
- [ ] `include/uos/authority/native_codex.hpp`
- [ ] `include/uos/authority/native_codex_429.hpp`
- [ ] `include/uos/authority/native_codex_capacity.hpp`
- [ ] `include/uos/authority/native_codex_contracts.hpp`
- [ ] `include/uos/authority/native_codex_evaluation.hpp`
- [ ] `include/uos/authority/native_codex_mutations.hpp`
- [ ] `include/uos/authority/native_contracts.hpp`
- [ ] `include/uos/authority/native_kernel.hpp`
- [ ] `include/uos/authority/native_kernel_contracts.hpp`
- [ ] `include/uos/authority/native_paid.hpp`
- [ ] `include/uos/authority/native_paid_contracts.hpp`
- [ ] `include/uos/authority/native_paid_policy.hpp`
- [ ] `include/uos/authority/native_principal_contracts.hpp`
- [ ] `include/uos/authority/native_runtime_contracts.hpp`
- [ ] `include/uos/bounded_response_body.hpp`
- [ ] `include/uos/catalog/index.hpp`
- [ ] `include/uos/catalog/models.hpp`
- [ ] `include/uos/catalog/promotion.hpp`
- [ ] `include/uos/catalog/store.hpp`
- [ ] `include/uos/catalog/types.hpp`
- [ ] `include/uos/config.hpp`
- [ ] `include/uos/debug_routing.hpp`
- [ ] `include/uos/decisions/handlers.hpp`
- [ ] `include/uos/defaults.hpp`
- [ ] `include/uos/handler/admission.hpp`
- [ ] `include/uos/handler/http.hpp`
- [ ] `include/uos/handler/index.hpp`
- [ ] `include/uos/handler/serve_handler.hpp`
- [ ] `include/uos/handler/terminal_log.hpp`
- [ ] `include/uos/handler/terminal_route.hpp`
- [ ] `include/uos/health.hpp`
- [ ] `include/uos/http.hpp`
- [ ] `include/uos/images.hpp`
- [ ] `include/uos/inference_admission.hpp`
- [ ] `include/uos/inference_deadline.hpp`
- [ ] `include/uos/input_normalization.hpp`
- [ ] `include/uos/jev_compaction/compaction.hpp`
- [ ] `include/uos/jev_compaction/hybrid.hpp`
- [ ] `include/uos/kv.hpp`
- [ ] `include/uos/kv_conversion/cli.hpp`
- [ ] `include/uos/kv_conversion/convert.hpp`
- [ ] `include/uos/kv_conversion/cursors.hpp`
- [ ] `include/uos/kv_conversion/legacy_codec.hpp`
- [ ] `include/uos/kv_conversion/native_schema.hpp`
- [ ] `include/uos/kv_conversion/profile_json.hpp`
- [ ] `include/uos/kv_conversion/proof.hpp`
- [ ] `include/uos/kv_conversion/typed_value.hpp`
- [ ] `include/uos/lib/jev_compaction/client.hpp`
- [ ] `include/uos/lib/jev_compaction/codex_items.hpp`
- [ ] `include/uos/lib/jev_compaction/compact.hpp`
- [ ] `include/uos/lib/jev_compaction/jev_codex_smoke.hpp`
- [ ] `include/uos/lib/jev_compaction/jev_compaction_smoke.hpp`
- [ ] `include/uos/lib/jev_compaction/request.hpp`
- [ ] `include/uos/lib/jev_compaction/state.hpp`
- [ ] `include/uos/lib/jev_compaction/types.hpp`
- [ ] `include/uos/metered_quota.hpp`
- [ ] `include/uos/models/catalog.hpp`
- [ ] `include/uos/models/codex_models.hpp`
- [ ] `include/uos/models/codex_models_availability.hpp`
- [ ] `include/uos/models/codex_models_fetch.hpp`
- [ ] `include/uos/models/codex_models_whitelist.hpp`
- [ ] `include/uos/models/metadata.hpp`
- [ ] `include/uos/models/openrouter_models.hpp`
- [ ] `include/uos/openai.hpp`
- [ ] `include/uos/openai_schema.hpp`
- [ ] `include/uos/openai_telemetry.hpp`
- [ ] `include/uos/optional_telemetry_queue.hpp`
- [ ] `include/uos/quota_projection.hpp`
- [ ] `include/uos/recent_model_context.hpp`
- [ ] `include/uos/release.hpp`
- [ ] `include/uos/request.hpp`
- [ ] `include/uos/request_policy.hpp`
- [ ] `include/uos/responses_attempts.hpp`
- [ ] `include/uos/responses_buffered.hpp`
- [ ] `include/uos/responses_failover_stream.hpp`
- [ ] `include/uos/responses_handler.hpp`
- [ ] `include/uos/responses_native_headers.hpp`
- [ ] `include/uos/responses_request.hpp`
- [ ] `include/uos/responses_stream.hpp`
- [ ] `include/uos/runtime_config.hpp`
- [ ] `include/uos/sentinel/incident_outbox.hpp`
- [ ] `include/uos/sentinel/replay_admin.hpp`
- [ ] `include/uos/sentinel/replay_capture.hpp`
- [ ] `include/uos/sentinel/replay_envelope.hpp`
- [ ] `include/uos/sentinel/replay_limits.hpp`
- [ ] `include/uos/sentinel/replay_model.hpp`
- [ ] `include/uos/sentinel/replay_observation.hpp`
- [ ] `include/uos/sentinel/replay_read.hpp`
- [ ] `include/uos/sentinel/replay_retention.hpp`
- [ ] `include/uos/sentinel/replay_retention_bootstrap.hpp`
- [ ] `include/uos/sentinel/replay_retention_metadata.hpp`
- [ ] `include/uos/sentinel/replay_retention_schema.hpp`
- [ ] `include/uos/sentinel/replay_store.hpp`
- [ ] `include/uos/sentinel/upstream_capture.hpp`
- [ ] `include/uos/serve.hpp`
- [ ] `include/uos/static.hpp`
- [ ] `include/uos/systemone/handlers.hpp`
- [ ] `include/uos/types.hpp`
- [ ] `include/uos/upstream_wire.hpp`
- [ ] `include/uos/utils.hpp`

## Site ledger

A checked file has a corresponding entry here and in the JSON ledger.

### `src/admin/api_key_audit.cpp`

Reviewed list traversal, atomic staging and request forwarding.

- L27, 33 **SAFE** · The list coroutine owns the store, iterator and page vector, and awaits each iterator operation while those owners are in its frame.
- L41, 53, 75, 76, 77 **SAFE** · Request and override handles enter the owning handler by value, and the synchronous list wrapper copies the input before the child runs.
- L66 **SAFE** · The audit sources are captureless synchronous calls consumed during event construction.

### `src/admin/api_key_mutations.cpp`

Reviewed update authorization, versioned writes, revocation and guarded deletion. The wrappers at the end are synchronous.

- L42, 43 **SAFE** · The record reader invokes both reference-capturing helpers synchronously before its input can go away.
- L104, 118, 133 **SAFE** · The resolver frames own KV, copied JSON and records, and the paid initializer receives an owned signal.
- L153, 159, 167, 169, 170, 182, 183 **SAFE** · Each projection or reservation step owns its fields and KV and completes before that frame leaves scope.
- L196, 205, 214, 225, 228 **SAFE** · The write frame retains original entries, optional snapshots and its atomic operation through commit and conflict handling.
- L234, 236, 240, 247, 252, 269, 274, 434, 435 **SAFE** · The wrapper eagerly copies context. The update coroutine keeps the variant containing the borrowed target alive while each Promise completes, and its children receive copies.
- L277, 279, 284, 290, 303, 315, 436, 437, 438, 439 **SAFE** · The revocation frame retains the request, context, KV, fetched records and atomic operation through commit.
- L328, 334, 341, 350, 356, 358, 365, 369 **SAFE** · The local-deletion claim and release tasks take entry, key and claim values and keep each atomic operation in its awaiting frame.
- L374, 376, 381, 385, 387, 392, 396, 400, 404, 413, 415, 440, 441 **SAFE** · The deletion frame owns both the result variant and the operation, including the blocked alternative accessed after the release wait.
- L423, 425, 428, 429 **SAFE** · The prefix vector is frame-local and immutable, and each iterator and yielded entry remains owned through its awaited deletion.

### `src/admin/api_keys.cpp`

Reviewed paid-fallback fan-out, request-owned mutation records, detached reconciliation, and immutable list snapshots.

- L61, 63, 65, 66, 213, 217 **SAFE** · Both paid ledger children take their key ID and KV owner by value. Fail-fast all can release the parent without invalidating either child.
- L91, 103, 106, 121, 124, 128, 145, 150, 293, 294 **SAFE** · The synchronous create wrapper copies the context, and the create and policy frames own their request, signal, records and atomic operation until the respective waits finish.
- L157, 163, 177, 354, 355 **SAFE** · History owns its request, KV and normalized ID through the key read and record list. Both overloads transfer ownership to the history frame.
- L173, 174 **SAFE** · The detached reconciler owns the copied optional shared KV and timestamp in its frame. The rejection handler borrows nothing.
- L297, 303, 304, 322 **SAFE** · The listing frame owns the store, iterator and record vector. The vector is sorted before usage awaits and stays immutable while element references are in use.
- L327, 330, 333, 334 **SAFE** · Each paid-field child copies its row and shared KV before it starts. The parent keeps independent output and ordering state.

### `src/admin/capacity_observation_sync.cpp`

Reviewed the capped body reader and ingest options handoff.

- L35, 40, 41 **SAFE** · The request, stream handle, reader and chunk vector are frame-local owners. Cancellation waits with the same reader and the rejection callback is captureless.
- L56, 61, 81, 87, 100, 103 **SAFE** · The plain wrapper copies KV and clock callback into the ingest frame. Storage receives a named task with copied host and moved points; the borrowed JSON field is converted to an owning string first.

### `src/admin/chart_source_proxy.cpp`

Reviewed the body cancellation, local and remote response reads, and retained capacity push.

- L55, 56, 71, 74, 78, 131, 134, 138 **SAFE** · Each fetch or cancellation child has a named response or body owner in its awaiting frame, and Fetch retains its callable through the delegated task.
- L152, 168, 169, 170 **SAFE** · The push task owns copied samples, fetcher and shared state. The normal transport's timeout aborts the exchange and the owned finally reaction clears inFlight on settlement.
- L179, 186, 191, 196, 205, 207, 212, 215, 218, 221 **SAFE** · The proxy frame owns request, pathname string, dependency callbacks and local response. Work escaping through the push is copied into its own frame.

### `src/admin/claude_code.cpp`

Reviewed credential validation and persistence, account targeting, guard and access handlers, and the compatibility quota read.

- L67, 88 **SAFE** · The validation frame owns both parsed credentials and workspace, and the passive status projection uses its snapshot only after the wait.
- L132, 186, 193 **SAFE** · JSON member pointers refer to unchanged frame-local body storage while account resolution is pending.
- L171, 174 **SAFE** · The persistence frame owns its submission, account patch and store while both writes are awaited.
- L202, 209, 212, 225, 234, 238, 242 **SAFE** · The upload frame retains request, input, options and the settled variant. The account reference after the final status wait still points into that frame-local variant.
- L250, 253, 257, 259, 267, 272, 276, 279, 287, 292, 299, 306, 310 **SAFE** · The quota, deletion and guard operations await each child with a frame-local store and account or body snapshot. The raw member used after resolution is in the unchanged local JSON.
- L316, 321, 324, 328, 335, 338, 342, 345 **SAFE** · Both access and active-account handlers retain request, store and parsed IDs until completion, including the store borrowed by the access write.

### `src/admin/codex.cpp`

Reviewed all admin Codex actions, catalog construction and refresh, provider selection, and prompt purge.

- L46, 52, 53, 54 **SAFE** · The roster owns its accounts and every hash child owns its account ID. The fail-fast combination cannot invalidate the remaining hash inputs.
- L70, 71, 72, 78, 83, 88, 93, 95 **SAFE** · The request, scenario and baseline are owned by the handler. The experiment task owns its baseline before the handler awaits it.
- L103, 105, 107, 111, 124 **SAFE** · The telemetry overload owns the adapter closure in the by-value reader parameter throughout the awaited task. The analytics handler likewise keeps its callback and options during direct delegation.
- L128, 129, 135, 153, 166, 174 **SAFE** · Native and memory KV readers acquire a store owner synchronously. The upload frame owns validation and atomic state, and keeps the database alive through the directly awaited catalog write.
- L181, 183, 188, 193, 194, 195, 197, 201, 202, 227, 228, 233, 257, 258, 263 **SAFE** · The individual paths await children with stable frame-local receiver, request or iterator owners. The entries and result snapshots are values.
- L207, 208, 209 **DEFECT** · Catalog override lifetime. A whitelist read rejects while an injected capturing catalog coroutine is pending. The join unwinds the dependency object; the pending coroutine can resume through its destroyed closure. Hold the catalog callable in rt::async before starting its Promise.
- L216, 217, 218, 219, 220, 221 **DEFECT** · Refresh override lifetime. A refresh child rejects while a sibling created by an injected capturing refresh coroutine is suspended. Handler exit destroys the callback holding its captures; the sibling can still resume. Wrap each injected callback in rt::async with copied options.
- L238, 239, 240 **DEFECT** · Provider selection override lifetime. The selection read rejects while an injected capturing catalog coroutine is pending. Handler exit destroys the callable; the pending build can resume through it. Hold the catalog callable in rt::async before starting its Promise.

### `src/admin/codex_reset.cpp`

Reviewed the manual-reset dispatch and error translation.

- L6, 9, 14 **SAFE** · The handler owns request and dependencies. The hash view is converted to an owning string before the separately constructed consuming Promise, which the handler awaits.

### `src/admin/defaults.cpp`

Reviewed defaults reads and cutover writes, and the migration admin handlers.

- L43, 118, 121, 125, 129, 130, 132, 133, 134 **SAFE** · The request and database handle remain in the awaiting frame. The migration line vector and options are local to the caller until the import task completes.
- L51, 52, 54 **SAFE** · The normal GET children own their KV store state or use an independent diagnostics task. The result tuple is named before its entries are borrowed.
- L53, 54 **DEFECT** · Defaults diagnostics override lifetime. A KV read rejects while an injected capturing diagnostics coroutine waits. Fail-fast unwinding drops the callback object and the pending child can resume through the freed closure. Wrap the injected callable in rt::async.
- L61, 70, 85, 100, 107, 113 **SAFE** · The update attempt owns the fetched tuple, cutover guard and options across awaits. The public POST retries with copied arguments and keeps each child within its own await.
- L99 **DEFECT** · GCC 12.2 coroutine-rule violation in the defaults write. Atomic commit suspends and returns a temporary, and the enclosing expression immediately accesses its member. Bind const auto result = co_await atomic->commit(), then assign committed = result.ok.

### `src/admin/error_log.cpp`

Reviewed error persistence, host log sizes, history iteration and replay retention reporting.

- L143, 172, 176, 178 **SAFE** · The public wrappers copy all input into the owning recorder before returning. The record, key, options and store remain frame-local until the write ends.
- L269, 277 **SAFE** · The immutable path vector is owned by the frame throughout the microtask turn and the current path reference remains valid.
- L299, 314, 328, 333, 335, 337 **SAFE** · The history frame retains the iterator, KV and its copied result entries; the sorting callable runs synchronously.
- L432, 435, 443, 448, 462 **SAFE** · The request and history are values and the named store owner outlives the directly awaited maintenance borrowing it.

### `src/admin/index.cpp`

Include-only translation unit. The admin public interfaces are audited in the header.

### `src/admin/kernel.cpp`

Rechecked the previously reported queue-list defect and both usage fan-outs against the integrated source, then traced the admin writes and diagnostics.

- L50, 56, 90, 92 **SAFE** · The database Promise yields an owning handle. The nullable adapter callbacks are captureless and transform outcomes by value.
- L96, 98, 105, 112, 114, 118, 123, 129, 131 **SAFE** · Public-key writes await an atomic operation held by the same coroutine and retain the request, KV and copied records.
- L79, 85, 135, 138, 143, 144, 146, 147 **SAFE** · The fixed queue workers receive copied KV, shared output and owner or record parameters. Both fail-fast joins may outlive the parent without losing child owners.
- L159, 166, 181, 187, 188, 195, 201, 202 **SAFE** · Usage workers likewise hold their output maps and input records in frame parameters. Their safety survives exceptional sibling failure.
- L172, 176, 209, 212, 215, 220, 223, 229, 235, 236, 239, 242 **SAFE** · Inventory and usage snapshots are named values. Direct delegation keeps the query strings and options in the caller or copies them into the child.
- L270, 277, 279, 284, 286, 293, 300, 308, 310, 314, 315 **SAFE** · Policy mutations use named options and await each child while the request and resolved input variants remain live.
- L324, 332, 335, 340, 342, 345, 346, 351, 368, 372, 373 **SAFE** · The projection children own their KV and configuration values, and the rejection adapters borrow nothing. References into the named result tuple remain valid across stats recording.
- L379, 380, 384, 386, 389, 390, 397, 398, 399, 403, 406, 407, 408 **SAFE** · Diagnostics use by-value account and signal state, stable local body and request owners, and runtime combined signals whose dependents are weak.
- L413, 414, 415, 416, 418, 421, 422, 426, 427, 430, 431, 432, 433, 435, 438, 439, 443, 444, 445 **SAFE** · Each row or identity Promise is built from owned copies before the join. The lookups capture by reference only during synchronous none_of calls, and PATCH views refer to the immutable local JSON.

### `src/admin/sentinel_incident.cpp`

Reviewed incident ID validation and the paged incident response.

- L121, 137, 138, 145, 146, 156, 158 **SAFE** · Query strings and the URL are frame-local, the query callbacks are inline algorithms, and the handler keeps both the KV owner and its injected reader closure through the directly awaited list.

### `src/agent_messages.cpp`

Reviewed both GitHub-authenticated handlers and their shared synchronous normalization.

- L169, 177, 183, 195, 199, 223, 224, 241 **SAFE** · The post frame owns dependency callbacks and the authentication variant that supplies the GitHub reference. The clock and ID functions are synchronous and the request and atomic inputs are values.
- L250, 256, 268, 288, 294 **SAFE** · The list frame owns authentication, request, query, store and iterator and awaits each step with the iterator still in scope.

<!-- SITE-LEDGER-END -->

## Out-of-area observations

Pending caller and runtime-helper tracing.

## Ranked defects and counts

14/286 files reviewed. 59 sites: 54 SAFE, 5 DEFECT, 0 UNCLEAR. <!-- AUDIT-COUNTS -->
