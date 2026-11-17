# uos::rt async: tasks, promises, timers, abort, streams

Oracle: Bun 1.4.0. Headers: `uos/rt/async.hpp`, `uos/rt/clock.hpp`,
`uos/rt/abort.hpp`, `uos/rt/stream.hpp`, `uos/rt/test_async.hpp`.

A JS `async` function is a `rt::Task<T>` (`boost::asio::awaitable<T>`). Everything
runs on one `rt::Loop` (one io_context thread), like JavaScript's run-to-completion
model, so ported code needs no locks. The loop runs one macrotask at a time (a
timer callback, an IO completion, a posted handler) and drains the microtask queue
after each one. Coroutines start synchronously: calling `rt::Promise(f())` or
`rt::spawn(f())` runs `f` up to its first suspension right there, as JS does.

## 1. TypeScript -> C++

| TypeScript                                                                                                                        | C++                                                                                                                                                                                                                                                                                                                                                                                                                        |
| --------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `async function f(a: A): Promise<T>`                                                                                              | `rt::Task<T> f(A a)` — parameters by value (section 4)                                                                                                                                                                                                                                                                                                                                                                     |
| `await f()`                                                                                                                       | `co_await f()` (callee runs inline; see section 5 for the one-turn difference)                                                                                                                                                                                                                                                                                                                                             |
| `await promise` (a stored promise)                                                                                                | `co_await promise.wait()`                                                                                                                                                                                                                                                                                                                                                                                                  |
| `const p = f(); ...; await p`                                                                                                     | `rt::Promise<T> p = f();` (starts now) `...; co_await p.wait();`                                                                                                                                                                                                                                                                                                                                                           |
| `void f()` / `void f().catch(() => {})`                                                                                           | `rt::spawn(f())` / `rt::spawn(f(), rt::ignoreRejection)`                                                                                                                                                                                                                                                                                                                                                                   |
| `new Promise((resolve, reject) => {...})`                                                                                         | `rt::newPromise<T>([&](auto resolve, auto reject) {...})`                                                                                                                                                                                                                                                                                                                                                                  |
| `Promise.withResolvers<T>()`                                                                                                      | `auto [promise, resolve, reject] = rt::withResolvers<T>();`                                                                                                                                                                                                                                                                                                                                                                |
| `Promise.resolve(v)` / `Promise.reject(e)`                                                                                        | `rt::Promise<T>::resolve(v)` / `rt::Promise<T>::reject(reason)`                                                                                                                                                                                                                                                                                                                                                            |
| `return Promise.resolve(v)` where the C++ signature is a Task (an override, a callback slot)                                      | `return rt::fulfilledTask(v);` / `return rt::fulfilledTask();` (completes inline when awaited: skips the turn JS `await` takes, like every Task; `rt::Promise<T>::resolve(v).wait()` takes it)                                                                                                                                                                                                                             |
| `p1 === p2` (promises)                                                                                                            | `p1 == p2` (copies are the same promise; `then`/`catch_`/`finally_` make new ones)                                                                                                                                                                                                                                                                                                                                         |
| `WeakMap<Promise<T>, V>` / `WeakSet<Promise<T>>`                                                                                  | `rt::WeakMap<rt::Promise<T>, V>` / `rt::WeakSet<...>` (uos/rt/weak.hpp), keyed by `p.identity()` / `p.weakIdentity()`: an entry dies with the last copy, resolver or awaiting task                                                                                                                                                                                                                                         |
| `await Promise.all(list)`                                                                                                         | `co_await rt::all(std::move(promises)).wait()` (vector) or `rt::all(a, b, c)` (tuple)                                                                                                                                                                                                                                                                                                                                      |
| `Promise.allSettled / race / any`                                                                                                 | `rt::allSettled / rt::race / rt::any` (all return `rt::Promise`)                                                                                                                                                                                                                                                                                                                                                           |
| `p.then(f)`, `p.then(f, g)`, `p.catch(g)`, `p.finally(h)`                                                                         | `p.then(f)`, `p.then(f, g)`, `p.catch_(g)`, `p.finally_(h)`                                                                                                                                                                                                                                                                                                                                                                |
| `queueMicrotask(f)` / `await null`                                                                                                | `rt::queueMicrotask(f)` / `co_await rt::microtask()`                                                                                                                                                                                                                                                                                                                                                                       |
| `setTimeout / clearTimeout / setInterval / clearInterval`                                                                         | same names; handle `rt::Timeout` (`ref`, `unref`, `hasRef`, `refresh`, `id`)                                                                                                                                                                                                                                                                                                                                               |
| `await new Promise((r) => setTimeout(r, ms))`, `Bun.sleep(ms)`                                                                    | `co_await rt::sleep(ms)`                                                                                                                                                                                                                                                                                                                                                                                                   |
| `Date.now()` / `performance.now()`                                                                                                | `rt::dateNow()` / `rt::performanceNow()`                                                                                                                                                                                                                                                                                                                                                                                   |
| `throw new TypeError(m)`                                                                                                          | `throw rt::TypeError(m)` (any `rt::JsError`)                                                                                                                                                                                                                                                                                                                                                                               |
| `catch (e)`                                                                                                                       | `catch (const rt::JsError& e)`, or `catch (...)` with `rt::Reason e = std::current_exception()`                                                                                                                                                                                                                                                                                                                            |
| `error.code` (`Object.assign(new Error(m), { code })`, a class field `code = "..."`)                                              | `rt::JsError::code` (`std::optional<std::string>`, std::nullopt = undefined; set it after construction). Errors that carried a code before keep their own member, which hides the base one: read `fs::ErrnoError::code`, `CodedTypeError::code`, `ws::CodedError::code`, `InvalidURLError::code`, `NodeURLError::code`, `SQLiteError::code` through their type, and a port's own error class with a `code` member likewise |
| a non-Error thrown/rejected value                                                                                                 | `rt::thrown(json)` (`rt::ThrownValue`); `undefined` is a null `rt::Reason`                                                                                                                                                                                                                                                                                                                                                 |
| `new Error(m, { cause })`, any cause (`new TypeError(m, { cause: reason })`)                                                      | `rt::withCause(rt::JsError(m), cause)` / `rt::withCause(rt::TypeError(m), cause)` (uos/rt/error.hpp): sets `JsError::causeValue` to the cause as given (an `rt::Reason`: null is undefined, `rt::thrown(json)` a value) and, for an Error cause, `JsError::cause` as before                                                                                                                                                |
| `error.cause` (read generically), `"cause" in error`                                                                              | `std::optional<rt::Reason> c = rt::errorCause(reason)` (std::nullopt: no cause property; else the value: `causeValue`, or `JsError::cause` as a JsError copy); `error.cause === undefined` is `!c \|\| !*c`; then `rt::isError(*c)`, `rt::errorMessage(*c)`, `rt::thrownValue(*c)`, `rt::reasonToString(*c)`                                                                                                               |
| `String(e)`, `e instanceof Error`, `e.name`, `e.message`                                                                          | `rt::reasonToString(e)`, `rt::isError(e)`, `rt::errorName(e)`, `rt::errorMessage(e)`                                                                                                                                                                                                                                                                                                                                       |
| `try { await a } finally { await b }`                                                                                             | `co_await rt::withFinally(a(), cleanup)` or the idiom in section 2                                                                                                                                                                                                                                                                                                                                                         |
| `using` / `await using`                                                                                                           | not used by the TS; `rt::scopeExit(f)` for synchronous cleanup                                                                                                                                                                                                                                                                                                                                                             |
| `async function* g()`, `yield v`, `yield* inner`                                                                                  | `rt::AsyncGenerator<T, R>(body)`, `co_await yield(v)`, `co_await yield.from(inner)`                                                                                                                                                                                                                                                                                                                                        |
| `it.next()`, `it.return()`                                                                                                        | `co_await it.next()`, `co_await it.return_()` (`rt::IteratorResult<T>`)                                                                                                                                                                                                                                                                                                                                                    |
| `for await (const x of it) { ... }`                                                                                               | `co_await rt::forAwait(it, body)`; body returns `Task<void>` or `Task<bool>` (false = `break`)                                                                                                                                                                                                                                                                                                                             |
| `new AbortController()`, `.signal`, `.abort(r)`                                                                                   | `rt::AbortController c;` `c.signal()`, `c.abort(r)`                                                                                                                                                                                                                                                                                                                                                                        |
| `signal.aborted / reason / throwIfAborted()`                                                                                      | same names                                                                                                                                                                                                                                                                                                                                                                                                                 |
| `signal.addEventListener("abort", fn, { once: true })`                                                                            | `signal.addEventListener("abort", listener, {.once = true})`; `fn` is a named `rt::EventListener` (identity matters for remove)                                                                                                                                                                                                                                                                                            |
| `signal.onabort = fn`                                                                                                             | `signal.setOnabort(listener)` / `signal.onabort()`                                                                                                                                                                                                                                                                                                                                                                         |
| `AbortSignal.abort(r) / timeout(ms) / any([...])`                                                                                 | `rt::AbortSignal::abort(r) / timeout(ms) / any({...})`                                                                                                                                                                                                                                                                                                                                                                     |
| `new DOMException(m, name)`                                                                                                       | `rt::DOMException(m, name)` (`code()` as Bun)                                                                                                                                                                                                                                                                                                                                                                              |
| await something abortable (IO the signal must cancel)                                                                             | `co_await rt::abortable(signal, task)`; `co_await rt::sleep(ms, signal)`                                                                                                                                                                                                                                                                                                                                                   |
| `new ReadableStream({ start, pull, cancel }, { highWaterMark })`                                                                  | `rt::ReadableStream<T>(source, {.highWaterMark = n})`; `ReadableStream<Uint8Array>` is `rt::ReadableStream<rt::ByteChunk>`                                                                                                                                                                                                                                                                                                 |
| `controller.enqueue / close / error / desiredSize`                                                                                | same names (`desiredSize()` is `std::optional<double>`; null when errored)                                                                                                                                                                                                                                                                                                                                                 |
| `stream.getReader()`, `reader.read() / cancel(r) / releaseLock() / closed`                                                        | same names; `read()` registers the request when called                                                                                                                                                                                                                                                                                                                                                                     |
| `reader.read()` / `reader.cancel(r)` / `stream.cancel(r)` held or combined as promises (`Promise.race([reader.read(), aborted])`) | `reader.readPromise()` / `reader.cancelPromise(r)` / `stream.cancelPromise(r)` -> `rt::Promise` (the same request, settled in the turn JS's promise is; racing `rt::Promise(reader.read())` costs one turn more: a ready chunk then loses to an already-rejected abort)                                                                                                                                                    |
| `stream.locked`, `stream.cancel(r)`, `const [a, b] = stream.tee()`                                                                | `stream.locked()`, `stream.cancel(r)`, `auto [a, b] = stream.tee();`                                                                                                                                                                                                                                                                                                                                                       |
| the spec's [[disturbed]] (no JS property; what `bodyUsed` reads)                                                                  | `stream.disturbed()`: set by the first `read()` or a cancel, not by `getReader()`                                                                                                                                                                                                                                                                                                                                          |
| `for await (const chunk of stream)`                                                                                               | `co_await rt::forAwait(stream.values(), body)`                                                                                                                                                                                                                                                                                                                                                                             |

`error.cause`: `JsError::cause` (a `std::shared_ptr<const JsError>`) keeps holding Error causes
for its existing readers; `JsError::causeValue` (`std::optional<std::exception_ptr>`) holds any
cause. Its type is spelled `std::exception_ptr` because error.hpp must not include async.hpp
(asio): `rt::Reason` is an alias of that standard type, so the slot needs no forward declaration or
type-erased holder and a Reason assigns to it directly; only making a non-Error value
(`rt::thrown`, async.hpp) needs async.hpp. Error classes that already had a `causeValue` member of
their own (`ResponsesStreamError`, `CodexError`, `metered::ErrorWithCause`,
`auth_refresh::ErrorWithCause`, ...) hide the base slot: read theirs through their type
(`rt::errorCause` then sees only their Error causes, through `JsError::cause`); `rt::withCause` on
such a class sets its own member. A member function of a JsError subclass must not name a
parameter or local `causeValue` (-Wshadow).

Promise-returning rt calls do their synchronous part when called (register the read
request, start the timer, call the cancel algorithm) and return a Task or Promise for
the rest. Combinators and `reader.closed()` return `rt::Promise`; the others return
`rt::Task`. Await a Promise with `.wait()`: Boost 1.81 cannot `co_await` a promise
object directly.

## 2. Common shapes

```cpp
// async function with a sequential await loop
rt::Task<std::vector<User>> loadUsers(Store store, std::vector<std::string> ids) {
  std::vector<User> users;
  for (const auto& id : ids) {
    auto user = co_await store.load(id);  // `for (...) await`: one at a time
    users.push_back(std::move(user));
  }
  co_return users;
}

// await Promise.all(ids.map((id) => store.load(id)))
std::vector<rt::Promise<User>> pending;
for (const auto& id : ids) pending.push_back(store.load(id));  // each starts now, like map()
std::vector<User> users = co_await rt::all(std::move(pending)).wait();

// void recordUsage(context).catch(() => {});
rt::spawn(recordUsage(context), rt::ignoreRejection);

// AbortSignal.any([callerSignal, AbortSignal.timeout(30_000)]) passed to IO
auto timeout = rt::AbortSignal::timeout(30000);
auto signal = rt::AbortSignal::any({callerSignal, timeout});
FetchInit init{.signal = signal};  // a named local, never a temporary inside co_await
auto response = co_await rt::abortable(signal, upstream.request(init));

// Promise.race([work(), rejectAfter(ms)]): the loser keeps running, as in JS
auto deadline = rt::newPromise<int>([ms](auto, auto reject) {
  rt::setTimeout([reject] { reject(rt::makeReason(rt::JsError("timeout"))); }, ms);
});
std::vector<rt::Promise<int>> racers{rt::Promise(work()), deadline};
int value = co_await rt::race(std::move(racers)).wait();

// try { await work(); } finally { await release(lease); }
auto cleanup = [lease]() -> rt::Task<void> { co_await release(lease); };
co_await rt::withFinally(work(), cleanup);
// ... or inline (co_await is not allowed inside a catch handler). An early
// co_return inside the try skips this cleanup: use withFinally then.
rt::Reason failure;
try {
  co_await work();
} catch (...) {
  failure = std::current_exception();
}
co_await release(lease);
if (failure) std::rethrow_exception(failure);

// new ReadableStream({ async pull(controller) {...}, cancel(reason) {...} })
auto state = std::make_shared<RelayState>();  // outlives this function
rt::UnderlyingSource<rt::ByteChunk> source;
source.pull = [state](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) -> rt::Task<void> {
  auto next = co_await state->frames.next();
  if (next.done) {
    controller.close();
    co_return;
  }
  controller.enqueue(encode(*next.value));
};
source.cancel = [state](rt::Reason reason) { state->cancellation.abort(reason); };
rt::ReadableStream<rt::ByteChunk> body(source);

// reader loop
auto reader = body.getReader();
for (;;) {
  auto chunk = co_await reader.read();
  if (chunk.done) break;
  consume(*chunk.value);
}

// async function* frames(): yield, then for await with break
rt::AsyncGenerator<Frame> frames([state](rt::Yield<Frame> yield) -> rt::Task<void> {
  auto cleanup = rt::scopeExit([state] { state->close(); });  // the `finally`
  while (auto frame = co_await state->nextFrame()) co_await yield(std::move(*frame));
});
auto onFrame = [&](Frame frame) -> rt::Task<bool> { co_return handle(frame); };  // false = break
co_await rt::forAwait(frames, onFrame);

// a coroutine lambda that outlives the statement: rt::async owns it
rt::spawn(rt::async([id, kv]() -> rt::Task<void> { co_await refresh(kv, id); }));
```

The entry point: `int main() { rt::loop().setUncaughtPolicy(rt::UncaughtPolicy::report); rt::blockOn(serveMain()); }`
(section 6 says why `report`).

## 3. GCC 12.2 coroutine rules (mandatory; verified on this toolchain)

GCC 12.2 miscompiles some temporaries in coroutines (fixed in later GCC). Violations
of 1 and 2 compile cleanly and corrupt memory silently (a `shared_ptr` count goes
wrong), so follow them everywhere, tests included.

1. A full-expression that contains `co_await` must not create a lambda, a closure or an
   aggregate temporary (`T{...}`, designated initializers, `{ .signal = s }`), not even
   as an argument of an inner call: they are destroyed twice. Make it a named local
   first and pass the local (copy or `std::move`). Temporaries made by a constructor
   (`std::string("x")`, `std::shared_ptr`, `rt::AbortSignal`, a returned Task) are fine.
   Wrong: `co_await rt::withFinally(task, [x] {...})`, `co_await f(Options{.retries = 2})`,
   `co_await rt::async([x]() -> rt::Task<int> {...})`.
2. Bind a `co_await` result to a named variable before passing it to a function by
   value: `T v = co_await t; sink(std::move(v));`, never `sink(co_await t)` or
   `co_return co_await t;` (wrong for aggregates with owning members).
   `auto x = co_await t;`, `x = co_await t;`, `(co_await t).field` and discarding are fine.
3. No braced list (`std::vector<T>{a, b}`, `{a, b}` for an initializer_list parameter)
   and no string literal bound to a reference or forwarding parameter
   (`Promise<std::string>::resolve("x")`) inside a `co_await` full-expression: GCC
   rejects it ("array used as initializer"). Build the vector first; wrap literals in
   `std::string(...)`.
4. `co_await` is not allowed inside a `catch` handler (C++20): store
   `std::current_exception()` and act after the handler (section 2).
5. A promise variable is awaited with `co_await promise.wait()` (Boost 1.81 cannot
   `co_await` an lvalue async operation, nor one completing with `(exception_ptr, T)`).

## 4. Lifetime rules

- Coroutine parameters are copied into the frame when passed by value; a reference
  parameter keeps pointing at the caller's object, which is gone after the first
  suspension whenever the task is spawned, stored, or awaited after the caller's
  statement ended. Take coroutine parameters by value (`std::string`, `json::value`,
  `std::shared_ptr<State>`), never `const T&` / `std::string_view` / `std::span`.
- A coroutine lambda keeps its captures in the lambda object, not in the frame.
  `[x]() -> rt::Task<void> {...}()` leaves the captures in a temporary destroyed at the
  end of the statement. Wrap it: `rt::async(lambda)` owns the lambda for the life of
  the coroutine. `rt::withFinally`, `rt::forAwait`, `then`/`catch_`/`finally_`, stream
  algorithms and timers own the callables you pass them.
- A member coroutine that captures `this` needs the object to outlive the task:
  capture `shared_from_this()` (or a `shared_ptr` to the state) when the task can
  outlive the caller.
- Callbacks are stored (timers, listeners, `then`, stream algorithms): capture by value
  or `shared_ptr`, never references to locals of a coroutine that may finish first.
- Cycles: a timer or listener whose callback captures its own handle is released when
  it fires (timeouts), is cleared, or the signal aborts. A stream whose algorithms
  capture its controller is released when it closes, errors or is cancelled.
- A promise nobody can settle keeps the coroutines awaiting it alive until the loop is
  destroyed (JS would garbage-collect them). Destroying a `rt::Loop` destroys every
  coroutine still suspended on it without resuming it: scope guards run, `catch`
  blocks do not. Everything created on a loop (promises, timers, IO objects,
  generators) must be gone before the loop itself; the default loop lives forever.
- `rt::AbortSignal::any` keeps a dependent signal that has listeners alive while a
  source can still abort it (JS does the same); one without listeners lives only
  while you hold it.

## 5. Ordering guarantees

The loop. Each turn first fires the timers due at the start of the turn (deadline
order, equal deadlines in creation order), then runs up to 256 ready IO/posted
handlers. Every macrotask (one timer callback, one handler) is followed by a microtask
checkpoint: the FIFO microtask queue is drained, then promises still rejected without
a handler are reported (section 6). A timer created during a turn never fires in the
same turn. With nothing ready, the loop sleeps until the next timer or IO completion,
and ends when no referenced timer, pending IO operation or ready handler remains.
There is no `unref()` for IO: every pending Asio operation (a socket read, an
`asio::signal_set` wait) keeps `Loop::run` alive, whereas Bun's signal listeners and
idle keep-alive sockets do not; cancel such waits when the program should end.
Where Bun is ambiguous (a due timer and a ready IO completion at the same instant) the
C++ runs the timer first; the TS cannot depend on either order.

Exact (verified against Bun with the same scenario; see the tests):

| construct                                                     | settles / resumes                                                                                                                                                                                                                                                                                                                |
| ------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `co_await p.wait()`                                           | one turn after `p` settles (also when already settled)                                                                                                                                                                                                                                                                           |
| `p.then(f)`, `p.catch_(g)`                                    | `f`/`g` one turn after `p`; the derived promise in the turn `f` returns a value                                                                                                                                                                                                                                                  |
| callback returns a Promise or Task, `resolve(promise)`        | derived promise two turns after the returned one                                                                                                                                                                                                                                                                                 |
| `p.finally_(h)`                                               | Bun: same turn as a synchronous `h`, one turn after an async `h` (the spec takes two more)                                                                                                                                                                                                                                       |
| `rt::all / allSettled / race / any`                           | one turn after the deciding input (`co_await ... .wait()` one more)                                                                                                                                                                                                                                                              |
| `rt::microtask()`, `rt::queueMicrotask(f)`                    | next turn, FIFO                                                                                                                                                                                                                                                                                                                  |
| `rt::Promise(f())`, `rt::spawn(f())`, `newPromise` executor   | run synchronously up to the first suspension                                                                                                                                                                                                                                                                                     |
| generator `co_await yield(v)`                                 | the request settles one turn after the yield; `next()` on a suspended body resumes it inline                                                                                                                                                                                                                                     |
| generator `return_()`                                         | on a suspended body: unwinds one turn later (the consumer's previous continuation runs first); before start/after end: settles one turn later                                                                                                                                                                                    |
| generator `co_return value` / end / throw                     | one turn / at once / at once                                                                                                                                                                                                                                                                                                     |
| stream `read()`                                               | one turn after the chunk or close is available; pull runs inside `read()` for highWaterMark 0, one turn after start otherwise                                                                                                                                                                                                    |
| `reader.readPromise()`                                        | settled when the read is (at once for a queued chunk, the close or the error; with the enqueue/close/error otherwise): `.then` one turn later, a race over it two (bun: ready chunk vs rejected abort: read wins, race settles turn 1, its `.then` turn 2; pending read, enqueue at turn 3: `.then` turn 4, race `.then` turn 5) |
| `reader.cancelPromise(r)` with a synchronous cancel algorithm | settles turn 1 (the algorithm runs inside the call): `.then` / `await` turn 2                                                                                                                                                                                                                                                    |
| `controller.abort()`                                          | listeners run synchronously, in registration order; `AbortSignal.any` dependents are aborted before and dispatched after the source                                                                                                                                                                                              |

Not exact — insert a yield where the TS depends on it:

- `co_await f()` on a Task resumes the caller the moment `f` returns; JS `await f()`
  takes one more turn. `co_await rt::Promise(f()).wait()`, or `co_await f()` followed
  by `co_await rt::microtask()`, is exact.
- An async function that `return`s a promise settles two turns after it in JS:
  port `return p` as `co_await rt::microtask(); auto v = co_await p.wait(); co_return v;`.
- `tee()`: branches close or error one turn later than the spec's synchronous
  close/error steps; chunk delivery and pull counts match Bun.
- `cancel()` / `cancelPromise()` when the cancel algorithm is async: the result settles one turn
  after the algorithm's promise, Bun's two (bun: algorithm done at turn 1, `.then` at turn 4; rt
  turn 3). Synchronous cancel algorithms (every port so far) are exact.
- `rt::abortable` and `rt::sleep(ms, signal)` react to an abort through an ordinary
  listener, so they run in registration order relative to user listeners (the spec
  runs its own abort algorithms before all listeners). Both only settle promises, so
  user code sees the same order.

Timers (Bun's rules): delays are truncated to whole milliseconds; NaN, negative, zero
and values above 2^31 - 1 become 1 ms, printing Bun's `TimeoutNaNWarning` /
`TimeoutNegativeWarning` / `TimeoutOverflowWarning` once per kind. Ids are 1, 2, 3...
per process. An interval's next run is due `delay` after the turn that ran it began.
`hasRef()` is true while the timer is armed (also inside its own callback). Clearing
works on either kind, on fired or cleared handles, and inside the callback.
`AbortSignal.timeout(ms)` uses an unreferenced timer (it never keeps the process
alive) and throws `TypeError("Value -1 is outside the range [0, 9007199254740991]")`
for negative, NaN or infinite `ms`.

## 6. Unhandled rejections and uncaught exceptions

The gateway installs no `unhandledRejection`/`uncaughtException` handler. Bun's
behaviour then depends on whether a top-level `await` is pending:

- none pending (module evaluation, scripts): an unhandled rejection or an exception
  escaping a timer/listener/`queueMicrotask` callback ends the process with status 1
  (during module evaluation at once, later after about one more macrotask);
- pending (serve.ts awaits `server.finished` for the server's whole life): Bun prints
  the error and keeps running; `process.exit(0)` at shutdown still exits 0.

`rt::UncaughtPolicy::raise` (default, and what tests use) throws `rt::UncaughtError` out
of `rt::blockOn` / `Loop::run` at the end of the checkpoint that found it.
`rt::UncaughtPolicy::report` prints `UncaughtError::report()` (Bun's first line:
`error: msg` for a plain Error, `Name: msg` otherwise, `error: String(v)` for a
primitive, `error` for an object) to stderr, counts it (`Loop::uncaughtCount()`), and
continues: serve's `main` sets it. A rejection handled later in the same checkpoint
is not reported; one handled in a later turn already was. Bun's source excerpt and
stack trace are not reproduced.

## 7. Tests

`ASYNC_TEST("name") { co_await ...; }` (include `uos/rt/test_async.hpp`) runs the body
with `rt::blockOn` on a fresh `rt::Loop` driven by a fresh `rt::FakeClock`
(`rt::test::clock()`): Date.now() starts at the real time, performance.now() at 0, and
an idle loop jumps straight to the next timer, so `co_await rt::sleep(5000)` returns at
once with earlier timers fired in order. While an Asio IO operation is pending the fake
clock does not jump: the next timer fires only after that much real time passed, so a
local socket round trip is not overtaken by a fake timeout. `clock().advance(ms)` is
the TS busy-wait. `EXPECT_REJECTS(task, Type)`, `EXPECT_REJECTS_MESSAGE(task, Type, msg)`
and `EXPECT_REJECTS_REASON(task, reason)` await a task (rule 1 applies to `task`).
`assert.throws(() => expr, /pattern/)` is `EXPECT_THROWS_MATCHING(expr, "pattern")` (uos/rt/test.hpp;
an `rt::Regex` for flags): like node:assert it matches String(thrown), so `"^TypeError: bad"`
sees the name and any thrown value counts. A test file's own `expectThrowsMatching` helper
and identical `#define` keep working and take precedence.
An unhandled rejection fails the test. The test ends when the body completes; its
loop and pending timers are then destroyed.

The TS tests' time idioms map one to one:

- `const realNow = Date.now; Date.now = () => clock; ...; Date.now = realNow;` ->
  `auto realNow = rt::globalDateNow(); rt::globalDateNow() = [&] { return clock; }; ...; rt::globalDateNow() = realNow;`
- `globalThis.setTimeout = (handler, ms) => ...` / `globalThis.clearTimeout = ...` ->
  `rt::globalSetTimeout()` / `rt::globalClearTimeout()` (same save/replace/restore
  shape; `setInterval` and `AbortSignal.timeout` bypass them, as in Bun).
- injected `now: () => number` -> a `std::function<double()>` parameter.
- `await Bun.sleep(ms)` in a test -> `co_await rt::sleep(ms)` (instant in fake time).

`ASYNC_TEST` restores the three globals after each test.

## 8. Not provided here

- `pipeThrough(new CompressionStream("gzip"))` / `DecompressionStream` as stream objects:
  use `uos::rt::gzip` instead (see `gzip.md`). `TransformStream`, `pipeTo`,
  `TextDecoderStream` are unused by the TS.
- Stream misuse errors carry Bun's name and message but not its
  `code: "ERR_INVALID_STATE"` property (they leave `rt::JsError::code` unset).
- `Timeout::refresh()` does not re-arm a timeout that already fired (the TS never calls
  `refresh`).
- Async generator `yield*` forwards values and `return()` but is not turn-exact.
