#pragma once
// AbortController / AbortSignal / DOMException with Bun's names, messages and
// listener ordering, plus the bridge from an abort to a pending awaitable.
#include "uos/rt/async.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uos::rt {
  // `new DOMException(message, name)`: an Error (`instanceof Error`) whose name
  // is the DOMException name. String(e) is "name: message" ("name" when the
  // message is empty).
  class DOMException : public JsError {
  public:
    explicit DOMException(std::string message_ = "", std::string name_ = "Error");
    // The legacy code: 20 for AbortError, 23 for TimeoutError, ..., 0 for names without one.
    int code() const noexcept;
  };

  // The reasons Bun creates (a new object each time): `controller.abort()` /
  // `AbortSignal.abort()` without a reason, and `AbortSignal.timeout(ms)` expiring.
  Reason abortErrorReason();    // DOMException("The operation was aborted.", "AbortError")
  Reason timeoutErrorReason();  // DOMException("The operation timed out.", "TimeoutError")

  // A listener with identity, the C++ form of a named JS function: adding the
  // same listener twice registers it once, and removeEventListener removes
  // exactly that listener. Copies share the identity; two listeners made from
  // separate lambdas are different listeners.
  class EventListener {
  public:
    EventListener() = default;
    template <class F>
      requires(!std::is_same_v<std::decay_t<F>, EventListener> && std::is_invocable_v<std::decay_t<F>&>)
    EventListener(F&& callback) : callback_(std::make_shared<std::function<void()>>(std::forward<F>(callback))) {}
    void operator()() const { (*callback_)(); }
    explicit operator bool() const noexcept { return callback_ != nullptr; }
    friend bool operator==(const EventListener& left, const EventListener& right) noexcept { return left.callback_ == right.callback_; }

  private:
    std::shared_ptr<std::function<void()>> callback_;
  };

  struct AddEventListenerOptions {
    bool once = false;
  };

  namespace detail {
    struct AbortSignalState;
  }

  // `AbortSignal`: a shared handle; copies refer to the same signal.
  class AbortSignal {
  public:
    explicit AbortSignal(std::shared_ptr<detail::AbortSignalState> signalState);
    bool aborted() const noexcept;
    // `signal.reason`: undefined (null) until aborted.
    const Reason& reason() const noexcept;
    // `signal.throwIfAborted()`: throws the reason itself.
    void throwIfAborted() const;
    // `signal.addEventListener(type, listener, { once })`. Only "abort" events
    // are ever dispatched (other types are accepted and never fire). Listeners
    // run synchronously inside the abort, in registration order, after
    // `aborted`/`reason` are set (for AbortSignal.any dependents too); one added
    // or removed during the dispatch is not called; an exception from a
    // listener is uncaught and the next listener still runs. A listener added
    // after the abort is never called.
    void addEventListener(std::string_view type, EventListener listener, AddEventListenerOptions options = {}) const;
    void removeEventListener(std::string_view type, const EventListener& listener) const;
    // `signal.onabort = handler` (std::nullopt is `null`) and `signal.onabort`.
    // The handler keeps the listener position of its first assignment.
    void setOnabort(std::optional<EventListener> handler) const;
    std::optional<EventListener> onabort() const;

    // `AbortSignal.abort(reason)`: an already aborted signal.
    static AbortSignal abort(Reason reason = nullptr);
    // `AbortSignal.timeout(ms)`: aborts with a TimeoutError DOMException after
    // `ms` (an unreferenced timer, so it never keeps Loop::run alive). TypeError
    // "Value -1 is outside the range [0, 9007199254740991]" for a negative,
    // NaN or infinite `ms`.
    static AbortSignal timeout(double ms);
    // `AbortSignal.any(signals)`: aborted with the first source's reason; already
    // aborted when a source already is. Its listeners run after the source's.
    static AbortSignal any(const std::vector<AbortSignal>& signals);

    friend bool operator==(const AbortSignal& left, const AbortSignal& right) noexcept { return left.state_ == right.state_; }
    const std::shared_ptr<detail::AbortSignalState>& state() const noexcept { return state_; }

  private:
    std::shared_ptr<detail::AbortSignalState> state_;
  };

  // `new AbortController()`
  class AbortController {
  public:
    AbortController();
    // `controller.signal`
    AbortSignal signal() const { return signal_; }
    // `controller.abort(reason)`: no-op once aborted; undefined (a null Reason)
    // becomes the AbortError DOMException.
    void abort(Reason reason = nullptr) const;

  private:
    AbortSignal signal_;
  };

  namespace detail {
    // Starts `child` (which settles `out`) so that an abort of `signal` rejects
    // `out` with the signal's reason and cancels (terminal) the child's pending
    // Asio operation.
    void runAbortable(const AbortSignal& signal, std::shared_ptr<PromiseCore> out, Task<void> child);
  }

  // Starts `task` now and returns a Task that settles like it, or rejects with
  // signal.reason as soon as the signal aborts (at once, without starting the
  // task, when already aborted). The abort also cancels the task's pending Asio
  // operation (socket read, timer wait), which then throws `operation_aborted`
  // inside the task, as does every later async operation it awaits; its outcome
  // is then ignored. Waits on rt promises (rt::Promise, rt::sleep) are not
  // interrupted, as in JS.
  template <class T> [[nodiscard]] Task<T> abortable(AbortSignal signal, Task<T> task) {
    if (signal.aborted()) return detail::rejected<T>(signal.reason());
    auto out = std::make_shared<detail::PromiseState<T>>();
    detail::runAbortable(signal, out, detail::runInto(out, std::move(task)));
    return detail::awaitOwned(std::move(out));
  }

  // `sleep(ms)` that rejects with signal.reason as soon as the signal aborts
  // (and clears its timer); rejects at once when already aborted.
  [[nodiscard]] Task<void> sleep(double ms, AbortSignal signal);
}
