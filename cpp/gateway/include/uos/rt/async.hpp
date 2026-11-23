#pragma once
// JavaScript's promise model on one Asio io_context thread. A TS `async` function
// returns `rt::Task<T>`; a promise held as a value is `rt::Promise<T>`. The loop
// runs one macrotask (timer, IO completion, posted handler) at a time and drains
// the microtask queue after each one, like Bun. See docs/rt/async.md.
#include "uos/rt/error.hpp"
#include "uos/rt/prelude.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace boost::asio {
  class io_context;
}

namespace uos::rt {
  template <class T = void> using Task = boost::asio::awaitable<T>;
  using Executor = boost::asio::any_io_executor;

  // Any JavaScript value that can be thrown: a rejection, abort or cancel reason.
  // Errors are C++ exceptions deriving from rt::JsError; other values are wrapped
  // in rt::ThrownValue. A null Reason is `undefined`.
  using Reason = std::exception_ptr;

  // `throw value` / `reject(value)` / `controller.abort(value)` for a value that is
  // not an Error instance. std::nullopt is `undefined`.
  class ThrownValue : public std::exception {
  public:
    std::optional<json::value> value;
    explicit ThrownValue(std::optional<json::value> value_);
    const char* what() const noexcept override { return text_.c_str(); }

  private:
    std::string text_;
  };

  // `Promise.any` with every input rejected (Bun's message is empty).
  struct AggregateError : JsError {
    std::vector<Reason> errors;
    AggregateError(std::vector<Reason> errors_, std::string message_);
  };

  // An unhandled rejection (a promise still rejected without a handler at the
  // end of a microtask checkpoint) or an exception escaping a timer callback,
  // event listener or queueMicrotask callback.
  class UncaughtError : public std::exception {
  public:
    Reason reason;
    bool rejection;
    UncaughtError(Reason reason_, bool rejection_);
    const char* what() const noexcept override { return text_.c_str(); }
    // The first line Bun prints: "error: message" for a plain Error with a
    // message, "Name: message" for other errors, "error: String(value)" for a
    // primitive value and "error" for an object.
    std::string report() const;

  private:
    std::string text_;
  };

  // What the loop does with an UncaughtError.
  enum class UncaughtPolicy : std::uint8_t {
    // Bun with no top-level await pending: the process ends with status 1. The
    // loop finishes the current microtask checkpoint, then Loop::run / blockOn
    // throw the UncaughtError. The default, and what tests want.
    raise,
    // Bun while a top-level await is pending (serve.ts awaits server.finished):
    // the report is printed to stderr and the loop keeps running.
    report,
  };

  Reason thrown(std::optional<json::value> value);
  template <class E>
    requires std::derived_from<std::decay_t<E>, std::exception>
  Reason makeReason(E&& error) {
    return std::make_exception_ptr(std::forward<E>(error));
  }
  // `throw reason`. Rethrowing `undefined` throws ThrownValue(std::nullopt).
  [[noreturn]] void rethrow(const Reason& reason);
  // `reason instanceof Error`: any std::exception except ThrownValue.
  bool isError(const Reason& reason);
  // `reason.name` / `reason.message` when the reason is an Error.
  std::optional<std::string> errorName(const Reason& reason);
  std::optional<std::string> errorMessage(const Reason& reason);
  // The thrown non-Error value, or std::nullopt when the reason is an Error or undefined.
  std::optional<json::value> thrownValue(const Reason& reason);
  // `String(reason)`.
  std::string reasonToString(const Reason& reason);

  class Clock;
  class Loop;

  namespace detail {
    // A move-only one-shot callable.
    class Job {
    public:
      Job() = default;
      template <class F>
        requires(!std::is_same_v<std::decay_t<F>, Job> && std::is_invocable_v<std::decay_t<F>&>)
      Job(F&& function) : impl_(std::make_unique<Impl<std::decay_t<F>>>(std::forward<F>(function))) {}
      explicit operator bool() const noexcept { return impl_ != nullptr; }
      void operator()() {
        auto impl = std::move(impl_);
        impl->run();
      }

    private:
      struct Base {
        virtual ~Base() = default;
        virtual void run() = 0;
      };
      template <class F> struct Impl final : Base {
        F function;
        explicit Impl(F&& value) : function(std::move(value)) {}
        explicit Impl(const F& value) : function(value) {}
        void run() override { function(); }
      };
      std::unique_ptr<Base> impl_;
    };

    struct PromiseCore;
    // A loop's pending promises that have reactions waiting: destroying the loop
    // drops those reactions, and with them the coroutines suspended on them
    // (JS would garbage-collect a promise nobody can settle any more).
    struct WaitingList {
      PromiseCore* head = nullptr;
    };

    struct PromiseCore : std::enable_shared_from_this<PromiseCore> {
      enum class Status : std::uint8_t { pending, fulfilled, rejected };
      Status status = Status::pending;
      // [[PromiseIsHandled]]: a reaction was registered.
      bool handled = false;
      // The resolving functions were used (resolve(promise) locks a still
      // pending promise into following another one).
      bool locked = false;
      Reason error;
      std::vector<Job> reactions;
      WaitingList* waitingList = nullptr;
      PromiseCore* previousWaiting = nullptr;
      PromiseCore* nextWaiting = nullptr;

      PromiseCore() = default;
      PromiseCore(const PromiseCore&) = delete;
      PromiseCore& operator=(const PromiseCore&) = delete;
      virtual ~PromiseCore();
      bool pending() const noexcept { return status == Status::pending; }
      // Runs `reaction` in a microtask once settled (queued at once when
      // already settled) and marks the promise handled.
      void react(Job reaction);
      // Settles: queues every reaction as a microtask in registration order.
      void settle(Status outcome);
      // Rejects a pending promise; no-op once settled.
      void reject(Reason reason);
    };

    template <class T> struct PromiseState final : PromiseCore {
      std::optional<T> value;
      bool fulfill(T result) {
        if (!pending()) return false;
        value.emplace(std::move(result));
        settle(Status::fulfilled);
        return true;
      }
    };
    template <> struct PromiseState<void> final : PromiseCore {
      bool fulfill() {
        if (!pending()) return false;
        settle(Status::fulfilled);
        return true;
      }
    };

    Loop& currentLoop();
    // Starts `task` now: it runs synchronously up to its first suspension when
    // `loop` is this thread's current loop, otherwise at that loop's next turn.
    void start(Loop& loop, Task<void> task);
    void queueMicrotask(Loop& loop, Job job);
    // Routes an exception escaping a callback to the current loop's policy.
    void reportUncaught(Reason reason, bool rejection);
    // Suspends until `core` settles, then resumes one microtask later.
    Task<void> awaitSettled(std::shared_ptr<PromiseCore> core);
    // Starts `task` on `loop` and runs the loop until `core` settles
    // (std::logic_error when the loop is already running, or runs out of work
    // first).
    void runUntilSettled(Loop& loop, Task<void> task, PromiseCore& core);

    // GCC 12 rule (see docs/rt/async.md): a co_await result is bound to a named
    // variable before it is passed to a function.
    template <class T> Task<void> runInto(std::shared_ptr<PromiseState<T>> state, Task<T> task) {
      try {
        if constexpr (std::is_void_v<T>) {
          co_await std::move(task);
          state->fulfill();
        } else {
          T value = co_await std::move(task);
          state->fulfill(std::move(value));
        }
      } catch (...) {
        state->reject(std::current_exception());
      }
    }

    template <class T> Task<T> awaitOwned(std::shared_ptr<PromiseState<T>> state) {
      auto ready = awaitSettled(state);
      co_await std::move(ready);
      if (state->status == PromiseCore::Status::rejected) rethrow(state->error);
      if constexpr (!std::is_void_v<T>) co_return std::move(*state->value);
    }

    // A Task over a promise rejected now: like the JS promise it stands for, it
    // is an unhandled rejection unless awaited within the current checkpoint.
    template <class T> Task<T> rejected(Reason reason) {
      auto state = std::make_shared<PromiseState<T>>();
      state->reject(std::move(reason));
      return awaitOwned(std::move(state));
    }
  }

  // The event loop: one io_context, the microtask queue, the timer list and the
  // clock that drives them. Run it with rt::blockOn or Loop::run. Everything
  // created on a loop (promises, timers, coroutines, IO objects) must be gone
  // before the loop is destroyed.
  class Loop {
  public:
    Loop();
    explicit Loop(Clock& clock);
    ~Loop();
    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    boost::asio::io_context& context() noexcept;
    // Executor for IO objects and co_spawn: on the loop's thread it runs a
    // dispatched handler inline, so coroutines start and resume synchronously.
    Executor executor() noexcept;
    Clock& clock() noexcept;
    // Runs until no ready handler, pending IO operation, microtask or referenced
    // timer remains (a fake clock jumps to each due timer), like a process whose
    // event loop drains. Throws UncaughtError under UncaughtPolicy::raise.
    void run();
    // Timers created and neither fired (for timeouts) nor cleared, ref'd or not.
    std::size_t timerCount() const noexcept;
    // Microtasks waiting to run.
    std::size_t microtaskCount() const noexcept;
    void setUncaughtPolicy(UncaughtPolicy policy) noexcept;
    UncaughtPolicy uncaughtPolicy() const noexcept;
    // Errors reported so far under UncaughtPolicy::report (Bun's exit status
    // would be 1 if this is not zero and the process ended without process.exit).
    std::size_t uncaughtCount() const noexcept;

    struct Impl;
    Impl& impl() noexcept { return *impl_; }

  private:
    std::unique_ptr<Impl> impl_;
  };

  // The loop of this thread's innermost LoopScope, else the process-wide default
  // loop (system clock, never destroyed).
  Loop& loop();
  Executor executor();

  // Makes `loop` the current loop of this thread for the scope.
  class LoopScope {
  public:
    explicit LoopScope(Loop& loop_);
    ~LoopScope();
    LoopScope(const LoopScope&) = delete;
    LoopScope& operator=(const LoopScope&) = delete;

  private:
    Loop* previous_;
  };

  template <class T = void> class Promise;
  template <class T = void> class Resolve;
  class Reject;

  namespace detail {
    template <class T> struct IsPromise : std::false_type {};
    template <class T> struct IsPromise<Promise<T>> : std::true_type {};
    template <class T> struct IsTask : std::false_type {};
    template <class T> struct IsTask<Task<T>> : std::true_type {};
    // What a then/catch callback result settles the derived promise with.
    template <class R> struct Settles {
      using type = R;
    };
    template <class R> struct Settles<Task<R>> {
      using type = R;
    };
    template <class R> struct Settles<Promise<R>> {
      using type = R;
    };
    template <class T> using Slot = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
  }

  // A JavaScript promise: shared state settled once, awaited by any number of
  // consumers, before or after it settles. Copies refer to the same promise.
  // `await promise` is `co_await promise.wait()`.
  template <class T> class Promise {
  public:
    using value_type = T;

    // A pending promise (settle it through rt::withResolvers or rt::newPromise).
    Promise() : state_(std::make_shared<detail::PromiseState<T>>()) {}
    // Calls the async function now: `task` runs synchronously up to its first
    // suspension, exactly where JS would evaluate `f()`.
    Promise(Task<T> task) : Promise() { detail::start(detail::currentLoop(), detail::runInto(state_, std::move(task))); }

    // `Promise.resolve(value)`
    template <class... A>
      requires(sizeof...(A) == (std::is_void_v<T> ? 0 : 1))
    static Promise resolve(A&&... value) {
      Promise promise;
      promise.state_->fulfill(std::forward<A>(value)...);
      return promise;
    }
    // `Promise.reject(reason)`
    static Promise reject(Reason reason) {
      Promise promise;
      promise.state_->reject(std::move(reason));
      return promise;
    }

    // `await promise`: resumes one microtask after settlement and rethrows a
    // rejection. Each await yields its own copy of the value.
    [[nodiscard]] Task<T> wait() const { return awaitState(state_); }
    bool pending() const noexcept { return state_->status == detail::PromiseCore::Status::pending; }
    bool fulfilled() const noexcept { return state_->status == detail::PromiseCore::Status::fulfilled; }
    bool rejected() const noexcept { return state_->status == detail::PromiseCore::Status::rejected; }
    bool settled() const noexcept { return !pending(); }
    // The fulfillment value (fulfilled promises only).
    template <class U = T>
      requires(!std::is_void_v<U>)
    const U& value() const {
      return *state_->value;
    }
    // The rejection reason (rejected promises only).
    const Reason& reason() const noexcept { return state_->error; }

    // `promise.then(onFulfilled)`; the callback may return a value, void, a Task or a Promise.
    template <class F> auto then(F onFulfilled) const;
    // `promise.then(onFulfilled, onRejected)`; onRejected takes the Reason.
    template <class F, class G> auto then(F onFulfilled, G onRejected) const;
    // `promise.catch(onRejected)`. A callback returning void yields Promise<void>
    // (JS: the value is `undefined` after a rejection).
    template <class G> auto catch_(G onRejected) const;
    // `promise.finally(onFinally)`; onFinally returns void or Task<void>.
    template <class F> Promise<T> finally_(F onFinally) const;

    // `p1 === p2`: copies are the same promise; every `then`/`catch_`/`finally_` is a new one.
    friend bool operator==(const Promise& left, const Promise& right) noexcept { return left.state_ == right.state_; }
    // The identity behind `==`, for rt::WeakMap / rt::WeakSet keys (`WeakMap<Promise<void>,
    // T>`): the shared state, alive while a copy, a resolver or an awaiting task holds it.
    const void* identity() const noexcept { return state_.get(); }
    std::weak_ptr<const void> weakIdentity() const noexcept { return state_; }

    const std::shared_ptr<detail::PromiseState<T>>& state() const noexcept { return state_; }

  private:
    static Task<T> awaitState(std::shared_ptr<detail::PromiseState<T>> state) {
      auto ready = detail::awaitSettled(state);
      co_await std::move(ready);
      if (state->status == detail::PromiseCore::Status::rejected) rethrow(state->error);
      if constexpr (!std::is_void_v<T>) co_return *state->value;
    }
    template <class U> friend class Promise;
    std::shared_ptr<detail::PromiseState<T>> state_;
  };

  template <class T> Promise(Task<T>) -> Promise<T>;

  namespace detail {
    template <class T> void copyOutcome(const PromiseState<T>& source, PromiseState<T>& target) {
      if (source.status == PromiseCore::Status::rejected) target.reject(source.error);
      else if constexpr (std::is_void_v<T>) target.fulfill();
      else target.fulfill(*source.value);
    }
    // `resolve(promise)` with a native promise: one job later
    // (NewPromiseResolveThenableJob) `promise.then(resolve, reject)` is
    // registered, so `target` settles two turns after `source`.
    template <class T> void adopt(std::shared_ptr<PromiseState<T>> target, std::shared_ptr<PromiseState<T>> source) {
      queueMicrotask(currentLoop(), [target = std::move(target), source = std::move(source)]() mutable {
        auto from = source;
        source->react([from = std::move(from), to = std::move(target)] { copyOutcome(*from, *to); });
      });
    }
  }

  // The `resolve` function of `new Promise((resolve, reject) => ...)`. Copyable;
  // calls after the promise settled or was resolved with a promise are ignored.
  // Resolving with a promise adopts its outcome (two turns later, as in JS).
  template <class T> class Resolve {
  public:
    explicit Resolve(std::shared_ptr<detail::PromiseState<T>> state_) : state(std::move(state_)) {}
    template <class... A>
      requires(sizeof...(A) == (std::is_void_v<T> ? 0 : 1) && (!detail::IsPromise<std::decay_t<A>>::value && ...))
    void operator()(A&&... value) const {
      if (!state->pending() || state->locked) return;
      state->locked = true;
      state->fulfill(std::forward<A>(value)...);
    }
    void operator()(const Promise<T>& other) const {
      if (!state->pending() || state->locked) return;
      state->locked = true;
      detail::adopt(state, other.state());
    }

  private:
    std::shared_ptr<detail::PromiseState<T>> state;
  };

  // The `reject` function of `new Promise((resolve, reject) => ...)`.
  class Reject {
  public:
    explicit Reject(std::shared_ptr<detail::PromiseCore> state_) : state(std::move(state_)) {}
    void operator()(Reason reason = nullptr) const {
      if (!state->pending() || state->locked) return;
      state->locked = true;
      state->reject(std::move(reason));
    }

  private:
    std::shared_ptr<detail::PromiseCore> state;
  };

  // `Promise.withResolvers<T>()`
  template <class T = void> struct Resolvers {
    Promise<T> promise;
    Resolve<T> resolve;
    Reject reject;
  };
  template <class T = void> Resolvers<T> withResolvers() {
    Promise<T> promise;
    return Resolvers<T>{promise, Resolve<T>(promise.state()), Reject(promise.state())};
  }

  // `new Promise<T>((resolve, reject) => { ... })`: the executor runs now; if it
  // throws, the promise rejects with the exception.
  template <class T = void, class F> Promise<T> newPromise(F&& executor) {
    auto resolvers = withResolvers<T>();
    try {
      std::forward<F>(executor)(resolvers.resolve, resolvers.reject);
    } catch (...) {
      resolvers.reject(std::current_exception());
    }
    return resolvers.promise;
  }

  // An un-awaited call, `void f()`: runs now up to its first suspension. A
  // rejection is unhandled (UncaughtError at the end of that microtask checkpoint).
  template <class T> void spawn(Task<T> task) { (void)Promise<T>(std::move(task)); }
  // `void f().catch(onRejected)`: onRejected(Reason) runs instead of the unhandled-rejection report.
  template <class T, class G> void spawn(Task<T> task, G onRejected) { (void)Promise<T>(std::move(task)).catch_(std::move(onRejected)); }
  // The `.catch(() => {})` callback.
  inline void ignoreRejection(const Reason&) {}

  namespace detail {
    template <class F, class R = std::invoke_result_t<F&>> R callHeld(std::shared_ptr<F> holder) {
      if constexpr (std::is_void_v<typename Settles<R>::type>) {
        co_await (*holder)();
      } else {
        auto value = co_await (*holder)();
        co_return value;
      }
    }
  }
  // `(async () => { ... })`: a Task that owns the coroutine lambda, so its
  // captures live as long as the coroutine. Calling a capturing coroutine lambda
  // directly (`[x]() -> Task<void> {...}()`) leaves the captures in a temporary
  // destroyed at the first suspension.
  template <class F>
    requires detail::IsTask<std::invoke_result_t<F&>>::value
  [[nodiscard]] std::invoke_result_t<F&> async(F function) {
    return detail::callHeld(std::make_shared<F>(std::move(function)));
  }

  // `await null` / `await Promise.resolve()`: yields exactly one microtask turn.
  [[nodiscard]] Task<void> microtask();

  // `return Promise.resolve(value)` where the C++ signature is a Task (an override, a
  // callback slot): completes with `value` as soon as it is awaited, without suspending.
  // Like every Task it skips the turn JS `await` takes (section 5 of docs/rt/async.md);
  // `rt::Promise<T>::resolve(value).wait()` takes it. Pass a std::string, not a literal.
  template <class T> [[nodiscard]] Task<T> fulfilledTask(T value) { co_return std::move(value); }
  // `return Promise.resolve()` as a Task<void>.
  [[nodiscard]] inline Task<void> fulfilledTask() { co_return; }
  // `queueMicrotask(callback)`. An exception from the callback is uncaught.
  void queueMicrotask(std::function<void()> callback);

  // One `Promise.allSettled` entry: status is "fulfilled" or "rejected".
  template <class T> struct SettledResult {
    std::string status;
    std::optional<detail::Slot<T>> value;
    Reason reason;
  };

  namespace detail {
    template <class T> Promise<T> asPromise(Promise<T> promise) { return promise; }
    template <class T> Promise<T> asPromise(Task<T> task) { return Promise<T>(std::move(task)); }

    template <class T> Slot<T> valueOf(const PromiseState<T>& state) {
      if constexpr (std::is_void_v<T>) return std::monostate{};
      else return *state.value;
    }

    template <class T> Promise<std::vector<T>> allOf(std::vector<Promise<T>> promises);
    template <class T> Promise<std::vector<SettledResult<T>>> allSettledOf(std::vector<Promise<T>> promises);
    template <class T> Promise<T> raceOf(std::vector<Promise<T>> promises);
    template <class T> Promise<T> anyOf(std::vector<Promise<T>> promises);
  }

  // The combinators register their reactions when called (like JS, which marks
  // the inputs handled at once) and return the combined promise, so
  // `co_await rt::all(...).wait()` and `rt::all(...).then(...)` take the turns JS takes.
  // `Promise.all(promises)`: rejects with the first rejection; the others keep running.
  template <class T> [[nodiscard]] Promise<std::vector<T>> all(std::vector<Promise<T>> promises) { return detail::allOf(std::move(promises)); }
  [[nodiscard]] Promise<void> all(std::vector<Promise<void>> promises);
  // `Promise.all([a, b, ...])` over different types: Promise<std::tuple<...>>;
  // void entries become std::monostate.
  template <class... P>
    requires(sizeof...(P) > 1)
  [[nodiscard]] auto all(P&&... promises);
  // `Promise.allSettled(promises)`
  template <class T> [[nodiscard]] Promise<std::vector<SettledResult<T>>> allSettled(std::vector<Promise<T>> promises) {
    return detail::allSettledOf(std::move(promises));
  }
  // `Promise.race(promises)`: settles like the first input to settle; the losers keep running.
  template <class T> [[nodiscard]] Promise<T> race(std::vector<Promise<T>> promises) { return detail::raceOf(std::move(promises)); }
  template <class T, class... Rest> [[nodiscard]] Promise<T> race(Promise<T> first, Rest&&... rest) {
    return detail::raceOf(std::vector<Promise<T>>{std::move(first), detail::asPromise(std::forward<Rest>(rest))...});
  }
  template <class T, class... Rest> [[nodiscard]] Promise<T> race(Task<T> first, Rest&&... rest) {
    return race(Promise<T>(std::move(first)), std::forward<Rest>(rest)...);
  }
  // `Promise.any(promises)`: AggregateError (message "") when every input rejects.
  template <class T> [[nodiscard]] Promise<T> any(std::vector<Promise<T>> promises) { return detail::anyOf(std::move(promises)); }

  // `try { await body } finally { await onFinally() }`: runs onFinally after body
  // settles (on success, exception, or an early co_return inside body); an
  // exception from onFinally replaces the body's outcome.
  template <class T, class F> [[nodiscard]] Task<T> withFinally(Task<T> body, F onFinally);

  // Runs a callback when the scope exits: on return, on exception and when a
  // suspended coroutine frame is destroyed. Destructors cannot co_await: use
  // rt::withFinally (or the catch-store-rethrow idiom) for asynchronous cleanup.
  template <class F> class ScopeExit {
  public:
    explicit ScopeExit(F callback_) : callback(std::move(callback_)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() {
      if (armed) callback();
    }
    void release() noexcept { armed = false; }

  private:
    F callback;
    bool armed = true;
  };
  template <class F> [[nodiscard]] ScopeExit<F> scopeExit(F callback) { return ScopeExit<F>(std::move(callback)); }

  // Runs `task` to completion on `loop` (the current loop by default) and returns
  // its value or rethrows its rejection: the entry point for main() and tests.
  // Never call it inside a running loop. Throws UncaughtError (policy raise) and
  // std::logic_error when the loop runs out of work with the task still pending.
  template <class T> T blockOn(Loop& target, Task<T> task) {
    auto state = std::make_shared<detail::PromiseState<T>>();
    state->handled = true;
    detail::runUntilSettled(target, detail::runInto(state, std::move(task)), *state);
    if (state->status == detail::PromiseCore::Status::rejected) rethrow(state->error);
    if constexpr (!std::is_void_v<T>) return std::move(*state->value);
  }
  template <class T> T blockOn(Task<T> task) { return blockOn(loop(), std::move(task)); }

  // ---- async generators (`async function*`)

  // `{ done, value }` of an async iterator step.
  template <class T> struct IteratorResult {
    bool done = false;
    std::optional<T> value;
  };

  // Thrown at a suspended `co_await yield(...)` when the consumer calls
  // `return()`; it unwinds the body the way JS runs its `finally` blocks.
  // Deliberately not a std::exception. A `catch (...)` that spans a yield must
  // rethrow it (JS `catch` never sees a return).
  struct GeneratorReturn {};

  namespace detail {
    template <class T> struct GeneratorCore;
  }

  // The generator body's `yield`: `co_await yield(value)` hands one value to the
  // consumer and suspends until the next `next()` / `return()`.
  template <class T> class Yield {
  public:
    explicit Yield(std::weak_ptr<detail::GeneratorCore<T>> core_) : core(std::move(core_)) {}
    [[nodiscard]] Task<void> operator()(T value) const;
    // `yield* inner`: forwards every value of `inner` and returns its return value.
    template <class G> [[nodiscard]] auto from(G inner) const;

  private:
    std::weak_ptr<detail::GeneratorCore<T>> core;
  };

  // An async generator object. Nothing runs until the first next(). next() and
  // return_() take effect when called (the body runs synchronously up to its
  // next suspension) and queue like JS: a return() issued while the body is
  // running (for example parked in an upstream read) waits for it to yield.
  // Turns match JS: a yield completes its request one turn after the yield
  // statement; return() resumes a suspended body one turn later (so the
  // consumer's continuation of the previous step runs before the body's
  // cleanup); `co_return value` takes one turn; falling off the end or
  // throwing completes at once. Dropping every handle of a suspended generator
  // destroys its frame without running further code, as JS garbage-collects it.
  template <class T, class TReturn = void> class AsyncGenerator {
  public:
    using Body = std::function<Task<TReturn>(Yield<T>&&)>;
    explicit AsyncGenerator(Body body);
    // `iterator.next()`
    [[nodiscard]] Task<IteratorResult<T>> next() const;
    // `iterator.return(value)`: the value is not retained (no caller reads it).
    [[nodiscard]] Task<IteratorResult<T>> return_() const;
    // The body's return value once it completed normally.
    template <class U = TReturn>
      requires(!std::is_void_v<U>)
    std::optional<U> returnValue() const;
    bool done() const noexcept;

    std::shared_ptr<detail::GeneratorCore<T>> core() const noexcept { return core_; }

  private:
    std::shared_ptr<detail::GeneratorCore<T>> core_;
    std::shared_ptr<std::optional<detail::Slot<TReturn>>> returned_;
  };

  // `for await (const value of iterator) { body }`: body(value) returns
  // Task<void>, or Task<bool> where false is `break`. On `break` the iterator's
  // return() is awaited; when body throws, return() is awaited and the body's
  // exception propagates (an error from return() is then ignored), as in JS.
  template <class T, class R, class F> [[nodiscard]] Task<void> forAwait(AsyncGenerator<T, R> iterator, F body);

  // ---- implementation

  namespace detail {
    // A single-waiter wake-up slot: sleepOn() suspends until wake() resumes the
    // waiter inline. A wake() with nobody waiting is remembered. Destroying the
    // slot with a waiter destroys the waiting coroutine without resuming it (the
    // sleeper holds only a weak reference, so the slot's owner decides).
    struct Waker {
      Job waiter;
      bool woken = false;
      void wake() {
        if (!waiter) {
          woken = true;
          return;
        }
        auto job = std::move(waiter);
        job();
      }
    };
    Task<void> sleepOn(std::weak_ptr<Waker> waker);
    // Suspends forever: the coroutine is destroyed without resuming.
    Task<void> abandon();

    template <class T, class Owner> Task<T> keepAlive(Owner keep, Task<T> task) {
      (void)keep;
      if constexpr (std::is_void_v<T>) {
        co_await std::move(task);
      } else {
        T value = co_await std::move(task);
        co_return value;
      }
    }

    // Settles `target` with the result of `call()`: a value, void, a Task (an
    // async callback, whose promise `target` adopts) or a Promise (adopted).
    // A callback Task may borrow both the callable and the source outcome.
    template <class U, class T, class Call>
    void settleWith(const std::shared_ptr<PromiseState<U>>& target, std::shared_ptr<void> keep, const std::shared_ptr<PromiseState<T>>& source, Call&& call) {
      using R = std::invoke_result_t<Call&>;
      try {
        if constexpr (std::is_void_v<R>) {
          static_assert(std::is_void_v<U>);
          call();
          target->fulfill();
        } else if constexpr (IsTask<R>::value) {
          auto inner = std::make_shared<PromiseState<U>>();
          auto task = call();
          auto owners = std::pair{std::move(keep), source};
          start(currentLoop(), keepAlive<void>(std::move(owners), runInto(inner, std::move(task))));
          adopt(target, std::move(inner));
        } else if constexpr (IsPromise<R>::value) {
          adopt(target, call().state());
        } else {
          target->fulfill(call());
        }
      } catch (...) {
        target->reject(std::current_exception());
      }
    }

    template <class T, class F> auto invokeWithValue(F& callback, const PromiseState<T>& state) {
      if constexpr (std::is_void_v<T>) return callback();
      else return callback(*state.value);
    }
    template <class T, class F> using FulfilledResult = decltype(invokeWithValue<T>(std::declval<F&>(), std::declval<const PromiseState<T>&>()));
  }

  template <class T> template <class F> auto Promise<T>::then(F onFulfilled) const {
    using U = typename detail::Settles<detail::FulfilledResult<T, F>>::type;
    Promise<U> out;
    auto source = state_;
    auto target = out.state_;
    auto callback = std::make_shared<F>(std::move(onFulfilled));
    source->react([source, target, callback] {
      if (source->status == detail::PromiseCore::Status::rejected) {
        target->reject(source->error);
        return;
      }
      detail::settleWith(target, callback, source, [&] { return detail::invokeWithValue<T>(*callback, *source); });
    });
    return out;
  }

  template <class T> template <class F, class G> auto Promise<T>::then(F onFulfilled, G onRejected) const {
    using U = typename detail::Settles<detail::FulfilledResult<T, F>>::type;
    static_assert(std::is_same_v<U, typename detail::Settles<std::invoke_result_t<G&, Reason>>::type>, "both callbacks must settle the same type");
    Promise<U> out;
    auto source = state_;
    auto target = out.state_;
    auto fulfilled = std::make_shared<F>(std::move(onFulfilled));
    auto rejected = std::make_shared<G>(std::move(onRejected));
    source->react([source, target, fulfilled, rejected] {
      if (source->status == detail::PromiseCore::Status::rejected) detail::settleWith(target, rejected, source, [&] { return (*rejected)(source->error); });
      else detail::settleWith(target, fulfilled, source, [&] { return detail::invokeWithValue<T>(*fulfilled, *source); });
    });
    return out;
  }

  template <class T> template <class G> auto Promise<T>::catch_(G onRejected) const {
    using U = typename detail::Settles<std::invoke_result_t<G&, Reason>>::type;
    static_assert(std::is_void_v<U> || std::is_same_v<U, T>, "catch_ must return void or the promise's value type");
    Promise<U> out;
    auto source = state_;
    auto target = out.state_;
    auto callback = std::make_shared<G>(std::move(onRejected));
    source->react([source, target, callback] {
      if (source->status == detail::PromiseCore::Status::rejected) {
        detail::settleWith(target, callback, source, [&] { return (*callback)(source->error); });
      } else if constexpr (std::is_void_v<U>) {
        target->fulfill();
      } else {
        target->fulfill(*source->value);
      }
    });
    return out;
  }

  // Bun (JavaScriptCore) settles the result in the turn a synchronous onFinally
  // runs, or one turn after the promise an async onFinally returns settles
  // (the specification takes two more turns in both cases).
  template <class T> template <class F> Promise<T> Promise<T>::finally_(F onFinally) const {
    using R = std::invoke_result_t<F&>;
    static_assert(std::is_void_v<R> || std::is_same_v<R, Task<void>>, "onFinally must return void or Task<void>");
    Promise<T> out;
    auto source = state_;
    auto target = out.state_;
    auto callback = std::make_shared<F>(std::move(onFinally));
    source->react([source, target, callback] {
      if constexpr (std::is_void_v<R>) {
        try {
          (*callback)();
        } catch (...) {
          target->reject(std::current_exception());
          return;
        }
        detail::copyOutcome(*source, *target);
      } else {
        auto after = std::make_shared<detail::PromiseState<void>>();
        try {
          detail::start(detail::currentLoop(), detail::keepAlive<void>(callback, detail::runInto(after, (*callback)())));
        } catch (...) {
          target->reject(std::current_exception());
          return;
        }
        after->react([after, source, target] {
          if (after->status == detail::PromiseCore::Status::rejected) target->reject(after->error);
          else detail::copyOutcome(*source, *target);
        });
      }
    });
    return out;
  }

  namespace detail {
    template <class T> Promise<std::vector<T>> allOf(std::vector<Promise<T>> promises) {
      Promise<std::vector<T>> combined;
      auto out = combined.state();
      auto values = std::make_shared<std::vector<std::optional<T>>>(promises.size());
      auto remaining = std::make_shared<std::size_t>(promises.size());
      if (promises.empty()) out->fulfill({});
      for (std::size_t index = 0; index < promises.size(); ++index) {
        auto source = promises[index].state();
        source->react([source, out, values, remaining, index] {
          if (source->status == PromiseCore::Status::rejected) {
            out->reject(source->error);
            return;
          }
          (*values)[index] = *source->value;
          if (--*remaining == 0 && out->pending()) {
            std::vector<T> result;
            result.reserve(values->size());
            for (auto& value : *values) result.push_back(std::move(*value));
            out->fulfill(std::move(result));
          }
        });
      }
      return combined;
    }

    template <class T> Promise<std::vector<SettledResult<T>>> allSettledOf(std::vector<Promise<T>> promises) {
      Promise<std::vector<SettledResult<T>>> combined;
      auto out = combined.state();
      auto results = std::make_shared<std::vector<SettledResult<T>>>(promises.size());
      auto remaining = std::make_shared<std::size_t>(promises.size());
      if (promises.empty()) out->fulfill({});
      for (std::size_t index = 0; index < promises.size(); ++index) {
        auto source = promises[index].state();
        source->react([source, out, results, remaining, index] {
          auto& entry = (*results)[index];
          if (source->status == PromiseCore::Status::rejected) {
            entry.status = "rejected";
            entry.reason = source->error;
          } else {
            entry.status = "fulfilled";
            entry.value = valueOf(*source);
          }
          if (--*remaining == 0) out->fulfill(std::move(*results));
        });
      }
      return combined;
    }

    template <class T> Promise<T> raceOf(std::vector<Promise<T>> promises) {
      Promise<T> combined;
      auto out = combined.state();
      for (auto& promise : promises) {
        auto source = promise.state();
        source->react([source, out] { copyOutcome(*source, *out); });
      }
      return combined;
    }

    template <class T> Promise<T> anyOf(std::vector<Promise<T>> promises) {
      Promise<T> combined;
      auto out = combined.state();
      auto errors = std::make_shared<std::vector<Reason>>(promises.size());
      auto remaining = std::make_shared<std::size_t>(promises.size());
      if (promises.empty()) out->reject(makeReason(AggregateError({}, "")));
      for (std::size_t index = 0; index < promises.size(); ++index) {
        auto source = promises[index].state();
        source->react([source, out, errors, remaining, index] {
          if (source->status == PromiseCore::Status::fulfilled) {
            copyOutcome(*source, *out);
            return;
          }
          (*errors)[index] = source->error;
          if (--*remaining == 0) out->reject(makeReason(AggregateError(std::move(*errors), "")));
        });
      }
      return combined;
    }

    template <class... T> Promise<std::tuple<Slot<T>...>> allTuple(std::tuple<Promise<T>...> promises) {
      Promise<std::tuple<Slot<T>...>> combined;
      auto out = combined.state();
      auto remaining = std::make_shared<std::size_t>(sizeof...(T));
      auto inputs = std::make_shared<std::tuple<Promise<T>...>>(std::move(promises));
      auto watch = [&](auto& promise) {
        auto source = promise.state();
        source->react([source, out, remaining, inputs] {
          if (source->status == PromiseCore::Status::rejected) {
            out->reject(source->error);
            return;
          }
          if (--*remaining == 0 && out->pending()) {
            out->fulfill(std::apply([](auto&... each) { return std::tuple<Slot<T>...>(valueOf(*each.state())...); }, *inputs));
          }
        });
      };
      std::apply([&](auto&... promise) { (watch(promise), ...); }, *inputs);
      return combined;
    }
  }

  template <class... P>
    requires(sizeof...(P) > 1)
  auto all(P&&... promises) {
    auto converted = std::tuple{detail::asPromise(std::forward<P>(promises))...};
    return detail::allTuple(std::move(converted));
  }

  template <class T, class F> Task<T> withFinally(Task<T> body, F onFinally) {
    Reason failure;
    std::optional<detail::Slot<T>> result;
    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(body);
      } else {
        T value = co_await std::move(body);
        result.emplace(std::move(value));
      }
    } catch (...) {
      failure = std::current_exception();
    }
    if constexpr (detail::IsTask<std::invoke_result_t<F&>>::value) co_await onFinally();
    else onFinally();
    if (failure) std::rethrow_exception(failure);
    if constexpr (!std::is_void_v<T>) co_return std::move(*result);
  }

  namespace detail {
    template <class T> struct GeneratorCore : std::enable_shared_from_this<GeneratorCore<T>> {
      enum class State : std::uint8_t { suspendedStart, suspendedYield, executing, awaitingReturn, completed };
      struct Request {
        bool isReturn;
        std::shared_ptr<PromiseState<IteratorResult<T>>> result;
      };
      State state = State::suspendedStart;
      std::deque<Request> queue;
      std::function<void()> launch;
      std::shared_ptr<Waker> waker = std::make_shared<Waker>();
      bool returnRequested = false;

      Request pop() {
        Request front = std::move(queue.front());
        queue.pop_front();
        return front;
      }
      void enqueue(bool isReturn, std::shared_ptr<PromiseState<IteratorResult<T>>> result) {
        queue.push_back({isReturn, std::move(result)});
        if (state != State::executing) resumeNext();
      }
      void resumeNext() {
        while (!queue.empty() && state != State::executing && state != State::awaitingReturn) {
          if (queue.front().isReturn && (state == State::suspendedStart || state == State::completed)) {
            // AsyncGeneratorAwaitReturn: the request completes one turn later.
            state = State::awaitingReturn;
            launch = nullptr;
            queueMicrotask(currentLoop(), [weak = this->weak_from_this(), result = pop().result] {
              result->fulfill(IteratorResult<T>{true, std::nullopt});
              if (auto core = weak.lock()) {
                core->state = State::completed;
                core->resumeNext();
              }
            });
            return;
          }
          if (state == State::completed) {
            pop().result->fulfill(IteratorResult<T>{true, std::nullopt});
            continue;
          }
          if (state == State::suspendedStart) {
            state = State::executing;
            auto body = std::move(launch);
            launch = nullptr;
            body();
            return;
          }
          state = State::executing;
          returnRequested = queue.front().isReturn;
          waker->wake();
          return;
        }
      }
      // The body yielded: completes the front request. Returns whether the body
      // must suspend (no request queued behind it).
      bool yielded(T value) {
        pop().result->fulfill(IteratorResult<T>{false, std::move(value)});
        if (queue.empty()) {
          state = State::suspendedYield;
          return true;
        }
        returnRequested = queue.front().isReturn;
        return false;
      }
      void finished(Reason failure) {
        state = State::completed;
        if (!queue.empty()) {
          auto front = pop();
          if (failure) front.result->reject(failure);
          else front.result->fulfill(IteratorResult<T>{true, std::nullopt});
        }
        resumeNext();
      }
    };

    // `yield value` awaits the value (one turn), completes the front request,
    // then suspends unless another request is queued. A return() resumes the
    // body with a return completion one turn later (it awaits its value first).
    template <class T> Task<void> yieldTo(std::weak_ptr<GeneratorCore<T>> weak, T value) {
      co_await microtask();
      bool suspend = false;
      bool returning = false;
      std::weak_ptr<Waker> waker;
      if (auto core = weak.lock()) {
        suspend = core->yielded(std::move(value));
        returning = !suspend && core->returnRequested;
        waker = core->waker;
      } else {
        co_await abandon();
        co_return;
      }
      if (suspend) {
        co_await sleepOn(std::move(waker));
        auto core = weak.lock();
        if (!core) {
          co_await abandon();
          co_return;
        }
        returning = core->returnRequested;
      }
      if (returning) {
        co_await microtask();
        throw GeneratorReturn{};
      }
    }

    template <class T, class TReturn>
    Task<void> runGenerator(std::weak_ptr<GeneratorCore<T>> weak, typename AsyncGenerator<T, TReturn>::Body body, std::shared_ptr<std::optional<Slot<TReturn>>> returned) {
      Reason failure;
      Yield<T> yield(weak);
      try {
        auto running = body(std::move(yield));
        if constexpr (std::is_void_v<TReturn>) {
          co_await std::move(running);
          returned->emplace();
        } else {
          auto value = co_await std::move(running);
          // `return value` in an async generator awaits the value first.
          co_await microtask();
          returned->emplace(std::move(value));
        }
      } catch (const GeneratorReturn&) {
      } catch (...) {
        failure = std::current_exception();
      }
      if (auto core = weak.lock()) core->finished(failure);
    }

    template <class T> Task<IteratorResult<T>> requestStep(std::shared_ptr<GeneratorCore<T>> core, bool isReturn) {
      auto result = std::make_shared<PromiseState<IteratorResult<T>>>();
      core->enqueue(isReturn, result);
      auto waiting = awaitOwned(std::move(result));
      return keepAlive<IteratorResult<T>>(std::move(core), std::move(waiting));
    }

    template <class T, class R> Task<R> delegateTo(Yield<T> outer, AsyncGenerator<T, R> inner) {
      for (;;) {
        IteratorResult<T> step = co_await inner.next();
        if (step.done) break;
        bool forwardReturn = false;
        try {
          auto passing = outer(std::move(*step.value));
          co_await std::move(passing);
        } catch (const GeneratorReturn&) {
          forwardReturn = true;
        }
        if (forwardReturn) {
          co_await inner.return_();
          throw GeneratorReturn{};
        }
      }
      if constexpr (!std::is_void_v<R>) co_return *inner.returnValue();
    }
  }

  template <class T> Task<void> Yield<T>::operator()(T value) const { return detail::yieldTo(core, std::move(value)); }

  template <class T> template <class G> auto Yield<T>::from(G inner) const { return detail::delegateTo(*this, std::move(inner)); }

  template <class T, class TReturn>
  AsyncGenerator<T, TReturn>::AsyncGenerator(Body body)
    : core_(std::make_shared<detail::GeneratorCore<T>>()), returned_(std::make_shared<std::optional<detail::Slot<TReturn>>>()) {
    std::weak_ptr<detail::GeneratorCore<T>> weak = core_;
    core_->launch = [weak, generatorBody = std::move(body), returned = returned_]() mutable {
      detail::start(detail::currentLoop(), detail::runGenerator<T, TReturn>(weak, std::move(generatorBody), returned));
    };
  }

  template <class T, class TReturn> Task<IteratorResult<T>> AsyncGenerator<T, TReturn>::next() const { return detail::requestStep(core_, false); }

  template <class T, class TReturn> Task<IteratorResult<T>> AsyncGenerator<T, TReturn>::return_() const { return detail::requestStep(core_, true); }

  template <class T, class TReturn>
  template <class U>
    requires(!std::is_void_v<U>)
  std::optional<U> AsyncGenerator<T, TReturn>::returnValue() const {
    return *returned_;
  }

  template <class T, class TReturn> bool AsyncGenerator<T, TReturn>::done() const noexcept {
    return core_->state == detail::GeneratorCore<T>::State::completed;
  }

  template <class T, class R, class F> Task<void> forAwait(AsyncGenerator<T, R> iterator, F body) {
    using Result = std::invoke_result_t<F&, T>;
    static_assert(std::is_same_v<Result, Task<void>> || std::is_same_v<Result, Task<bool>>, "the loop body returns Task<void> or Task<bool>");
    for (;;) {
      IteratorResult<T> step = co_await iterator.next();
      if (step.done) co_return;
      Reason failure;
      bool proceed = true;
      try {
        auto handling = body(std::move(*step.value));
        if constexpr (std::is_same_v<Result, Task<bool>>) proceed = co_await std::move(handling);
        else co_await std::move(handling);
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        try {
          co_await iterator.return_();
        } catch (...) {
        }
        std::rethrow_exception(failure);
      }
      if (!proceed) {
        co_await iterator.return_();
        co_return;
      }
    }
  }
}
