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
- [ ] `src/admin/capacity_observation_sync.cpp`
- [ ] `src/admin/chart_source_proxy.cpp`
- [ ] `src/admin/claude_code.cpp`
- [ ] `src/admin/codex.cpp`
- [ ] `src/admin/codex_reset.cpp`
- [ ] `src/admin/defaults.cpp`
- [ ] `src/admin/error_log.cpp`
- [ ] `src/admin/index.cpp`
- [ ] `src/admin/kernel.cpp`
- [ ] `src/admin/sentinel_incident.cpp`
- [ ] `src/agent_messages.cpp`
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

<!-- SITE-LEDGER-END -->

## Out-of-area observations

Pending caller and runtime-helper tracing.

## Ranked defects and counts

3/286 files reviewed. 18 sites: 18 SAFE, 0 DEFECT, 0 UNCLEAR. <!-- AUDIT-COUNTS -->
