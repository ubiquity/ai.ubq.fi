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
- [x] `include/uos/runtime/http_tasks.hpp`
- [x] `include/uos/runtime/http_upgrade.hpp`
- [x] `include/uos/runtime/native_kv_bootstrap.hpp`
- [x] `include/uos/runtime/permissions_linux.hpp`
- [x] `include/uos/runtime/websocket.hpp`
- [x] `src/runtime/http.cpp`
- [x] `src/runtime/http_body.cpp`
- [x] `src/runtime/http_early_sse.cpp`
- [x] `src/runtime/http_tasks.cpp`
- [x] `src/runtime/http_upgrade.cpp`
- [x] `src/runtime/native_kv_bootstrap.cpp`
- [x] `src/runtime/permissions_linux.cpp`
- [x] `src/runtime/websocket.cpp`
- [x] `include/uos/rt/fs.hpp`
- [x] `include/uos/rt/kv.hpp`
- [x] `include/uos/rt/kv_js.hpp`
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

### `include/uos/runtime/http_tasks.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 8-11 | Request task receipt API | **SAFE** | Receipts and tasks are copied shared-state handles. An owning drain callback is returned from the implementation. |

### `include/uos/runtime/http_upgrade.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 31-36,44-64 | Upgrade callback interfaces | **SAFE** | Handler and upgrade Task inputs are by value, and returned function wrappers are synchronous bridges into owning free coroutines. Socket-close ownership is reviewed at the registration site. |

### `include/uos/runtime/native_kv_bootstrap.hpp`

The path and receipt queries take references for synchronous calls. The asynchronous bootstrap entry takes an owning optional string. No inline coroutine or callback registration.

### `include/uos/runtime/permissions_linux.hpp`

Synchronous permission contract and owned value types. No coroutine, deferred capture or callback registration.

### `include/uos/runtime/websocket.hpp`

Declaration of the memoized module promise. Constructor call and cache lifetime are reviewed with the implementation.

### `src/runtime/http.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 35-74,1151-1177,1273-1277 | Emitter and server listener dispatch | **SAFE** | Dispatch snapshots registrations and weak or strong target handles are acquired by the callback. A reentrant removal leaves the callable alive through the copied registration. |
| 323-453,621-695,739-773 | llhttp callbacks and parser views | **SAFE** | The parser is immovable inside ConnectionState, runs llhttp callbacks synchronously, uses offsets across reads, and copies target, headers and payload before erasing or growing input. |
| 514-559 | ConnectionState::startRead | **SAFE** | The async completion owns ConnectionState and its fixed readBuffer. Destruction closes the stream but the queued completion still owns the storage. |
| 689-733,796-807,987-1031,1133-1143,1236-1255 | Connection, socket and request links on ordinary completion | **SAFE** | Server map owns attached connections, which own socket and active HTTP objects. Normal close or ws handoff drops the return edges; finish or dump releases the current request and response. |
| 756-759,783-787,809-833 | Body event and queued resume/end | **SAFE** | Event dispatch or the queued job holds a local IncomingMessage and ConnectionState; queued work checks that the request still matches before operating. |
| 850-889,987-1000 | Outstanding HTTP write buffer | **DEFECT** ([D01](#d01)) | flush gives Asio a view into output.front() and awaits its callback. An intervening close destroys that vector with output.clear() before the completion runs, violating Asio's buffer lifetime requirement. |
| 892-920,928-962,1001-1031 | Posted drain, finish and close callbacks | **SAFE** | Each dispatch holds an owning connection or response snapshot; after reentrant finish it checks that the same response is current before resetting members. |
| 1036-1090 | Connection timeout | **SAFE** | The timer captures a weak ConnectionState, locks it before onTimer and is cleared on destruction or handoff. |
| 1114-1117,979-983 | Standalone node-http requestTimeout refusal | **UNCLEAR** | destroyAfter drops the outgoing response before close, so a listener waiting specifically for its close may retain state. serveHttp sets requestTimeout to zero. Trace any separately shipped callers that enable it and expect a delivery receipt. |
| 1180-1195,1203-1233,1325-1349 | Server request dispatch, accept and retry on ordinary shutdown | **SAFE** | Deferred jobs own Server; accept and retry stop once listening is false, and close cancels the retry and clears the listening self-reference and registrations. |
| 1203-1215,1279-1320,1935-1944,1968-1985 | ServeState ownership after bind rejection | **DEFECT** ([D02](#d02)) | A listen error in the early-return branches only rejects ready and finished. There is no close to clear the server-owned strong request/upgrade listeners, and shutdown also exits at the rejected ready await. |
| 1328,1968-1979 | Active listener ownership on external loop destruction | **UNCLEAR** | Explicit close clears the Server self-owner and ServeState listener cycles. Establish whether a supported entry point can abandon a listening HTTP server in a reusable process without running close. |
| 1397-1400,1416-1420 | Socket handoff and message receiver | **SAFE** | The queued handoff check owns ConnectionState; the upgrade path holds the socket through synchronous ws handoff. pause and resume use a raw request identity only for a synchronous comparison. |
| 1485-1502,1505-1567 | Response write and header views | **SAFE** | Header and byte inputs are copied into owned fields or output before these synchronous calls return. The later lifetime of the output front is covered by D01. |
| 1620-1643,1656-1667,1890-1893 | Request body registration and disposal | **SAFE** | The request handler owns the Request and explicitly erases its entry after delivery; body access is synchronous and the body value does not hold the Request key. |
| 1687-1706,1722-1729 | Early SSE stop listeners | **SAFE** | The listeners weakly lock Readiness, which weakly locks the response. A local or registry handle keeps Readiness alive during a callback. |
| 1707-1721,1861-1872,1888-1896 | Early SSE registry key retained by its own value | **DEFECT** ([D04](#d04)) | The static registry stores a commit callable owning its Request key. If the handler frame is abandoned before the explicit unregister after delivery, the key cannot expire and the registry retains its body and signal indefinitely. |
| 1747-1778,1813-1816 | DrainWait callback self-ownership | **DEFECT** ([D03](#d03)) | DrainWait stores four listeners that each strongly own DrainWait. A frame abandoned at the drain await can drop without any of those events firing, so the self-cycle still retains the response, connection and signal. |
| 1783-1795 | Detached body cancellation | **SAFE** | ByteStream::cancel synchronously builds an owning cancellation task and the coroutine copies Request, Response, outgoing and signal. Cancellation does not borrow the local optional stream handle. |
| 1795-1801,1807-1823 | Stream reader and abort callback on ordinary completion | **SAFE** | The reader task owns its reader slot, the named chunk survives a directly awaited drain, and normal failure or abort removes the listener. Forced-teardown retention of the abort edge is covered separately. |
| 1796-1801,1821 | Response abort listener self-ownership | **DEFECT** ([D05](#d05)) | The abort signal owns a listener that captures that same signal and a reader. If the send coroutine is destroyed during a pending read without dispatching abort, the post-await remove never runs and the listener pins both. |
| 1846-1854,1986-1990 | Active tasks and finished aggregation | **SAFE** | Tasks own their child frames; settlement listeners weakly acquire ServeState. Shutdown takes a promise snapshot and its detached continuation copies only the finished resolver. |
| 1858-1873,1888-1898 | HTTP request coroutine and normal receipt/drain | **SAFE** | The request frame owns ServeState and the handler closure until its Promise settles. It retains the request, real delivery receipt, and drain callable across their waits, with no alias into the active task map. |
| 1901-1933 | Request finish, close and error listeners | **SAFE** | Listeners own the controller, resolver and one shared settlement bit. The raw response pointer is stored on that same response and both emitting paths keep the response alive in a local owning snapshot. |
| 1935-1944,2022-2039 | Shutdown coroutine and lingering close timer | **SAFE** | Shutdown takes state and close callback by value; the timer holds the callable for each scheduled invocation and its self-reference and state reference are weak. |
| 1992-2015,2040-2062 | Listen callback, abort cleanup and public handles | **SAFE** | Listening and terminal callbacks weakly lock state. The finished reaction removes the external abort registration and clears the state-held listener; returned handles own state for their use. |

### `src/runtime/http_body.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 32-69,91-118 | Body event and abort handlers | **SAFE** | Registered listeners use weak state and lock it for dispatch. The raw self callbacks are synchronous members of that same live state and are never independently registered. |
| 132-150 | Request body source algorithms and controller | **SAFE** | Source callbacks own body state; the state drops its strong controller after synchronous start and then reaches the stream weakly. EOF, cancel and dispose remove registrations. |

### `src/runtime/http_early_sse.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 14-17 | Early SSE registration cleanup at HTTP caller | **DEFECT** ([D04](#d04)) | Registration stores an owner of its callable in the weak-map value and returns an ordinary callable for deletion. The HTTP caller gives the stored value a strong edge back to the key and drops the explicit cleanup on frame abandonment. |
| 19-33 | Synchronous early SSE lookup and commit | **SAFE** | Commit takes a shared copy of Entry before invoking a possibly reentrant callback. The query that only reads the bit uses its map pointer synchronously. |

### `src/runtime/http_tasks.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 17-24,42-45 | Request drain and settlement aggregation | **SAFE** | The synchronous wrapper copies task set and receipt into the child frame. Each iteration snapshots promise values before allSettled; no iterator or entry reference survives the wait. |
| 27-39 | Settlement removal | **SAFE** | The map lookup is copied before use. The reaction owns the promise identity token and weakly locks the TaskSet so the set and task reaction cannot own each other. |

### `src/runtime/http_upgrade.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 22-42 | Upgrade refusal serialization and close wait | **SAFE** | The frame owns socket and response. Header iteration ends before suspension; newPromise invokes its reference-capturing executor synchronously and the socket owns only a copied resolver. |
| 83-99 | Authorization handler and refusal | **SAFE** | The owning Upgrades parameter retains the handler closure. Both request and handler info are value arguments, and refusal takes a Response copy out of the parent frame's variant. |
| 96-111,176-195 | WebSocket added after shutdown snapshot | **DEFECT** ([D06](#d06)) | Authorization checks stopping before the asynchronously resumed module wait. Shutdown can snapshot sockets during that wait; the resumed authorization can then accept a socket the shutdown task never closed. |
| 112-145 | WebSocket construction, handshake callback and close registration | **SAFE** | The selected ws options complete the handoff synchronously. Headers and accept callable are owned, close removes the map edge, and WebSocketRegistry clears attached socket listeners on loop destruction. |
| 148-173 | Upgrade socket close/error listeners | **SAFE** | The upgrade frame owns state and socket. Listeners own a controller with no strong socket edge and are removed after either outcome. |
| 176-195 | WebSocket shutdown timer and aggregate ownership | **SAFE** | The loop traverses an owning socket snapshot. newPromise immediately consumes its socket reference; the timer target is weak and close owns only the timer and resolver. Sibling operations remain independently owned if all rejects. |
| 199-208 | Returned WebSocketUpgrades wrappers | **SAFE** | Both wrappers are synchronous closures that pass an owning copy of Upgrades into free coroutine frames. |

### `src/runtime/native_kv_bootstrap.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 70-89 | Receipt string views | **SAFE** | The parsed JSON is alive for the synchronous call and fields are copied into the returned binding. |
| 143-148 | Deferred native KV preparation | **SAFE** | then owns the callback, which copies repositoryRoot and does synchronous receipt verification and preparation. |

### `src/runtime/permissions_linux.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 116-160 | Seccomp filter builders and syscall view | **SAFE** | Local closures fill the stack buffer synchronously, and the syscall consumes its program pointer before the frame exits. |
| 175-228,242-262,323-374 | Native Landlock and permission callbacks | **SAFE** | Rule data, grants, path views and receipt callbacks stay within synchronous stack scopes. Native permissions stores function pointers to linked functions. |

### `src/runtime/websocket.cpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 17-25 | WebSocket module memoization | **SAFE** | The free task returns owned constructor function objects without an intermediate suspension. The static retained Promise stores the result and no caller locals. |

### `include/uos/rt/fs.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 103,204-244,262-284 | Filesystem views, FileHandle and path pack | **SAFE** | The fs/promises names wrap synchronous cores. FileHandle owns its descriptor and borrows the read buffer only for one syscall. The template view arrays are consumed by synchronous path helpers. |

### `include/uos/rt/kv.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 77-80,94-99,193-199,335-336,380-419 | KV value and key views | **SAFE** | Input views are copied into owned variants; reference and find accessors borrow the containing value. The codecs and traced users consume them synchronously or copy before suspension. |
| 659-661,680-702,805-817,824-837 | Atomic builder and forwarding receiver contract | **SAFE** | Application commits retain the named builder and parent KV owner while awaiting. The production commit functions are synchronous forwarding closures that pass an owned store into free coroutine frames. |
| 707-723 | KV iterator ownership contract | **SAFE** | Call sites retain a unique iterator across next or return. kvListToArray borrows the caller-owned iterator and its named call sites await it through completion. |
| 730-771 | Typed KV lookup coroutines | **SAFE** | Keys and options are copied into the frame. Current typed callers retain their store through the initial virtual call and await; result conversions read only the frame-local entry or vector. |
| 847,860-882,899-917 | Memory KV clock and database owners | **SAFE** | The database owns the copied clock and version callbacks or their static defaults. The handle owns its database and the database tracks native handles weakly. |

### `include/uos/rt/kv_js.hpp`

| Lines | Site | Verdict | Ownership or failure |
| --- | --- | --- | --- |
| 104-106 | Uint8Array byte view contract | **SAFE** | The span borrows its input value explicitly. Implemented use in key conversion copies the bytes immediately and replay deserialization consumes them synchronously from a named entry. |


## Ranked defects and counts

The ranking is provisional while the checklist is open. D01 is a native buffer
lifetime contract violation. D02–D05 are retention failures under the stated exit paths.

<a id="d01"></a>

### D01. HTTP write storage released before completion, P1 request-path memory safety

- A backpressured write submits an Asio buffer over the output deque's front element.
- A disconnect or explicit destroy closes the stream and clears the deque before the
  completion callback runs. Asio requires the caller to keep the bytes valid through
  completion, including cancellation. An actual post-free access depends on the I/O backend.

```diff
--- a/cpp/gateway/src/runtime/http.cpp
+++ b/cpp/gateway/src/runtime/http.cpp
@@ ConnectionState::flush, async write completion
           self->writing = false;
-          if (self->destroyed || failure == asio::error::operation_aborted) return;
+          if (self->destroyed) {
+            self->output.clear();
+            return;
+          }
+          if (failure == asio::error::operation_aborted) return;
@@ ConnectionState::destroy
-        output.clear();
+        if (!writing) output.clear();
         outputBytes = 0;
```

<a id="d02"></a>

### D02. Failed bind retains the server and full handler, P2 startup resource leak

- Invalid address/port or bind failure emits error without entering the listening state.
- The error listener rejects both promises. Shutdown cannot pass the rejected ready wait
  and the server's stored handler/upgrade callbacks still own ServeState, which owns the server.

```diff
--- a/cpp/gateway/src/runtime/http.cpp
+++ b/cpp/gateway/src/runtime/http.cpp
@@ serveHttp, server error listener
       (*live->rejectReady)(args.error);
       (*live->rejectFinished)(args.error);
+      live->server->close();
```

<a id="d03"></a>

### D03. Abandoned backpressure wait retains itself, P2 response teardown leak

- A response blocked on drain leaves four registrations owning DrainWait, which also owns
  those four callback objects.
- On abandonment of the pending coroutine, dropping the resolver reaction cannot break the
  self-cycle. If no drain/error/close/abort event fires, the connection and signal stay retained.

Give the wait coroutine ownership and make all registered callbacks weak. Its scope guard
removes the registrations on frame destruction as well as on normal return.

```diff
--- a/cpp/gateway/src/runtime/http.cpp
+++ b/cpp/gateway/src/runtime/http.cpp
@@
-    rt::Promise<void> waitForDrain(std::shared_ptr<ServerResponse> outgoing, rt::AbortSignal signal) {
-      return rt::newPromise<void>([&](const rt::Resolve<void>& resolve, const rt::Reject& reject) {
-        auto wait = std::make_shared<DrainWait>(outgoing, signal, resolve, reject);
-        wait->drain = IncomingListener([wait](const IncomingEvent&) {
-          wait->cleanup();
-          wait->resolve();
-        });
-        wait->failed = IncomingListener([wait](const IncomingEvent& event) { wait->fail(std::get<rt::Reason>(event)); });
-        wait->closed = IncomingListener([wait](const IncomingEvent&) { wait->fail(interrupted()); });
-        wait->closedOnAbort = rt::EventListener([wait] { wait->fail(interrupted()); });
-        outgoing->once("drain", wait->drain);
-        outgoing->once("error", wait->failed);
-        outgoing->once("close", wait->closed);
-        signal.addEventListener("abort", wait->closedOnAbort, {.once = true});
-        if (signal.aborted() || outgoing->destroyed()) wait->fail(interrupted());
-      });
+    rt::Task<void> waitForDrain(std::shared_ptr<ServerResponse> outgoing, rt::AbortSignal signal) {
+      const auto result = rt::withResolvers<void>();
+      auto wait = std::make_shared<DrainWait>(outgoing, signal, result.resolve, result.reject);
+      const rt::ScopeExit remove([&] { wait->cleanup(); });
+      const std::weak_ptr<DrainWait> weak = wait;
+      wait->drain = IncomingListener([weak](const IncomingEvent&) {
+        if (auto live = weak.lock()) {
+          live->cleanup();
+          live->resolve();
+        }
+      });
+      wait->failed = IncomingListener([weak](const IncomingEvent& event) {
+        if (auto live = weak.lock()) live->fail(std::get<rt::Reason>(event));
+      });
+      wait->closed = IncomingListener([weak](const IncomingEvent&) {
+        if (auto live = weak.lock()) live->fail(interrupted());
+      });
+      wait->closedOnAbort = rt::EventListener([weak] {
+        if (auto live = weak.lock()) live->fail(interrupted());
+      });
+      outgoing->once("drain", wait->drain);
+      outgoing->once("error", wait->failed);
+      outgoing->once("close", wait->closed);
+      signal.addEventListener("abort", wait->closedOnAbort, {.once = true});
+      if (signal.aborted() || outgoing->destroyed()) wait->fail(interrupted());
+      co_await result.promise.wait();
@@ sendResponse
-            const rt::Promise<void> draining = waitForDrain(outgoing, signal);
-            co_await draining.wait();
+            auto draining = waitForDrain(outgoing, signal);
+            co_await std::move(draining);
```

<a id="d04"></a>

### D04. Early SSE entry pins its weak-map key, P2 abandoned request retention

- The readiness commit lambda stores the request strongly in the static registry's value.
- If the frame disappears before the delivery wait returns, the unregister lambda is
  dropped uncalled. The stored value keeps its key alive, including the request body.

```diff
--- a/cpp/gateway/src/runtime/http.cpp
+++ b/cpp/gateway/src/runtime/http.cpp
@@ handleRequest
       std::optional<ResponseReadiness> headerReadiness;
+      const rt::ScopeExit removeReadiness([&] {
+        if (headerReadiness) headerReadiness->remove();
+      });
@@ after the real delivery receipt
-      if (headerReadiness) headerReadiness->remove();
+      if (headerReadiness) {
+        headerReadiness->remove();
+        headerReadiness.reset();
+      }
```

<a id="d05"></a>

### D05. Abandoned response read leaves an abort self-cycle, P2 stream teardown leak

- A response waits for a stream read after attaching a listener with a copy of its signal.
- Dropping the send frame before either abort dispatch or explicit listener removal
  leaves the signal owning itself and the reader.

```diff
--- a/cpp/gateway/src/runtime/http.cpp
+++ b/cpp/gateway/src/runtime/http.cpp
@@ sendResponse
-      const auto cancel = [reader, signal] {
+      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal.state();
+      const auto cancel = [reader, weakSignal] {
         // Never await a pending provider pull to handle a disconnected client.
-        rt::spawn(reader.cancel(signal.reason()), rt::ignoreRejection);
+        if (auto live = weakSignal.lock()) {
+          const rt::AbortSignal abortSignal(std::move(live));
+          rt::spawn(reader.cancel(abortSignal.reason()), rt::ignoreRejection);
+        }
       };
       const rt::EventListener cancelListener(cancel);
+      const rt::ScopeExit removeCancel([&] { signal.removeEventListener("abort", cancelListener); });
```


19/97 files reviewed. **DEFECT 7, SAFE 51, UNCLEAR 2.**
