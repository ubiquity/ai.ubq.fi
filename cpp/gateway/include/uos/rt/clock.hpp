#pragma once
// Time for ported code: `Date.now()`, `performance.now()` and the timer
// functions, all through the current loop's injectable Clock.
#include "uos/rt/async.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace uos::rt {
  class Clock {
  public:
    virtual ~Clock() = default;
    // `Date.now()`: whole milliseconds since the Unix epoch.
    virtual double dateNow() = 0;
    // `performance.now()`: fractional milliseconds since the time origin.
    virtual double performanceNow() = 0;
    // The monotonic time base of timers, in milliseconds.
    virtual double monotonicNow() = 0;
    // A simulated clock never blocks: the loop moves it to the next timer when idle.
    virtual bool simulated() const noexcept { return false; }
    virtual void advanceTo(double monotonicMs) { (void)monotonicMs; }
  };

  // The operating system clocks (system_clock and steady_clock; the time origin
  // is process start).
  class SystemClock final : public Clock {
  public:
    double dateNow() override;
    double performanceNow() override;
    double monotonicNow() override;
  };
  SystemClock& systemClock();

  // A deterministic clock for tests. Time moves only through advance() and when
  // the loop has nothing else to do: then it jumps to the next timer, so
  // `co_await rt::sleep(5000)` returns at once with every earlier timer fired in
  // order. While an IO operation is pending it does not jump: the next timer
  // fires only after that much real time has passed without an IO completion.
  class FakeClock final : public Clock {
  public:
    // Date.now() starts at `dateNowMs` (default: the real current time) and
    // performance.now() at `performanceNowMs`; both then advance together.
    explicit FakeClock(std::optional<double> dateNowMs = std::nullopt, double performanceNowMs = 0);
    double dateNow() override;
    double performanceNow() override;
    double monotonicNow() override;
    bool simulated() const noexcept override { return true; }
    void advanceTo(double monotonicMs) override;
    // Moves time forward; timers that became due fire at the loop's next turn.
    void advance(double ms);

  private:
    double monotonic_;
    double dateOrigin_;
  };

  // `Date.now()` and `performance.now()`. Date.now() calls globalDateNow().
  double dateNow();
  double performanceNow();
  // `Date.now` as the replaceable global it is in JS (process-wide). It starts
  // as the current loop's clock. The TS test idiom
  //   const realNow = Date.now; Date.now = () => clock; ... Date.now = realNow;
  // ports as
  //   auto realNow = rt::globalDateNow(); rt::globalDateNow() = [&] { return clock; }; ... rt::globalDateNow() = realNow;
  std::function<double()>& globalDateNow();

  namespace detail {
    struct TimerState;
  }

  // The handle setTimeout/setInterval return (Bun's Timeout object). An empty
  // handle (default constructed) is accepted and ignored by clearTimeout.
  class Timeout {
  public:
    Timeout() = default;
    explicit Timeout(std::shared_ptr<detail::TimerState> state_) : state(std::move(state_)) {}
    // A referenced timer keeps Loop::run alive; every timer fires while the loop runs.
    Timeout& ref();
    Timeout& unref();
    // Referenced and still armed (true inside its own callback, false once a
    // timeout fired or the timer was cleared), as Bun reports it.
    bool hasRef() const noexcept;
    // Restarts the countdown of an armed timer from now with the same delay.
    // Unlike Bun it does not re-arm a timeout that already fired (its callback
    // is released when it fires; the TS never calls refresh()).
    Timeout& refresh();
    // `+timeout` / `timeout[Symbol.toPrimitive]()`: Bun's numeric id (1, 2, ... per process).
    double id() const noexcept;
    // Neither fired (for timeouts) nor cleared.
    bool active() const noexcept;
    explicit operator bool() const noexcept { return state != nullptr; }
    friend bool operator==(const Timeout& left, const Timeout& right) noexcept { return left.state == right.state; }

    const std::shared_ptr<detail::TimerState>& handle() const noexcept { return state; }

  private:
    std::shared_ptr<detail::TimerState> state;
  };

  // `setTimeout(callback, delay)` through globalSetTimeout(). Bun's delay rules:
  // the delay is truncated to whole milliseconds; NaN, negative, zero and values
  // above 2^31 - 1 become 1 ms (NaN, negative and overflowing values print Bun's
  // process warning, once per kind). Timers fire in deadline order, equal
  // deadlines in creation order, each as its own macrotask followed by a
  // microtask checkpoint. An exception from the callback is uncaught.
  Timeout setTimeout(std::function<void()> callback, double delay = 0);
  // `setInterval(callback, delay)`: the next run is due `delay` after the loop
  // turn that ran the previous one began.
  Timeout setInterval(std::function<void()> callback, double delay = 0);
  // `clearTimeout(handle)` (through globalClearTimeout()) / `clearInterval(handle)`:
  // no-op for an empty, fired or cleared handle; works on either kind of timer
  // and inside the timer's own callback.
  void clearTimeout(const Timeout& handle);
  void clearTimeout(const std::optional<Timeout>& handle);
  void clearInterval(const Timeout& handle);
  void clearInterval(const std::optional<Timeout>& handle);

  // `globalThis.setTimeout` / `globalThis.clearTimeout` as replaceable
  // process-wide functions, for tests that intercept timers:
  //   auto originalSetTimeout = rt::globalSetTimeout();
  //   rt::globalSetTimeout() = [&](std::function<void()> handler, double timeout) { ...; return originalSetTimeout(handler, timeout); };
  //   ... rt::globalSetTimeout() = originalSetTimeout;
  // They start as Bun's implementations. AbortSignal.timeout and setInterval do
  // not go through them (Bun's do not either).
  using SetTimeoutFunction = std::function<Timeout(std::function<void()> callback, double delay)>;
  using ClearTimeoutFunction = std::function<void(const Timeout& handle)>;
  SetTimeoutFunction& globalSetTimeout();
  ClearTimeoutFunction& globalClearTimeout();

  // `await new Promise((resolve) => setTimeout(resolve, ms))` (the timer is
  // created when called, through globalSetTimeout()).
  [[nodiscard]] Task<void> sleep(double ms);

  namespace detail {
    // The loop's timer list (owned by Loop::Impl).
    struct TimerQueue;
    std::shared_ptr<TimerQueue> makeTimerQueue();
    std::shared_ptr<TimerQueue> timerQueueOf(Loop& loop);
    // A timer on the current loop, without the globalSetTimeout indirection.
    Timeout createTimer(std::function<void()> callback, double delay, bool repeat);
    // A one-shot timer `ms` (a validated, non-negative count; fractions are
    // truncated) from now, without setTimeout's delay rules (AbortSignal.timeout).
    Timeout createExactTimer(std::function<void()> callback, double ms);
    // Earliest deadline among queued timers (monotonic ms).
    std::optional<double> nextTimerDeadline(const TimerQueue& queue);
    std::size_t referencedTimerCount(const TimerQueue& queue) noexcept;
    std::size_t activeTimerCount(const TimerQueue& queue) noexcept;
    // Fires the earliest timer due at `now`, if any; true when one fired.
    bool fireDueTimer(TimerQueue& queue, double now);
    // Disarms every timer and drops its callback (loop teardown).
    void clearAllTimers(TimerQueue& queue) noexcept;
  }
}
