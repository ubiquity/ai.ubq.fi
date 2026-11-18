# C++ gateway coroutine and callback lifetime audit, area A

## Audit contract v1

Review the runtime network layer for coroutine closure, borrowed parameter, callback
ownership, container invalidation and GCC 12 await-expression hazards. Analyze source
starting at `6d28e858e7ab8b8b1036db4a4adfdade0465ed40`, tree `6abb54014cf79ba7c64b24be4d28dde250c40ccf`,
which is the supplied integration branch tip. Paths and line numbers below are
relative to `cpp/gateway/` on that tree.

The owned artifacts are this report and `analysis/lifetime/a/`. Production code, tests,
TypeScript, workflows and other audit areas are out of scope for edits. Preserve the
runtime's fail-fast and race behavior, stream and socket cancellation, callback delivery
and network/accounting contracts. Follow callers across the boundary only to establish
ownership and record any findings there for the assigned owners.

The audit matrix covers overloaded and template entry points, native and fallback
branches, and ordinary, early-return, cancellation and teardown paths. Static review
covers all tracked files matching the area patterns and the ProcessLoop public
declarations in `process.hpp`. Dynamic checks use Linux ARM64, local-only fixtures
and a clean environment with an empty home and fresh temporary directory. No provider
connections or live deployment are in scope.

Use `rg` and the five requested clang-tidy checks to discover sites. Assign a verdict
from the actual owner/caller path, record it in both artifacts and commit each finished
file. Verify the report/JSON correspondence, formatting and `git diff --check`; run
bounded sanitizer checks via `bash cpp/gateway/tools/ci-build.sh --config asan --shard N/4`
or focused local fixtures where feasible. Toolchain availability, analyzer coverage
and the duration of sanitizer builds are initially unverified. Setup is capped at
20 minutes; source review continues if a tool cannot complete. Stop after all files,
sites and ranked proposed fixes reconcile, or report exactly what remains.

## Checklist (30 files)

- [x] `src/rt/abort.cpp`
- [x] `include/uos/rt/abort.hpp`
- [x] `src/rt/clock.cpp`
- [x] `include/uos/rt/clock.hpp`
- [x] `src/rt/async.cpp`
- [x] `include/uos/rt/async.hpp`
- [x] `src/rt/process_loop.cpp`
- [x] `include/uos/rt/process.hpp`
- [x] `src/rt/stream.cpp`
- [x] `include/uos/rt/stream.hpp`
- [x] `src/rt/http.cpp`
- [x] `include/uos/rt/http.hpp`
- [x] `include/uos/rt/fetch.hpp`
- [x] `include/uos/rt/fetch_transport.hpp`
- [x] `include/uos/rt/gzip.hpp`
- [x] `include/uos/rt/websocket.hpp`
- [x] `src/rt/fetch.cpp`
- [x] `src/rt/fetch_transport.cpp`
- [x] `src/rt/fetch_transport_codec.cpp`
- [ ] `src/rt/fetch_transport_http.cpp`
- [ ] `src/rt/fetch_transport_internal.hpp`
- [ ] `src/rt/fetch_transport_net.cpp`
- [ ] `src/rt/gzip.cpp`
- [ ] `src/rt/tls.cpp`
- [ ] `src/rt/tls_internal.hpp`
- [ ] `src/rt/websocket.cpp`
- [ ] `src/rt/websocket_client.cpp`
- [ ] `src/rt/websocket_frame.cpp`
- [ ] `src/rt/websocket_internal.hpp`
- [ ] `src/rt/websocket_socket.cpp`


## Tool receipts

Pending dependency/tool checks and finder runs.

## Site verdicts

### `src/rt/abort.cpp`

Abort dispatch is synchronous, so callback reentrancy and ownership during dispatch are the relevant suspension-equivalent boundaries.

- **SAFE** `30, 31, 34, 42, 46, 49, 51, 93, 94, 195, 205, 211, 220` (c/d), Dependent signal registration and pinning. The source temporarily owns interested dependents, the reverse links are weak, and abort moves pins and promoted dependents outside the registry before dispatch.
- **SAFE** `55, 63, 64, 66, 67, 69, 74, 131, 139, 143, 147` (c/d), Listener snapshots and removal traversal. Dispatch owns a snapshot of unique entries plus the executing callback, and live-vector removal exits its traversal immediately.
- **DEFECT (A-ABORT-OWNER)** `62, 69, 74, 81, 95, 228` (c/d), Abort dispatch borrows the controller's state slot. A synchronous listener may release or replace the controller that owns the shared pointer still used by the dispatch epilogue.
- **SAFE** `154, 156, 158, 163, 166, 171, 178` (c), onabort setter and getter in current dispatch paths. The handler slot and listener entry own callback copies and dispatch pins the active callable while assignment or removal changes the slot.
- **SAFE** `187, 189, 190, 191` (c), AbortSignal timeout callback. The unreferenced timer owns the signal by value, the signal has no timer backedge, and firing or loop teardown releases the callback.
- **SAFE** `231, 232, 234, 237, 239, 240, 241` (c), Native cancellation bridge. The completion handler owns the cancellation slot and signal while the listener uses a weak signal link, and completion removes any surviving registration.
- **SAFE** `247, 250, 252, 253, 257, 258, 259, 263, 264` (c), Abortable sleep timer and listener. The timer and listener intentionally retain each other while armed, and abort, timeout, or loop teardown clears an owning callback before releasing the active invocation.

### `include/uos/rt/abort.hpp`

The public header declares the abort bridge and listener handles. The implementation's reentrant dispatch defect is counted at its source.

- **SAFE** `34, 39, 40, 45, 65, 72, 73, 76, 77` (c), Listener identity and invocation. Each identity shares an owning function and the runtime dispatch copies the listener before invoking it, allowing deregistration during the call.
- **SAFE** `59, 62, 88, 91, 94, 102, 105` (b/c), Signal owner and borrowed accessors in current callers. The handle owns its state, callers copy or consume reason and state synchronously, and the separate abort-dispatch borrow is tracked in abort.cpp.
- **SAFE** `115, 125, 126, 127, 128, 129` (a/b/c), Eager abortable wrapper. This ordinary function moves its Task into runInto and returns a Task owning the result state, while its fetch caller gives the native child owned request inputs.

### `src/rt/clock.cpp`

Timer APIs are synchronous. The one Task-returning overload creates an owning result before handing it to the lazy awaiter.

- **SAFE** `13, 14, 21, 22, 78, 81, 85, 87, 91, 93, 99, 104` (c/d), Timer queue ownership and disarm. The queue owns each entry and the timer has a weak queue backedge; handles retain their timer during synchronous dequeue and callback release.
- **UNCLEAR** `129, 134, 135, 176, 181, 185, 186, 187, 191` (c), Self-replacement of injected global hooks. Current production uses defaults and the scoped fixture overrides checked retain their captures. Supporting an override that replaces its own slot while it runs would require proving an independent callable owner.
- **SAFE** `141, 145, 151, 155, 163, 165, 167, 168` (b/c), Timeout handle operations. Ref, unref and refresh are synchronous, lock the weak queue locally, and do not publish a reference to the handle.
- **SAFE** `196, 197, 198, 199, 205, 209, 210, 220, 224, 225` (b/c), Sleep callback and timer construction. The callback and awaiter independently own Promise state, and each newly queued timer owns its moved callback.
- **SAFE** `234, 236, 242, 244, 246, 247, 251, 254, 260, 264, 265, 268` (c/d), Timer dispatch and callback rearm. The dispatcher pins the timer, erases the map entry and moves out the callable before invoking code that may clear or rearm it.
- **SAFE** `272, 273, 276, 277, 280, 281` (c/d), Timer teardown. Teardown owns a detached map and releases moved callbacks after marking the corresponding timers disarmed.

### `include/uos/rt/clock.hpp`

The injected Clock methods are synchronous. Loop's borrowed Clock dependency is assessed in async.cpp and async.hpp.

- **SAFE** `75, 78, 94, 96, 99` (b/c), Timeout timer ownership. Timeout is a shared handle and the implementation consumes its state accessor within synchronous timer operations.
- **SAFE** `102, 108, 111, 115, 117, 127, 128, 129, 130, 134, 141, 145, 151, 153` (b/c), Timer registration and scheduling API. Registration accepts an owning callable by value, clearing borrows only synchronously, and sleep creates and owns its result independently of the caller.

### `src/rt/async.cpp`

Read the complete loop implementation and the PromiseCore callbacks. The process loop is intentionally permanent. Scoped loops have an explicit teardown path.

- **SAFE** `147, 148, 149, 156, 163, 379, 380, 385` (b/c), Loop owns the context and timer queue and borrows its Clock. A scoped Loop's clock is declared before it in the runtime fixture, and the permanent default Loop uses the static SystemClock.
- **SAFE** `178, 184, 186, 190, 191, 193, 194, 198, 199, 203, 209, 383, 384` (b/c), Asio executor's Loop implementation pointer. The Loop contract confines I/O and executor users to the owner lifetime, and execute owns dispatched handlers or discards them once the context is gone.
- **SAFE** `215, 216, 284, 288, 289, 290, 291, 293, 298, 299` (c/d), Microtask callback dispatch. Each one-shot job is moved out before it can reenter the loop, and rejection candidates live in a detached vector while they are inspected.
- **SAFE** `219, 227, 238, 242, 243, 244, 245, 246, 408, 413, 422` (c/d), Intrusive Promise waiting-list mutation. Promise destruction unlinks synchronously, and teardown unlinks and pins each node before destroying its reactions and any continuations they own.
- **DEFECT (A-LOOP-TEARDOWN-OWNER)** `252, 257, 258, 261, 263, 266, 268, 367, 368, 396, 397, 400, 401, 443, 445` (c), Scoped Loop teardown runs callbacks under the ambient loop. A destroyed frame can queue a microtask from a ScopeExit while the dying loop is absent from currentLoop, letting that job survive in an unrelated loop.
- **SAFE** `313, 316, 319, 322, 325, 332, 338, 349, 355, 364, 367, 372, 376, 454` (b/c), Run-loop stop predicate and scope guards. The synchronous caller holds the stop Promise through runLoop, and the local guards restore the running flag and scope before returning.
- **SAFE** `410, 414, 416, 420, 424, 425, 427, 431` (c/d), Promise settlement traversal. Reactions move into the microtask queue without invocation during traversal, and unhandled rejections keep a separate shared owner until the checkpoint.
- **SAFE** `437, 438, 439, 443, 481, 482` (a/b/c), Native start and user microtask registration. Asio owns the moved task and a captureless completion handler, and the queue owns the moved user callable.
- **UNCLEAR** `449, 450, 451` (b/c/e), Parked awaiter and pending Promise ownership. The waiter job owns the coroutine that owns the Promise; settlement and loop teardown break the cycle. Identifying indefinitely abandoned request waiters would establish where this topology retains request state on the permanent loop.
- **SAFE** `456, 457, 458, 462, 465, 467, 470, 472, 476, 477, 478` (a/b/c/e), Waker, abandon and microtask initiations. Each initiation is named and synchronous, and the handler goes to a weakly linked slot, to the loop, or is intentionally discarded.
- **DEFECT (A-PROMISE-REACTION-CYCLE)** `485, 490, 491, 492, 494, 497` (c/d), Pending Promise reactions self-own their source. A dropped never-settled input keeps its reaction and combinator output alive until Loop destruction, including losers of a completed combination on the permanent loop.

### `include/uos/rt/async.hpp`

Reviewed the lazy Task adapters, eager Promise combinators, callback return-type branches and generator implementation. The Promise callback defect is reachable through the public API. The production continuations checked consume by-reference inputs synchronously or use owned values.

- **SAFE** `109, 114, 116, 117, 118, 126, 128, 129, 130, 139, 143, 152, 153, 154, 155, 164` (c/d), One-shot jobs and waiting-list declarations. Job moves its erased callable into a local before invocation, and the intrusive list is maintained and unlinked synchronously by the loop implementation.
- **SAFE** `204, 207, 210, 211, 214, 218, 220, 221, 226, 228, 229, 319, 321, 338, 340, 345, 348, 352, 368, 369, 371, 374, 376, 377` (a/b/c), Owned awaiters and public Promise accessors. The wrappers synchronously copy the state or move the task into an owning frame, while value, reason and state accessors borrow from a live Promise in the checked callers.
- **DEFECT (A-PROMISE-GCC-AWAIT)** `219, 375` (e), Implicit shared_ptr conversion in Promise await expressions. Both awaiters form a shared_ptr<PromiseCore> temporary from a typed shared_ptr in the await full expression.
- **SAFE** `233, 235, 240, 245, 248, 249, 273, 274, 277, 279, 280, 285, 579, 580, 582, 583` (b/c), Loop, Clock, Scope and blockOn borrowed API. The synchronous blockOn owns its stop Promise and the current-loop scope is nested inside the named Loop and Clock lifetimes in the checked callers.
- **SAFE** `386, 394, 395, 396, 397, 405, 407, 415, 418, 428, 429, 445, 447, 452, 453, 455, 462, 464, 466` (a/b/c), Synchronous resolver and detached-task entry points. Resolvers share their state, newPromise calls its executor synchronously, and spawn hands the Task to runInto; a Task's own borrowed inputs remain governed by its caller.
- **SAFE** `471, 472, 473, 475, 486, 487, 497, 499, 559, 561, 564, 573, 679, 682, 684, 914, 919, 921, 927` (a/b/c/e), Coroutine holders and finally helpers. callHeld, keepAlive and withFinally keep the callback or explicit owner in an awaited frame; ScopeExit owns its synchronous cleanup and the helpers use named results.
- **DEFECT (A-PROMISE-CALLBACK-INPUT)** `692, 701, 704, 713, 715, 726, 731, 744, 745, 746, 758, 760` (a/b/c), Task-returning Promise callbacks can borrow a destroyed outcome. The Task-keeping branch pins the callable, but a coroutine parameter referencing the fulfilled value or rejection reason still borrows the source state held only by the reaction.
- **DEFECT (A-PROMISE-REACTION-CYCLE)** `397, 726, 744, 758, 780, 797, 815, 840, 860, 873, 889, 892` (c), Typed Promise reactions retain pending sources and input tuples. These registrations repeat the self-owning reaction pattern. Apply the source ownership handoff in the ranked fix and collect fulfilled tuple slots without retaining the tuple of input Promises.
- **SAFE** `773, 781, 783, 788, 790, 792, 797, 798, 799` (a/b), finally_ callable and settled source access. The synchronous branch reads the source inside its reaction, and the Task branch retains the callable through completion and the source through the after reaction.
- **SAFE** `519, 520, 521, 522, 529, 530, 537, 541, 542, 545, 549, 810, 820, 823, 824, 835, 841, 849, 869, 878, 890, 898, 902, 909, 910` (a/b/d), Combinator storage and traversal across sibling completion. Registered reactions are synchronous and own their result storage and copied indices, so sibling completion cannot invalidate a borrowed iterator or closure.
- **SAFE** `633, 634, 636, 941, 946, 947, 948, 951, 953, 956, 960, 961, 963, 967, 976, 978, 983, 989, 990, 998, 1001, 1005` (c/d), Generator queue dispatch. A request is moved out before delivery, the launch callable is moved out before execution, and queued work promotes a weak core only for synchronous mutation.
- **SAFE** `663, 664, 666, 671, 672, 675, 677, 1012, 1013, 1016, 1017, 1022, 1026, 1027, 1029, 1035` (b/c), Generator yield and weak waker parking. The Yield task owns its value and holds a weak backedge while parked, and Waker moves the current waiter out before resuming it.
- **SAFE** `1041, 1043, 1046, 1049, 1051, 1058, 1086, 1088, 1091, 1093, 1094, 1095` (a/b/c), Generator body and yielding adapters. The body closure and return slot move into runGenerator's frame, and the public Yield wrappers take a value or copy the weak handle before returning a Task.
- **DEFECT (A-GENERATOR-STEP-OWNER)** `1041, 1058, 1061, 1062, 1063, 1064, 1099, 1101` (b/c), Pending generator step does not keep its generator alive. A returned next or return Task owns only the result, so dropping the last generator handle while the body is executing can strand an outstanding step.
- **SAFE** `1067, 1069, 1073, 1078, 1082, 1114, 1118, 1123, 1124, 1130, 1136` (a/b/d), Delegate and forAwait ownership. The frames own both generator handles or callback and the complete current IteratorResult through the body await and return cleanup.
- **DEFECT (A-GENERATOR-GCC-AWAIT)** `1073, 1123, 1124` (e), Forwarded generator values constructed within await expressions. delegateTo moves a yielded T into a by-value adapter and forAwait may move it into a by-value coroutine callback in the await full expression.

### `src/rt/process_loop.cpp`

Reviewed both pidfd and SIGCHLD branches, subprocess pipe streams, process listeners and BunFile stream adapters. The production server registers process signals on the process loop.

- **SAFE** `45, 51, 53, 73, 76, 79, 84, 87` (b/c/e), POSIX child exit wait and signal names. The watcher owns its signal_set and PID throughout the wait, the error token is named, and Darwin's string views point into static storage.
- **DEFECT (A-PROCESS-SIGNAL-CONTEXT)** `100, 102, 107, 108, 119, 124, 125, 134, 136, 142, 148` (b/c), Static process signal registry retains a scoped Asio context. A listener registered on a scoped Loop can leave a static signal_set whose service and executor have already died.
- **SAFE** `123, 124, 126, 129, 130, 150, 154, 156, 158, 159, 161, 165, 168, 174, 263, 264, 266, 271, 272, 274, 279, 281` (c/d), Signal wait generation and synchronous listener dispatch. The wait callback captures a generation by value; listener delivery snapshots owning identities and erases once entries before invoking any callback.
- **SAFE** `231, 232, 234, 235, 236, 239, 240, 244, 248, 251, 252, 254, 255, 257, 259` (a/b/c/e), Subprocess pipe read and cancellation. The source closures own the descriptor, the forwarding pull moves an owned controller into the child frame, and the frame owns its I/O buffer and token through completion.
- **SAFE** `287, 288, 295, 296, 299, 300, 305, 306, 307, 308, 310, 311, 312, 313, 318, 319, 320, 323, 327, 328` (b/d/e), FileSink input views and suspended flush. write synchronously copies incoming views, and each flush owns the stable descriptor via its state and the moved chunk independently of later changes to pending.
- **SAFE** `330, 333, 344, 345, 346, 362, 363, 366, 367, 369, 370, 385, 386, 391, 395, 400, 405, 410, 415, 417, 430` (b/c/e), Child-exit observers and public subprocess accessors. The watchers own state and native descriptors across waits, while accessors synchronously copy owning handles and kill consumes its view before returning.
- **SAFE** `433, 437, 446, 448, 451, 459, 465, 474, 513, 524, 527, 528, 531, 533, 534, 536, 538, 540, 543, 545` (a/b/c/d), Spawn setup and watcher detachment. The borrowing lambdas and argv or env pointers are synchronous, and the detached watchers receive state and their native wait inputs by value.
- **SAFE** `551, 555, 560, 561, 570, 581, 591, 600, 602, 604, 605, 606, 608` (b/c), BunFile stream callbacks and descriptor lifetime. The source owns its pathname and descriptor, the synchronous pull and cancel closures own that source, and close clears the fd before teardown.

### `include/uos/rt/process.hpp`

The ProcessLoop public interface is split from its implementation in process_loop.cpp and the loop-free functions in process.cpp.

- **SAFE** `80, 84, 90, 92, 93, 95, 100, 103, 106, 110, 111, 112, 113, 117, 118` (b/c), Signal Listener closure and signal-name views. Registration copies the owning Listener identity, delivery invokes an owned snapshot, and the event/name views are used synchronously or point to static signal-name strings.
- **SAFE** `137, 141, 143, 144, 146, 148, 151, 155, 158, 162, 164, 165, 168, 178, 181, 190` (b/c), FileSink and Subprocess handle APIs. Writes and kill consume their views synchronously, and deferred work gets shared state from the handle or returns an owning stream or Promise copy.
- **SAFE** `212, 214, 216, 220, 223, 225` (b/c), BunFile path and stream declaration. The object owns its path and stream copies that path into the source before returning, allowing the BunFile wrapper to be a temporary.

### `src/rt/stream.cpp`

This translation unit contains synchronous reason/value construction and explicit ByteChunk template instantiations. The instantiated suspension sites are recorded in stream.hpp.

- **SAFE** `7, 10, 11, 13, 16, 19` (b), Tee cancellation reason construction. The helpers consume the borrowed exception handles synchronously and the resulting exception owns both reasons and its JSON representation.

### `include/uos/rt/stream.hpp`

Reviewed eager reader registration, source callback type erasure, reentrant cancellation, branch teeing and generator adapters. A copied controller is a strong stream owner. A copy kept in closure-owned state therefore needs an independent release path.

- **SAFE** `38, 46, 48, 49, 55, 56, 58, 60, 72, 73, 74, 136, 251, 294, 295, 296, 297, 373, 378` (a/b/c), Source algorithm callable lifetime. The start source is owned by the constructor, runtime pull/cancel calls use local algorithm copies and runAsync retains the erased coroutine closure throughout each asynchronous call.
- **UNCLEAR** `49, 72, 74` (b), Borrowing callback parameters through the by-value erased signature. The current callbacks checked take owned values or complete synchronously. A supported ABI check with a Task callback taking const A& would establish whether the materialized std::function parameter survives throughout its await.
- **DEFECT (A-STREAM-GCC-AWAIT)** `72, 540` (e), Stream callback and yield arguments materialized inside awaits. runAsync forwards an owning erased argument and iterateStream moves an owning chunk within the await full expressions.
- **SAFE** `103, 104, 111, 124, 128, 135, 136, 137, 140, 146, 305, 307, 311, 316, 321, 324, 376, 377` (c), Controller and active pull ownership. The executing controller and pull reaction pin state during use; a callback-owned controller cache can create a separate persistent backedge, assessed at each such source.
- **DEFECT (A-PROMISE-REACTION-CYCLE)** `137, 138, 252, 254, 378, 380, 570, 571` (c), Stream reactions capture their own pending Promise states. The pull, cancel, start and tee closed callbacks repeat the pending-source cycle from PromiseCore. Their source states need ownership only during delivery.
- **SAFE** `151, 153, 154, 155, 157, 162, 165, 171, 173, 174, 182, 183, 186, 190, 191, 269, 270, 283, 284, 286` (c/d), Stream and reader queue drain boundaries. Chunk and reader-request entries are moved out before settlement, reader release pins the stream, and Promise settlement queues reactions outside these synchronous traversals.
- **SAFE** `203, 204, 215, 216, 218, 221, 223, 225, 226, 231, 233, 234, 334, 346, 348, 359, 361, 408, 419, 425, 426, 428, 433` (b/c/d), Read registration and reader accessors. Read registration happens synchronously and returns an owned result state, and no borrow of the reader's stream is used after a pull callback can reenter.
- **DEFECT (A-STREAM-CANCEL-OWNER)** `238, 249, 251, 252, 253, 262, 264, 336, 338, 352, 355, 396, 398, 401, 404` (c/d), Stream cancel borrows the owner slot through callback invocation. The callback can release the reader or replace the stream handle while the cancellation epilogue still dereferences its shared_ptr slot.
- **DEFECT (A-STREAM-TEE-READ-CYCLE)** `444, 445, 446, 453, 454, 458, 460, 464, 469, 475, 479, 485, 486, 493, 496, 502, 554, 557, 558, 559, 561, 562, 567, 568` (b/c), In-flight tee read keeps abandoned branches' source alive. Weak branch references avoid one direct backedge, while an unfulfilled source read still owns the teeRead continuation that owns the source through TeeState.
- **SAFE** `505, 507, 510, 513, 514, 515, 517` (b/c), Tee cancellation releases its tee owner before waiting. The cancel coroutine transfers to the shared completion state once branch bookkeeping is done, and adoption owns the source cancellation operation.
- **DEFECT (A-PROMISE-GCC-AWAIT)** `516` (e), Tee cancellation converts the shared Promise state inside the await. The typed shared_ptr is converted to a base shared_ptr in the co_await full expression. Use the same named Task as for awaitOwned.
- **SAFE** `461, 472, 473, 488, 489, 491, 520, 522, 525, 529, 531, 535, 540, 545, 546, 547, 581, 582, 583` (a/b/d), Tee delivery and stream iterator step storage. The current step belongs to the coroutine, promoted branch handles are local, and the iteration frame owns its reader while it awaits the stored read or cancel.

### `src/rt/http.cpp`

Read the body readers and stream adapters, clone/transfer paths, synchronous FormData parsing and static MIME data. This file models Fetch values. The server transport lives outside Area A.

- **SAFE** `101, 106, 108, 109, 110, 113, 114, 115, 989, 990, 991, 1006, 1008, 1010, 1012` (b/c), Blob data and temporary Blob handles. The ordinary member wrappers copy the shared immutable bytes into the coroutine or stream before the receiver can die.
- **SAFE** `145, 146, 148, 150, 155, 156, 158, 160, 747, 752, 754, 757` (c/d), Buffered body streams and their source callbacks. The source owns bytes and offset without a stream backedge, and the copied pull algorithm owns both for synchronous iteration and close.
- **SAFE** `203, 205, 217, 555, 558, 578, 585, 601, 607, 617, 652, 661, 670, 683, 685, 1034, 1054, 1059, 1065` (b/d), FormData parse, serialize and entry mutation views. All parser views and entry iterators are consumed synchronously under the owned body or FormData and the products own their bytes and strings.
- **SAFE** `696, 698, 699, 700, 704, 708, 713, 718, 724, 730, 733, 894, 912, 913, 918, 925, 931, 1097, 1147, 1157, 1166, 1173, 1188, 1195, 1204, 1217, 1240, 1247, 1256` (b/c), Body extraction, clone, header and transfer wrappers. The reference captures are invoked immediately by visit and body cloning transfers owning stream handles synchronously. Tee retention is tracked at its implementation.
- **SAFE** `777, 789, 790, 797, 803, 806, 809, 810, 812, 813, 815, 817, 833, 834, 835, 839, 841, 847, 849, 857, 859, 862, 865, 867, 869, 872, 878, 879, 883, 1141, 1145, 1234, 1238` (b/c/d/e), Request and Response read tasks and their inputs. The wrappers snapshot owned data or a reader before returning, and each frame owns the read operation, complete step, accumulated bytes and form boundary during later awaits.
- **UNCLEAR** `771, 797, 803, 809, 812, 813, 817, 1141, 1234` (b/c), Abandoned streamed-body reader lock. The eager wrapper locks the body before the lazy Task starts and release is reached on success. Decide the required lock and cancellation behavior when a caller discards an unstarted Task or a stream read rejects, then exercise that behavior with an independently retained body handle.

### `include/uos/rt/http.hpp`

Reviewed the Blob, File, FormData, Request and Response API. The public Task methods are ordinary entry points that acquire ownership before the corresponding implementation coroutine starts.

- **SAFE** `48, 51, 52, 62, 66, 68, 69, 70, 72, 75, 76, 80, 81, 91, 95, 101` (b), Blob and File construction, getters and asynchronous reads. Value construction consumes views synchronously, borrowed getters refer to the owning handle, and asynchronous reads copy the backing shared_ptr at call time.
- **SAFE** `118, 120, 121, 122, 124, 125, 126, 129, 131, 132, 135, 136, 137, 138, 139, 142, 143` (b/d), FormData borrowed entries and iterators. Mutators and accessors execute synchronously. The reader and serializer paths finish entry traversal before returning or suspending.
- **SAFE** `177, 180, 184, 189, 192, 193, 201, 205, 209, 210, 211, 216, 219, 224, 229, 230, 231, 235, 236` (b/c), Response handle and body methods. Response owns shared state, reference getters are synchronous in the checked callers, and the body adapters return tasks with independently owned inputs.
- **SAFE** `249, 261, 262, 263, 265, 268, 269, 279, 287, 288, 290, 292, 293, 294, 296, 298, 299, 300, 301, 302, 303, 307, 308, 312, 313, 314, 323` (b/c), Request construction, body APIs and transfer. The constructors consume URL and init borrows synchronously, property access borrows a live handle, and each deferred body reader takes owned state from the ordinary wrapper.

### `include/uos/rt/fetch.hpp`

The statically erased Fetch and MockTransport callback signatures are the ownership boundary. Current callers mostly use by-value callback inputs. The generic Fetch constructor also accepts reference-taking coroutine callables.

- **SAFE** `27, 29, 38, 39, 40, 41, 43, 44, 45, 48, 49, 50, 54, 56, 58, 64, 65, 70, 89, 90, 100` (a/b/c), Ordinary Fetch wrapping and scoped global overrides. The synchronous Response wrapper captures its callable by shared owner and returns a value-owning Task, and an override owns its copied predecessor.
- **DEFECT (A-FETCH-CALLBACK-INPUT)** `20, 27, 28, 29, 33, 38, 40, 41, 49` (a/b), Fetch erasure can discard input storage before a borrowing coroutine starts. The invocability constraint accepts reference-taking callables, but the by-value erased invocation can destroy its RequestInfo and RequestInit before their borrowed uses.
- **SAFE** `81, 83, 84, 102, 105, 106, 107, 109, 112, 113` (b/d), Transport input and recorded request views. The virtual request parameter owns a handle and the recorded vector view is consumed while the MockTransport is held. The lifetime of the virtual receiver and callback is assessed at their implementations.

### `include/uos/rt/fetch_transport.hpp`

Reviewed the options and transport interface. The implementation copies the shared client state at the public send boundary, and the scoped-loop disposal path is covered at Pool.

- **SAFE** `28, 31, 34, 36, 40, 43, 46, 58, 59, 60, 64, 66, 68, 71, 76, 77` (b/c), Client options, send wrapper and installation. Options and requests cross entry points by value, deferred send takes a shared ClientState, and synchronous pool access and global installation use owning transport handles.

### `include/uos/rt/gzip.hpp`

The incremental codec interface exposes synchronous borrows with explicit validity windows. The coroutine wrappers take Bytes by value.

- **SAFE** `100, 101, 102, 103, 106, 108, 109, 110, 111, 113, 115, 121` (b/d), Inflater input and output spans in gateway callers. The fetch decoder drains and copies each inflater piece synchronously while the calling buffer and unique codec state remain alive.
- **SAFE** `129, 135, 138, 146, 149, 153` (a/b/c), One-shot, callback and coroutine codec APIs. The piece callback runs synchronously before the borrowed span changes and both Task APIs own their bytes across the microtask.

### `include/uos/rt/websocket.hpp`

Reviewed the listener and callback shapes, client and server ownership contracts and the upgrade handoff. Event views and target pointers are call-scoped in the checked production handlers.

- **SAFE** `57, 63, 69, 87, 93, 94, 95, 171, 173, 175, 179, 181, 187, 189, 194, 352, 359, 362` (b/d), Incoming values and event borrows in the checked callers. The native dispatch owns the active event, gateway listeners consume its raw pointers synchronously, and scheduled work copies payload or response data first.
- **SAFE** `105, 107, 108, 110, 111, 113, 115, 118, 132, 135, 138, 139, 141, 142, 144, 149, 150, 153, 160, 162, 166` (c/d), Listener identity and declared EventEmitter storage. The emitter owns the registered functions and dispatch uses owning snapshot entries. This pins callable identity through reentrant listener removal.
- **SAFE** `239, 243, 256, 259, 264, 265, 285, 286, 287, 288, 296, 302, 305, 311, 317, 318, 319, 320, 321, 322, 323, 324, 337, 347` (b/c), Client construction, send callbacks and event attributes. Asynchronous sends transfer owning payload and callable values, and attribute wrappers belong to the socket's registry. Receiver ownership during delivery is assessed in the implementation.
- **UNCLEAR** `326, 330, 331, 332, 333` (c), Self-replacement of injected buffered-amount getter. The production getter is fixed. Supporting a configured getter that overwrites its own callable requires testing that invocation with an independently pinned callable.
- **SAFE** `368, 372, 399, 403, 407, 409, 414, 415, 427, 429, 435, 437, 439, 442, 444, 447` (b/c/d), Upgrade API borrows and cross-loop ownership. Protocol and upgrade callbacks run synchronously against the caller's request, the socket is moved into native state, and client list access returns an owning vector snapshot.

### `src/rt/fetch.cpp`

Reviewed both request-init paths, both abort windows, the transport seam and the replaceable global Fetch slot. The installed HTTP client has an owning send wrapper. The polymorphic transport API also permits coroutine member implementations.

- **SAFE** `23, 28, 35, 43, 44, 49, 50, 55, 59, 61, 64, 65, 155, 160, 161, 164, 165, 168, 170, 187, 199, 200` (b/d), Request preparation and forwarding into owned tasks. The validation wrappers are synchronous, variant references end before dispatch, and rejected or sending tasks take owned reasons or Request handles.
- **SAFE** `77, 80, 84, 87, 88, 90, 91, 94, 95, 97, 100, 103, 104, 105, 106, 113, 115, 116, 118, 119, 121, 125, 126, 140, 145, 146` (a/b/c/d), Response abort and cancel delivery while active. The callback promotes a weak body for the whole invocation, pull and cancel frames own their parameters, and detached reader cancellation has an owned result independent of its caller.
- **DEFECT (A-FETCH-ABORT-BODY-CYCLE)** `70, 72, 73, 74, 77, 112, 114, 115, 120, 123, 124, 125, 126, 127, 128` (c), Dropped abortable response body is kept by its own controller. The wrapper's algorithms own AbortableBody and AbortableBody owns the controller of that same stream. Normal completion clears the algorithms but an unread HWM-zero response may be abandoned without a terminal callback.
- **DEFECT (A-FETCH-ABORT-TRANSPORT)** `131, 132, 134, 135, 136, 137, 138, 195` (b/c), Abort can outlive the owning virtual transport frame. The default HTTP client copies ClientState before suspension. A transport whose member coroutine uses its receiver can survive the front-end await without owning the virtual object.
- **SAFE** `134, 139, 140, 141, 142, 143, 144, 146, 149, 152` (b/c/e), Abort-window result and owned error callback. The send frame owns the signal and response and schedules a source with its own Reason. The wrapper uses synchronous factories and the awaited abortable Task is named.
- **DEFECT (A-FETCH-REENTRANT-FUNCTION)** `176, 177, 178, 181, 183, 189, 191, 193` (a/b/c), Fetch calls a replaceable slot before reliably taking ownership. Argument evaluation may invoke the old callable before copying its owner. A synchronous injected function can replace the active slot and drop its own closure during that invocation.
- **DEFECT (A-FETCH-CALLBACK-INPUT)** `176, 178, 181, 183` (a/b), Injected async Fetch arguments cross the erasure lifetime boundary. The wrapper retains the callable but the by-value std::function invocation owns its arguments only within that call expression.
- **DEFECT (A-MOCK-HANDLER-OWNER)** `204, 205, 206` (a/b/c), Mock transport returns a callback task without its handler owner. The ordinary send method records a copy but passes a separate erased Request to its handler and returns the child task directly.

### `src/rt/fetch_transport.cpp`

Reviewed DNS, both connection branches, concurrent upload/download, stale retry, redirects, EOF/error/cancel and pooling handoff. The writer starts independently so an early response can be delivered before upload completion.

- **SAFE** `55, 57, 58, 60, 61, 63, 66, 69, 81, 93, 94, 95, 96, 103, 104, 105, 106, 109, 110, 111` (b/d/e), Owned request body and DNS inputs. The request frame owns both handles and copies bytes. The resolver frame owns host, service, resolver, error token and the final endpoint copy.
- **SAFE** `95, 97, 98, 99, 100, 106, 107` (b/c), DNS cancellation closure. The cancellation slot retains only a weak resolver reference, and the active resolution keeps the resolver and slot context alive until completion or teardown.
- **SAFE** `117, 118, 124, 125, 130, 131, 133, 134, 139, 143, 144, 146, 147, 155, 156, 157, 161, 162` (b/d/e), TCP connection, TLS receiver and pool acquisition. The private endpoint vector cannot be mutated by other work, and the connecting frame owns the client, TLS context owner and Connection across the handshake.
- **SAFE** `182, 184, 186, 188, 189, 193, 194, 197, 198, 203, 204, 209, 210, 219, 222, 223, 225, 226, 229, 230, 231, 270, 274, 275, 279` (a/b/c/d), Detached write task, dynamic buffers and local cleanup. The writer owns connection, reader, upload inputs and each write owner. The synchronous scope-exit reference is to a local declared before its guard.
- **DEFECT (A-FETCH-UPLOAD-STRANDED)** `173, 182, 203, 204, 209, 210, 215, 216, 274, 276, 277, 279, 296, 298, 299, 349, 350, 368, 372, 378, 475, 478, 479, 530, 537, 555` (b/c), Terminal connection close can strand a detached upload reader. Closing the socket wakes its native operations. A writeRequest frame waiting on a JavaScript-style reader Promise also needs its reader released or cancelled.
- **SAFE** `264, 265, 269, 271, 280, 282, 284, 288, 293, 294, 302, 306, 307, 308, 314, 316, 318, 319, 320, 323, 328` (b/d), Exchange head parse, retry and connection handoff. Response head fields are owned and the input view ends before fill; retry passes owned configuration and connection into the exchange frame.
- **SAFE** `334, 335, 336, 341, 349, 354, 355, 357, 360, 368, 370, 373, 383, 384, 389, 390, 396, 397, 402, 409, 410, 464, 466, 467, 470, 471, 474, 475, 476, 478, 479, 483, 488` (b/c/d), Response pump, parser views and source closures. The stream owns the pump while active, pool backedges are weak, and parser views are consumed before queue mutation or suspension. Scope-bound loop ownership is required by docs/rt/async.md.
- **SAFE** `413, 416, 417, 419, 421, 422, 426, 427, 428, 429, 436, 437, 438, 440, 445, 446, 447, 455, 457, 458` (a/b/c/d/e), Native fill receiver during response cancellation. The named fill Task pins the Connection through the native read. Cancellation sets the shared flag before dropping the pump's slot, and the continuation checks it before using that slot.
- **DEFECT (A-FETCH-RESPONSE-STRANDED)** `135, 214, 215, 216, 236, 237, 238, 416, 436, 437, 438, 440, 441, 452, 455, 474` (b/c), Post-head upload failure leaves a response read parked. The detached upload writer can close the Connection with a body failure after the response exists. The response pull treats the resulting aborted read as a silent successful pull.
- **SAFE** `492, 493, 496, 497, 498, 501, 502, 503, 504, 513, 514, 515, 518, 519, 520, 521, 522, 523, 526, 527, 533, 535, 549, 552, 553, 559, 561, 562, 563, 567, 568, 581, 584, 589, 595, 596` (b/d/e), Redirect drain, header edits and public send owner. The redirect frame owns request metadata, next URL and exchange. Header iteration is synchronous, and the public ordinary wrapper copies ClientState into the deferred operation.

### `src/rt/fetch_transport_codec.cpp`

Reviewed native and dynamically resolved codec branches. Decoder operations and function-table initialization run synchronously.

- **SAFE** `24, 25, 27, 34, 35, 37, 40, 44, 45, 47, 52, 64, 67, 73, 76, 78, 79, 80, 85, 88` (b/d), Zlib adapter and borrowed decoder buffers. The response pump owns the decoder and input during the synchronous write, and each inflater output span is copied before the next read. On failure the pump stops using the decoder.
- **SAFE** `122, 124, 127, 129, 131, 134, 135, 137, 140, 147, 150, 154, 157, 160, 163, 164, 174, 179, 181` (b/c), Brotli function table and streaming buffer ownership. Each decoder owns its C state and borrows a static API table, backed by the process-lifetime loader handle in the fallback. The C call uses stack buffers whose bytes are appended before returning.
- **SAFE** `222, 224, 227, 229, 231, 234, 235, 238, 241, 248, 250, 252, 256, 259, 261, 262, 263, 265, 271, 276, 278` (b/c), Zstd function table and streaming buffer ownership. Each decoder frees its C stream through the static API table; its input view and output buffer descriptors are used entirely in one synchronous write.
- **SAFE** `285, 287, 288, 300, 301, 302, 303, 304, 309, 311, 314, 315, 316, 320, 321, 323, 324, 327, 329, 332, 334, 339, 340, 345, 346` (b/d), Decoder selection and coding-name views. The code copies each view into its local vector before taking the selected reference, performs no later mutation of that vector and passes the URL into an owning string.

## Ranked defects and counts

Reviewed **19/30 files** and **112 site groups**. A site group can have multiple line anchors and hazard classes under the same verdict.

| Hazard class | DEFECT | SAFE | UNCLEAR |
| --- | ---: | ---: | ---: |
| a. Capturing coroutine closure | 5 | 19 | 0 |
| b. Borrowed coroutine parameter or receiver | 11 | 65 | 3 |
| c. Callback ownership or cycle | 16 | 66 | 4 |
| d. Container invalidation | 3 | 38 | 0 |
| e. GCC 12 await-expression rule | 4 | 12 | 1 |
| **Unique site groups** | 22 | 85 | 5 |

### 1. A-FETCH-ABORT-TRANSPORT (memory safety in the injectable transport path) — Abort can outlive the owning virtual transport frame

`src/rt/fetch.cpp:131, 132, 134, 135, 136, 137, 138, 195`

- An installed transport's member send coroutine suspends on an rt Promise. Replace the global transport so sendThrough is its remaining owner, then abort the signal.
- abortable rejects sendThrough immediately but cannot interrupt the rt Promise. sendThrough releases the transport while the child's member frame remains pending.

```diff
--- a/cpp/gateway/src/rt/fetch.cpp
+++ b/cpp/gateway/src/rt/fetch.cpp
@@ -136,2 +136,3 @@
       Task<Response> sending = transport->send(std::move(request));
-      Task<Response> guarded = abortable(signal, std::move(sending));
+      auto ownedSend = detail::keepAlive<Response>(std::move(transport), std::move(sending));
+      Task<Response> guarded = abortable(signal, std::move(ownedSend));
```

### 2. A-STREAM-CANCEL-OWNER (memory safety in stream cancellation) — Stream cancel borrows the owner slot through callback invocation

`include/uos/rt/stream.hpp:238, 249, 251, 252, 253, 262, 264, 336, 338, 352, 355, 396, 398, 401, 404`

- reader.cancel closes the stream and invokes the underlying cancel callback while borrowing reader->stream.
- The callback releases the reader lock. The same shared_ptr slot becomes null, then cancelStreamInto dereferences it to clear the algorithms.

```diff
--- a/cpp/gateway/include/uos/rt/stream.hpp
+++ b/cpp/gateway/include/uos/rt/stream.hpp
@@ -238,1 +238,1 @@
-    template <class T> void cancelStreamInto(const std::shared_ptr<StreamState<T>>& stream, Reason reason, const std::shared_ptr<PromiseState<void>>& out) {
+    template <class T> void cancelStreamInto(std::shared_ptr<StreamState<T>> stream, Reason reason, const std::shared_ptr<PromiseState<void>>& out) {
```

### 3. A-FETCH-CALLBACK-INPUT (memory safety in the injectable Fetch API) — Fetch erasure can discard input storage before a borrowing coroutine starts

`include/uos/rt/fetch.hpp:20, 27, 28, 29, 33, 38, 40, 41, 49`

- Construct Fetch with a coroutine callable taking const RequestInfo& or const RequestInit&. Fetch invokes the by-value std::function and returns a lazy Task.
- The full expression in Fetch::operator() ends and destroys the erased argument. The child resumes with a dangling reference even though keepAlive retains the callable.

```diff
diff --git a/cpp/gateway/include/uos/rt/fetch.hpp b/cpp/gateway/include/uos/rt/fetch.hpp
--- a/cpp/gateway/include/uos/rt/fetch.hpp
+++ b/cpp/gateway/include/uos/rt/fetch.hpp
@@ -20 +20 @@
-    using Function = std::function<Task<Response>(RequestInfo input, RequestInit init)>;
+    using Function = std::function<Task<Response>(RequestInfo&& input, RequestInit&& init)>;
@@ -49 +49,2 @@
-    static Task<Response> keepAlive(std::shared_ptr<const Function> function, Task<Response> task);
+    using Inputs = std::pair<RequestInfo, RequestInit>;
+    static Task<Response> keepAlive(std::shared_ptr<const Function> function, std::shared_ptr<Inputs> inputs, Task<Response> task);
diff --git a/cpp/gateway/src/rt/fetch.cpp b/cpp/gateway/src/rt/fetch.cpp
--- a/cpp/gateway/src/rt/fetch.cpp
+++ b/cpp/gateway/src/rt/fetch.cpp
@@ -176,4 +176,7 @@
   Task<Response> Fetch::operator()(RequestInfo input, RequestInit init) const {
-    if (!function_) throw std::logic_error("rt::Fetch: called an empty fetch value");
-    return keepAlive(function_, (*function_)(std::move(input), std::move(init)));
+    auto function = function_;
+    if (!function) throw std::logic_error("rt::Fetch: called an empty fetch value");
+    auto inputs = std::make_shared<Inputs>(std::move(input), std::move(init));
+    auto task = (*function)(std::move(inputs->first), std::move(inputs->second));
+    return keepAlive(std::move(function), std::move(inputs), std::move(task));
   }
@@ -181,3 +184,4 @@
-  Task<Response> Fetch::keepAlive(std::shared_ptr<const Function> function, Task<Response> task) {
+  Task<Response> Fetch::keepAlive(std::shared_ptr<const Function> function, std::shared_ptr<Inputs> inputs, Task<Response> task) {
     (void)function;
+    (void)inputs;
     Response response = co_await std::move(task);
```

### 4. A-FETCH-REENTRANT-FUNCTION (memory safety during a reentrant Fetch callback) — Fetch calls a replaceable slot before reliably taking ownership

`src/rt/fetch.cpp:176, 177, 178, 181, 183, 189, 191, 193`

- fetch invokes a synchronous Fetch callback through the global slot. The callable argument to keepAlive is evaluated first and the callback replaces globalThis.fetch.
- Replacement drops the callback's only owner. Its remaining code or returned child can borrow the destroyed closure, and keepAlive can pick up the replacement.

```diff
--- a/cpp/gateway/src/rt/fetch.cpp
+++ b/cpp/gateway/src/rt/fetch.cpp
@@ -177,2 +177,4 @@
-    if (!function_) throw std::logic_error("rt::Fetch: called an empty fetch value");
-    return keepAlive(function_, (*function_)(std::move(input), std::move(init)));
+    auto function = function_;
+    if (!function) throw std::logic_error("rt::Fetch: called an empty fetch value");
+    auto task = (*function)(std::move(input), std::move(init));
+    return keepAlive(std::move(function), std::move(task));
```

### 5. A-MOCK-HANDLER-OWNER (memory safety in local transport injection) — Mock transport returns a callback task without its handler owner

`src/rt/fetch.cpp:204, 205, 206`

- Call MockTransport::send with a capturing coroutine handler and release the mock while retaining the returned task.
- The lazy handler then runs with a dangling closure. A handler borrowing the erased Request also loses that argument at the end of send's call expression.

```diff
--- a/cpp/gateway/include/uos/rt/fetch.hpp
+++ b/cpp/gateway/include/uos/rt/fetch.hpp
@@ -105,2 +105,2 @@
-    using Handler = std::function<Task<Response>(Request request)>;
-    explicit MockTransport(Handler handler) : handler_(std::move(handler)) {}
+    using Handler = std::function<Task<Response>(Request&& request)>;
+    explicit MockTransport(Handler handler) : handler_(std::make_shared<const Handler>(std::move(handler))) {}
@@ -112 +112 @@
-    Handler handler_;
+    std::shared_ptr<const Handler> handler_;
--- a/cpp/gateway/src/rt/fetch.cpp
+++ b/cpp/gateway/src/rt/fetch.cpp
@@ -205,2 +205,4 @@
     requests_.push_back(request);
-    return handler_(std::move(request));
+    auto invocation = std::make_shared<std::pair<std::shared_ptr<const Handler>, Request>>(handler_, std::move(request));
+    auto task = (*invocation->first)(std::move(invocation->second));
+    return detail::keepAlive<Response>(std::move(invocation), std::move(task));
```

### 6. A-PROMISE-CALLBACK-INPUT (memory safety in the public runtime API) — Task-returning Promise callbacks can borrow a destroyed outcome

`include/uos/rt/async.hpp:692, 701, 704, 713, 715, 726, 731, 744, 745, 746, 758, 760`

- Register a then or catch callback whose Task takes a const reference to the outcome. Drop the source Promise while keeping the derived Promise.
- The queued reaction starts the callback until its first suspension. The reaction finishes and releases the source, then the callback resumes with a dangling outcome reference.

```diff
--- a/cpp/gateway/include/uos/rt/async.hpp
+++ b/cpp/gateway/include/uos/rt/async.hpp
@@ -692,1 +692,2 @@
-    template <class U, class Call> void settleWith(const std::shared_ptr<PromiseState<U>>& target, std::shared_ptr<void> keep, Call&& call) {
+    template <class U, class Call> void settleWith(const std::shared_ptr<PromiseState<U>>& target, std::shared_ptr<void> keep,
+                                                 std::shared_ptr<void> source, Call&& call) {
@@ -701,1 +702,2 @@
-          start(currentLoop(), keepAlive<void>(std::move(keep), runInto(inner, call())));
+          auto owners = std::make_shared<std::pair<std::shared_ptr<void>, std::shared_ptr<void>>>(std::move(keep), std::move(source));
+          start(currentLoop(), keepAlive<void>(std::move(owners), runInto(inner, call())));
@@ -731,1 +733,1 @@
-      detail::settleWith(target, callback, [&] { return detail::invokeWithValue<T>(*callback, *source); });
+      detail::settleWith(target, callback, source, [&] { return detail::invokeWithValue<T>(*callback, *source); });
@@ -745,2 +747,2 @@
-      if (source->status == detail::PromiseCore::Status::rejected) detail::settleWith(target, rejected, [&] { return (*rejected)(source->error); });
-      else detail::settleWith(target, fulfilled, [&] { return detail::invokeWithValue<T>(*fulfilled, *source); });
+      if (source->status == detail::PromiseCore::Status::rejected) detail::settleWith(target, rejected, source, [&] { return (*rejected)(source->error); });
+      else detail::settleWith(target, fulfilled, source, [&] { return detail::invokeWithValue<T>(*fulfilled, *source); });
@@ -760,1 +762,1 @@
-        detail::settleWith(target, callback, [&] { return (*callback)(source->error); });
+        detail::settleWith(target, callback, source, [&] { return (*callback)(source->error); });
```

### 7. A-ABORT-OWNER (memory safety in the public runtime API) — Abort dispatch borrows the controller's state slot

`src/rt/abort.cpp:62, 69, 74, 81, 95, 228`

- AbortController::abort passes a reference to its own shared pointer and begins notifying listeners.
- A listener drops the controller or replaces its signal. Dispatch then accesses the ended state or cleans up the replacement's listeners through that borrowed slot.

```diff
--- a/cpp/gateway/src/rt/abort.cpp
+++ b/cpp/gateway/src/rt/abort.cpp
@@ -81,1 +81,1 @@
-    void signalAbort(const std::shared_ptr<State>& signal, Reason reason) {
+    void signalAbort(std::shared_ptr<State> signal, Reason reason) {
```

### 8. A-FETCH-ABORT-BODY-CYCLE (request-path leak) — Dropped abortable response body is kept by its own controller

`src/rt/fetch.cpp:70, 72, 73, 74, 77, 112, 114, 115, 120, 123, 124, 125, 126, 127, 128`

- A request with an explicit signal receives a streaming response. The front end caches the wrapper's controller in the owner retained by its pull and cancel callbacks.
- The caller drops the response without reading it or aborting. No pull occurs, and the controller, stream algorithms, source reader and signal remain retained.

```diff
--- a/cpp/gateway/src/rt/fetch.cpp
+++ b/cpp/gateway/src/rt/fetch.cpp
@@ -72 +72 @@
-      std::optional<ReadableStreamDefaultController<ByteChunk>> controller;
+      std::weak_ptr<detail::StreamState<ByteChunk>> wrapper;
@@ -76,2 +76,3 @@
       explicit AbortableBody(AbortSignal signal_) : signal(std::move(signal_)) {}
+      ~AbortableBody() { finish(); }
       void finish() {
@@ -120,2 +121,2 @@
-        if (held->controller) held->controller->error(reason);
+        if (auto wrapper = held->wrapper.lock()) wrapper->error(reason);
         spawn(held->reader->cancel(reason), [](Reason) {});
@@ -124,5 +125,6 @@
-      wrapper.start = [body](ReadableStreamDefaultController<ByteChunk> controller) { body->controller = controller; };
       wrapper.pull = [body](ReadableStreamDefaultController<ByteChunk> controller) { return pullAbortable(body, controller); };
       wrapper.cancel = [body](Reason reason) { return cancelAbortable(body, std::move(reason)); };
+      ByteStream stream(std::move(wrapper), QueuingStrategy{0});
+      body->wrapper = stream.state();
       signal.addEventListener("abort", body->onAbort, AddEventListenerOptions{.once = true});
-      response.setBodyStream(ByteStream(std::move(wrapper), QueuingStrategy{0}));
+      response.setBodyStream(std::move(stream));
```

### 9. A-FETCH-UPLOAD-STRANDED (request-path retention after a failed or early response) — Terminal connection close can strand a detached upload reader

`src/rt/fetch_transport.cpp:173, 182, 203, 204, 209, 210, 215, 216, 274, 276, 277, 279, 296, 298, 299, 349, 350, 368, 372, 378, 475, 478, 479, 530, 537, 555`

- The client sends the upload head and parks its detached writer in reader.read(). The peer closes before the response head, or the early response finishes before the upload source produces another chunk.
- The terminal exchange closes the socket with an unaborted signal. The upload Promise never settles and retains the writer, stream and closed connection on the process loop.

```diff
--- a/cpp/gateway/src/rt/fetch_transport.cpp
+++ b/cpp/gateway/src/rt/fetch_transport.cpp
@@ -173 +173,6 @@
       std::optional<ReadableStreamDefaultReader<ByteChunk>> reader;
+      void releaseReader() {
+        auto active = std::exchange(reader, std::nullopt);
+        if (active) active->releaseLock();
+      }
@@ -276,2 +281,3 @@
         connection->close();
         if (writer->reader && signal.aborted()) spawn(writer->reader->cancel(signal.reason()), ignoreRejection);
+        writer->releaseReader();
@@ -377,2 +383,3 @@
       }
       connection->close();
+      pump.writer->releaseReader();
```

### 10. A-FETCH-RESPONSE-STRANDED (request-path pending response read) — Post-head upload failure leaves a response read parked

`src/rt/fetch_transport.cpp:135, 214, 215, 216, 236, 237, 238, 416, 436, 437, 438, 440, 441, 452, 455, 474`

- The response head arrives while an upload and a response body read are both pending. The upload reader rejects and writeRequest records the failure and closes the connection.
- The body fill reports aborted with pump.cancelled false. pullBody returns without closing or erroring the stream, leaving the downstream reader Promise and its continuation parked.

```diff
--- a/cpp/gateway/src/rt/fetch_transport.cpp
+++ b/cpp/gateway/src/rt/fetch_transport.cpp
@@ -439,4 +439,5 @@
           if (status == ReadStatus::data) continue;
           if (pump->connection->timedOut()) rethrow(timeoutErrorReason());
-          if (status == ReadStatus::aborted) co_return;
+          if (pump->writer->bodyFailure) rethrow(pump->writer->bodyFailure);
+          if (status == ReadStatus::aborted) throw closedError(pump->url);
           if (status == ReadStatus::eof && pump->framing == Framing::close) {
```

### 11. A-PROCESS-SIGNAL-CONTEXT (memory safety with scoped runtime loops) — Static process signal registry retains a scoped Asio context

`src/rt/process_loop.cpp:100, 102, 107, 108, 119, 124, 125, 134, 136, 142, 148`

- Register process::on inside a local LoopScope. The static signal registry constructs its Asio signal_set on that loop.
- Destroy the loop before explicitly removing the listener. Later registry use or destruction still uses the old Asio service and executor.

```diff
--- a/cpp/gateway/src/rt/process_loop.cpp
+++ b/cpp/gateway/src/rt/process_loop.cpp
@@ -106,5 +106,13 @@
+    struct SignalRegistry final : asio::execution_context::service {
+      inline static asio::execution_context::id id;
+      Signals state;
+      explicit SignalRegistry(asio::execution_context& context) : asio::execution_context::service(context) {}
+      void shutdown() override {
+        state.set.reset();
+        state.listeners.clear();
+      }
+    };
+
     Signals& signals() {
-      static Signals state;
-      return state;
+      return asio::use_service<SignalRegistry>(rt::loop().context()).state;
     }
```

### 12. A-STREAM-TEE-READ-CYCLE (retention on cloned streaming bodies) — In-flight tee read keeps abandoned branches' source alive

`include/uos/rt/stream.hpp:444, 445, 446, 453, 454, 458, 460, 464, 469, 475, 479, 485, 486, 493, 496, 502, 554, 557, 558, 559, 561, 562, 567, 568`

- Tee starts a read of a source that has no next chunk yet, registering the continuation on the source read request.
- Drop both branch handles and the original source. The read request owns the parked frame, whose TeeState parameter owns the source and its read queue until external settlement or Loop teardown.

```diff
--- a/cpp/gateway/include/uos/rt/stream.hpp
+++ b/cpp/gateway/include/uos/rt/stream.hpp
@@ -460,5 +460,12 @@
-    template <class T> Task<void> teeRead(std::shared_ptr<TeeState<T>> tee) {
+    template <class T> Task<void> teeRead(std::weak_ptr<TeeState<T>> weak) {
+      Task<ReadableStreamReadResult<T>> reading;
+      {
+        auto live = weak.lock();
+        if (!live) co_return;
+        reading = readFrom(live->reader);
+      }
       ReadableStreamReadResult<T> result;
       bool failed = false;
       try {
-        result = co_await readFrom(tee->reader);
+        result = co_await std::move(reading);
@@ -467,1 +474,3 @@
       }
+      auto tee = weak.lock();
+      if (!tee) co_return;
@@ -502,1 +511,1 @@
-      start(currentLoop(), teeRead(tee));
+      start(currentLoop(), teeRead<T>(std::weak_ptr<TeeState<T>>(tee)));
```

### 13. A-PROMISE-REACTION-CYCLE (retention on long-running loops) — Pending Promise reactions self-own their source

`src/rt/async.cpp:485, 490, 491, 492, 494, 497`

- all registers a reaction whose owning source capture points to the state storing that reaction. Another input can reject the combined Promise.
- Drop the pending input and every resolver. Its last ownership is the reaction inside itself, so neither that source nor the captured output/continuations can be reclaimed.

```diff
--- a/cpp/gateway/src/rt/async.cpp
+++ b/cpp/gateway/src/rt/async.cpp
@@ -416,1 +416,3 @@
-      currentLoop().impl().microtasks.push_back(std::move(reaction));
+      currentLoop().impl().microtasks.push_back(
+          [source = shared_from_this(), job = std::move(reaction)]() mutable { job(); });
+
@@ -424,1 +426,4 @@
-    for (auto& reaction : reactions) impl.microtasks.push_back(std::move(reaction));
+    for (auto& reaction : reactions) {
+      impl.microtasks.push_back(
+          [source = shared_from_this(), job = std::move(reaction)]() mutable { job(); });
+    }
@@ -492,1 +497,1 @@
-      source->react([source, out, remaining] {
+      source->react([source = source.get(), out, remaining] {
```

### 14. A-GENERATOR-STEP-OWNER (public runtime liveness) — Pending generator step does not keep its generator alive

`include/uos/rt/async.hpp:1041, 1058, 1061, 1062, 1063, 1064, 1099, 1101`

- Call next on a generator whose body first yields through a microtask. Keep the step Task and release the generator handle before that microtask runs.
- yieldTo finds the weak core expired and abandons the body. The outstanding step has no owner capable of fulfilling or rejecting it.

```diff
--- a/cpp/gateway/include/uos/rt/async.hpp
+++ b/cpp/gateway/include/uos/rt/async.hpp
@@ -1061,5 +1061,6 @@
-    template <class T> Task<IteratorResult<T>> requestStep(const std::shared_ptr<GeneratorCore<T>>& core, bool isReturn) {
+    template <class T> Task<IteratorResult<T>> requestStep(std::shared_ptr<GeneratorCore<T>> core, bool isReturn) {
       auto result = std::make_shared<PromiseState<IteratorResult<T>>>();
       core->enqueue(isReturn, result);
-      return awaitOwned(std::move(result));
+      auto waiting = awaitOwned(std::move(result));
+      return keepAlive<IteratorResult<T>>(std::move(core), std::move(waiting));
     }
```

### 15. A-LOOP-TEARDOWN-OWNER (wrong-loop teardown and retention) — Scoped Loop teardown runs callbacks under the ambient loop

`src/rt/async.cpp:252, 257, 258, 261, 263, 266, 268, 367, 368, 396, 397, 400, 401, 443, 445`

- A pending coroutine belongs to a local Loop. Loop::run or blockOn returns and its LoopScope restores the previous loop.
- Destroying the local Loop drops the frame. A destructor that queues a cleanup job sends it to the previous or permanent process loop, outside the teardown drain.

```diff
--- a/cpp/gateway/src/rt/async.cpp
+++ b/cpp/gateway/src/rt/async.cpp
@@ -252,1 +252,2 @@
   Loop::Impl::~Impl() {
+    LoopScope scope(owner);
```

### 16. A-PROMISE-GCC-AWAIT (GCC 12 portability rule) — Implicit shared_ptr conversion in Promise await expressions

`include/uos/rt/async.hpp:219, 375`

- awaitOwned or awaitState calls awaitSettled directly with a typed state. The call converts it to an owning base shared_ptr inside co_await.
- The retained GCC 12 coroutine rule rejects this expression shape because await temporaries can be destroyed twice. This occurrence has static evidence.

```diff
--- a/cpp/gateway/include/uos/rt/async.hpp
+++ b/cpp/gateway/include/uos/rt/async.hpp
@@ -219,1 +219,2 @@
-      co_await awaitSettled(state);
+      auto ready = awaitSettled(state);
+      co_await std::move(ready);
@@ -375,1 +376,2 @@
-      co_await detail::awaitSettled(state);
+      auto ready = detail::awaitSettled(state);
+      co_await std::move(ready);
```

### 17. A-GENERATOR-GCC-AWAIT (GCC 12 portability rule) — Forwarded generator values constructed within await expressions

`include/uos/rt/async.hpp:1073, 1123, 1124`

- Instantiate the forwarding helpers with an owning yielded value, such as bytes, and a by-value callback. The moved parameter is materialized in the await expression.
- The retained GCC 12 coroutine rules require creating that operation in a separate statement. This occurrence has static evidence.

```diff
--- a/cpp/gateway/include/uos/rt/async.hpp
+++ b/cpp/gateway/include/uos/rt/async.hpp
@@ -1073,1 +1073,2 @@
-          co_await outer(std::move(*step.value));
+          auto passing = outer(std::move(*step.value));
+          co_await std::move(passing);
@@ -1123,2 +1124,3 @@
-        if constexpr (std::is_same_v<Result, Task<bool>>) proceed = co_await body(std::move(*step.value));
-        else co_await body(std::move(*step.value));
+        auto handling = body(std::move(*step.value));
+        if constexpr (std::is_same_v<Result, Task<bool>>) proceed = co_await std::move(handling);
+        else co_await std::move(handling);
```

### 18. A-STREAM-GCC-AWAIT (GCC 12 portability rule) — Stream callback and yield arguments materialized inside awaits

`include/uos/rt/stream.hpp:72, 540`

- A stream uses an owning controller, cancellation reason or chunk in a callback/yield invocation. The argument is materialized inside co_await.
- The GCC 12 rule calls for creating the Task separately. Using an rvalue-reference erased signature keeps a borrowing callee attached to runAsync's frame while performing that split.

```diff
--- a/cpp/gateway/include/uos/rt/stream.hpp
+++ b/cpp/gateway/include/uos/rt/stream.hpp
@@ -49,1 +49,1 @@
-        else async_ = std::make_shared<const std::function<Task<void>(A)>>(std::forward<F>(callable));
+        else async_ = std::make_shared<const std::function<Task<void>(A&&)>>(std::forward<F>(callable));
@@ -72,3 +72,6 @@
-      static Task<void> runAsync(std::shared_ptr<const std::function<Task<void>(A)>> function, A argument) { co_await (*function)(std::move(argument)); }
+      static Task<void> runAsync(std::shared_ptr<const std::function<Task<void>(A&&)>> function, A argument) {
+        auto handling = (*function)(std::move(argument));
+        co_await std::move(handling);
+      }
       std::shared_ptr<const std::function<void(A)>> sync_;
-      std::shared_ptr<const std::function<Task<void>(A)>> async_;
+      std::shared_ptr<const std::function<Task<void>(A&&)>> async_;
@@ -540,1 +543,2 @@
-          co_await yield(std::move(*step->value));
+          auto yielding = yield(std::move(*step->value));
+          co_await std::move(yielding);
```
