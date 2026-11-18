# Area B coroutine and callback lifetime audit

Job `cppgw-lifetime-b-20261118`, source `6d28e858e7ab8b8b1036db4a4adfdade0465ed40`.
Source ref `agent-jobs/cppgw-integrate-x1-20261118-44302040764-1`.
The local job checkout starts on `agent-job` at the same commit.

## Contract v1

- Audit the server runtime, KV/SQLite/fs/process, storage, cache, kernel and worker C++ implementation and headers.
- Review escaping coroutine closures, borrowed arguments and receivers, callback teardown and cycles,
  container aliases across suspension, and the port's GCC 12 await-expression rules.
- Trace normal and exceptional completion, early aggregation failure, detached work, stop/abort,
  and owner destruction. Provider-facing paths use local mocks only. Delivery and settlement semantics remain binding for the proposed fixes.
- The writable scope is this report and `analysis/lifetime/b/`. Proposed source changes are diffs only.
- Run grep-based finders, the five requested clang-tidy checks, and focused sanitizer confirmation where cheap.
  Tests use an empty home and fresh temporary directory in a credential-free, isolated environment.
  Time-box tool setup to 20 minutes and continue manual review if a tool cannot cover a file.
- The stop condition is a verdict for every in-scope site, one checked checklist item per file,
  matching report and JSON counts, ranked diffs, and an explicit record of remaining evidence gaps.

## Checklist

97 tracked files selected from the assigned source and header paths. Header declarations
of coroutine bodies are indexed by their definitions unless the declaration itself establishes a lifetime contract.

- [x] `include/uos/runtime/http.hpp`
- [x] `include/uos/runtime/http_body.hpp`
- [x] `include/uos/runtime/http_early_sse.hpp`
- [ ] `include/uos/runtime/http_tasks.hpp`
- [ ] `include/uos/runtime/http_upgrade.hpp`
- [ ] `include/uos/runtime/native_kv_bootstrap.hpp`
- [ ] `include/uos/runtime/permissions_linux.hpp`
- [ ] `include/uos/runtime/websocket.hpp`
- [ ] `src/runtime/http.cpp`
- [ ] `src/runtime/http_body.cpp`
- [ ] `src/runtime/http_early_sse.cpp`
- [ ] `src/runtime/http_tasks.cpp`
- [ ] `src/runtime/http_upgrade.cpp`
- [ ] `src/runtime/native_kv_bootstrap.cpp`
- [ ] `src/runtime/permissions_linux.cpp`
- [ ] `src/runtime/websocket.cpp`
- [ ] `include/uos/rt/fs.hpp`
- [ ] `include/uos/rt/kv.hpp`
- [ ] `include/uos/rt/kv_js.hpp`
- [ ] `include/uos/rt/kv_sqlite.hpp`
- [ ] `include/uos/rt/process.hpp`
- [ ] `include/uos/rt/sqlite.hpp`
- [ ] `src/rt/fs.cpp`
- [ ] `src/rt/kv.cpp`
- [ ] `src/rt/kv_internal.hpp`
- [ ] `src/rt/kv_js.cpp`
- [ ] `src/rt/kv_jsc.cpp`
- [ ] `src/rt/kv_jsc_deserialize.cpp`
- [ ] `src/rt/kv_key.cpp`
- [ ] `src/rt/kv_memory.cpp`
- [ ] `src/rt/kv_sdk.cpp`
- [ ] `src/rt/kv_sdk.hpp`
- [ ] `src/rt/kv_sqlite.cpp`
- [ ] `src/rt/kv_value.cpp`
- [ ] `src/rt/process.cpp`
- [ ] `src/rt/sqlite.cpp`
- [ ] `src/rt/sqlite_darwin.hpp`
- [ ] `include/uos/storage/key_compatibility.hpp`
- [ ] `include/uos/storage/kv.hpp`
- [ ] `include/uos/storage/native_addon.hpp`
- [ ] `include/uos/storage/native_kv.hpp`
- [ ] `include/uos/storage/sqlite_readonly.hpp`
- [ ] `include/uos/storage/value_compatibility.hpp`
- [ ] `src/storage/key_compatibility.cpp`
- [ ] `src/storage/kv.cpp`
- [ ] `src/storage/native_addon.cpp`
- [ ] `src/storage/native_kv.cpp`
- [ ] `src/storage/sqlite_readonly.cpp`
- [ ] `src/storage/value_compatibility.cpp`
- [ ] `include/uos/cache/kv_migration.hpp`
- [ ] `include/uos/cache/kv_migration_base.hpp`
- [ ] `include/uos/cache/kv_migration_paid_fallback.hpp`
- [ ] `include/uos/cache/kv_migration_projection.hpp`
- [ ] `include/uos/cache/kv_migration_usage.hpp`
- [ ] `include/uos/cache/kv_migration_validation.hpp`
- [ ] `include/uos/cache/prompt_analytics.hpp`
- [ ] `include/uos/cache/prompt_analytics_core.hpp`
- [ ] `include/uos/cache/prompt_analytics_queue.hpp`
- [ ] `include/uos/cache/prompt_analytics_read.hpp`
- [ ] `include/uos/cache/scope_experiment.hpp`
- [ ] `include/uos/cache/scope_experiment_model.hpp`
- [ ] `include/uos/cache/scope_experiment_targets.hpp`
- [ ] `include/uos/cache/scope_targets.hpp`
- [ ] `include/uos/cache/telemetry_gate.hpp`
- [ ] `src/cache/kv_migration.cpp`
- [ ] `src/cache/kv_migration_base.cpp`
- [ ] `src/cache/kv_migration_paid_fallback.cpp`
- [ ] `src/cache/kv_migration_projection.cpp`
- [ ] `src/cache/kv_migration_usage.cpp`
- [ ] `src/cache/kv_migration_validation.cpp`
- [ ] `src/cache/prompt_analytics.cpp`
- [ ] `src/cache/prompt_analytics_core.cpp`
- [ ] `src/cache/prompt_analytics_queue.cpp`
- [ ] `src/cache/prompt_analytics_read.cpp`
- [ ] `src/cache/scope_experiment.cpp`
- [ ] `src/cache/scope_experiment_model.cpp`
- [ ] `src/cache/scope_experiment_targets.cpp`
- [ ] `src/cache/scope_targets.cpp`
- [ ] `src/cache/telemetry_gate.cpp`
- [ ] `include/uos/kernel/attestation.hpp`
- [ ] `include/uos/kernel/policy_queue.hpp`
- [ ] `include/uos/kernel/quota_reservations.hpp`
- [ ] `include/uos/kernel/quota_reserve.hpp`
- [ ] `include/uos/kernel/quota_v2.hpp`
- [ ] `include/uos/kernel/quota_write.hpp`
- [ ] `include/uos/kernel/usage.hpp`
- [ ] `src/kernel/attestation.cpp`
- [ ] `src/kernel/policy_queue.cpp`
- [ ] `src/kernel/quota_reservations.cpp`
- [ ] `src/kernel/quota_reserve.cpp`
- [ ] `src/kernel/quota_v2.cpp`
- [ ] `src/kernel/quota_write.cpp`
- [ ] `src/kernel/usage.cpp`
- [ ] `include/uos/worker/entrypoint.hpp`
- [ ] `include/uos/worker/transport.hpp`
- [ ] `src/worker/entrypoint.cpp`
- [ ] `src/worker/transport.cpp`

## Tooling and verification

Initial checkpoint. Manual review and tool verification are pending.

## File verdicts
### `include/uos/runtime/http.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 57-71,94-106,138-152,198-211 | Event registration and dispatch interfaces | **SAFE** | Registration copies the listener handle and emitter dispatch snapshots its registrations. Receiver keep-alive and listeners' capture lifetimes are classified in the implementation. |
| 101-106,127-129,145-152,204-211,282-287 | Borrowed node-http member accessors | **SAFE** | In-scope callers use these member references synchronously while an owning Socket, IncomingMessage, ServerResponse or Server remains live. The WebSocket handoff moves the stream synchronously on the supplied production options. |
| 224-238,254-265 | ServerListener and RequestListener | **SAFE** | An event argument reference is only used in the synchronous wrapper, which copies the request, socket and head into the upgrade task. The server stores the callback by value. |
| 338-368 | ServeHandler, ServeHttpOptions and HttpServer | **SAFE** | Handler arguments are owning values and the handler closure resides in ServeState, kept by request frames. Returned handle closures own ServeState. Listener and server back-edges are reviewed at their registration sites. |

### `include/uos/runtime/http_body.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 27-38,47-51 | IncomingListener identity and invocation | **SAFE** | Listeners use shared callable identity and receive an event reference only for synchronous invocation. The concrete emitter's snapshot retains the active functor if a callback removes its registration. |
| 58-77 | IncomingBody handles | **SAFE** | The wrapper owns both its stream and body state. The implementation weakens the controller edge after stream construction and registers weak listener targets. |

### `include/uos/runtime/http_early_sse.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 12-24 | Early SSE registration API | **SAFE** | The callback is passed as an owning function and invoked synchronously with the headers reference. The registry and captured request lifetime are reviewed at the actual registration. |


## Ranked defects and counts

3/97 files reviewed. **DEFECT 0, SAFE 7, UNCLEAR 0.**
