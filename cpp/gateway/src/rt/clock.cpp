#include "uos/rt/clock.hpp"
#include "uos/rt/memory.hpp"
#include "uos/rt/number.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unistd.h>
#include <utility>

namespace uos::rt {
  namespace detail {
    struct TimerQueue {
      std::map<std::pair<double, std::uint64_t>, std::shared_ptr<TimerState>> queued;
      // Armed timers (queued, or running their callback and not cleared).
      std::size_t active = 0;
      std::size_t referenced = 0;
      std::uint64_t nextOrder = 0;
      memory::ScopedCache memory{memory::Cache::runtime_timers};
      void publishMemory() noexcept;
    };

    struct TimerState {
      std::weak_ptr<TimerQueue> queue;
      std::function<void()> callback;
      double delay = 1;
      double deadline = 0;
      std::uint64_t order = 0;
      double id = 0;
      bool repeat = false;
      bool referenced = true;
      bool armed = true;
      bool queued = false;
    };

    void TimerQueue::publishMemory() noexcept {
      // Account for the timer and the ordered-map node that owns it.
      constexpr auto entryBytes = sizeof(TimerState) + sizeof(decltype(queued)::value_type) + 4 * sizeof(void*);
      memory.set(queued.size(), queued.size() * entryBytes);
    }
  }

  namespace {
    const std::chrono::steady_clock::time_point timeOrigin = std::chrono::steady_clock::now();

    enum class TimeoutWarning : std::uint8_t { nan, negative, overflow };

    // Bun prints each kind of timeout warning once per process, and the
    // --trace-warnings hint after the first warning.
    void warnOnce(TimeoutWarning kind, const std::string& text) {
      static bool printed[3] = {false, false, false};
      static bool hinted = false;
      auto& seen = printed[static_cast<std::size_t>(kind)];
      if (seen) return;
      seen = true;
      std::fprintf(stderr, "(node:%ld) %s\n", static_cast<long>(::getpid()), text.c_str());
      if (!hinted) {
        hinted = true;
        std::fprintf(stderr, "(Use `bun --trace-warnings ...` to show where the warning was created)\n");
      }
      std::fflush(stderr);
    }

    double normalizeDelay(double delay) {
      if (std::isnan(delay)) {
        warnOnce(TimeoutWarning::nan, "TimeoutNaNWarning: NaN is not a number.\nTimeout duration was set to 1.");
        return 1;
      }
      if (delay < 0) {
        warnOnce(TimeoutWarning::negative, "TimeoutNegativeWarning: " + numberToString(delay) + " is a negative number.\nTimeout duration was set to 1.");
        return 1;
      }
      if (delay > 2147483647.0) {
        warnOnce(TimeoutWarning::overflow, "TimeoutOverflowWarning: " + numberToString(delay) + " does not fit into a 32-bit signed integer.\nTimeout duration was set to 1.");
        return 1;
      }
      const double whole = std::trunc(delay);
      return whole < 1 ? 1 : whole;
    }

    double nextTimerId() noexcept {
      static double next = 0;
      return ++next;
    }

    void enqueue(detail::TimerQueue& queue, const std::shared_ptr<detail::TimerState>& timer, double deadline) {
      timer->deadline = deadline;
      timer->order = queue.nextOrder++;
      queue.queued.emplace(std::make_pair(deadline, timer->order), timer);
      queue.publishMemory();
      timer->queued = true;
    }

    void dequeue(detail::TimerQueue& queue, detail::TimerState& timer) {
      if (!timer.queued) return;
      queue.queued.erase(std::make_pair(timer.deadline, timer.order));
      queue.publishMemory();
      timer.queued = false;
    }

    void disarm(detail::TimerQueue& queue, detail::TimerState& timer) {
      if (!timer.armed) return;
      dequeue(queue, timer);
      timer.armed = false;
      --queue.active;
      if (timer.referenced) --queue.referenced;
    }

    void clearTimer(const Timeout& handle) {
      const auto& timer = handle.handle();
      if (!timer) return;
      if (auto queue = timer->queue.lock()) disarm(*queue, *timer);
      else timer->armed = false;
      timer->callback = nullptr;
    }
  }

  double SystemClock::dateNow() { return std::floor(std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count()); }
  double SystemClock::performanceNow() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - timeOrigin).count(); }
  double SystemClock::monotonicNow() { return performanceNow(); }

  SystemClock& systemClock() {
    static SystemClock clock;
    return clock;
  }

  FakeClock::FakeClock(std::optional<double> dateNowMs, double performanceNowMs)
    : monotonic_(performanceNowMs), dateOrigin_((dateNowMs ? *dateNowMs : systemClock().dateNow()) - performanceNowMs) {}
  double FakeClock::dateNow() { return std::floor(dateOrigin_ + monotonic_); }
  double FakeClock::performanceNow() { return monotonic_; }
  double FakeClock::monotonicNow() { return monotonic_; }
  void FakeClock::advanceTo(double monotonicMs) {
    if (monotonicMs > monotonic_) monotonic_ = monotonicMs;
  }
  void FakeClock::advance(double ms) {
    if (ms > 0) monotonic_ += ms;
  }

  std::function<double()>& globalDateNow() {
    static std::function<double()> function = [] { return detail::currentLoop().clock().dateNow(); };
    return function;
  }

  double dateNow() {
    const auto& function = globalDateNow();
    return function ? function() : detail::currentLoop().clock().dateNow();
  }

  double performanceNow() { return detail::currentLoop().clock().performanceNow(); }

  Timeout& Timeout::ref() {
    if (state && !state->referenced) {
      state->referenced = true;
      if (state->armed) {
        if (auto queue = state->queue.lock()) ++queue->referenced;
      }
    }
    return *this;
  }

  Timeout& Timeout::unref() {
    if (state && state->referenced) {
      state->referenced = false;
      if (state->armed) {
        if (auto queue = state->queue.lock()) --queue->referenced;
      }
    }
    return *this;
  }

  bool Timeout::hasRef() const noexcept { return state && state->referenced && state->armed; }

  Timeout& Timeout::refresh() {
    if (!state || !state->armed) return *this;
    auto queue = state->queue.lock();
    if (!queue) return *this;
    dequeue(*queue, *state);
    enqueue(*queue, state, detail::currentLoop().clock().monotonicNow() + state->delay);
    return *this;
  }

  double Timeout::id() const noexcept { return state ? state->id : 0; }
  bool Timeout::active() const noexcept { return state && state->armed; }

  SetTimeoutFunction& globalSetTimeout() {
    static SetTimeoutFunction function = [](std::function<void()> callback, double delay) { return detail::createTimer(std::move(callback), delay, false); };
    return function;
  }

  ClearTimeoutFunction& globalClearTimeout() {
    static ClearTimeoutFunction function = [](const Timeout& handle) { clearTimer(handle); };
    return function;
  }

  Timeout setTimeout(std::function<void()> callback, double delay) { return globalSetTimeout()(std::move(callback), delay); }
  Timeout setInterval(std::function<void()> callback, double delay) { return detail::createTimer(std::move(callback), delay, true); }
  void clearTimeout(const Timeout& handle) { globalClearTimeout()(handle); }
  void clearTimeout(const std::optional<Timeout>& handle) {
    if (handle) clearTimeout(*handle);
  }
  void clearInterval(const Timeout& handle) { clearTimer(handle); }
  void clearInterval(const std::optional<Timeout>& handle) {
    if (handle) clearTimer(*handle);
  }

  Task<void> sleep(double ms) {
    auto state = std::make_shared<detail::PromiseState<void>>();
    setTimeout([state] { state->fulfill(); }, ms);
    return detail::awaitOwned(std::move(state));
  }

  namespace detail {
    std::shared_ptr<TimerQueue> makeTimerQueue() { return std::make_shared<TimerQueue>(); }

    Timeout createTimer(std::function<void()> callback, double delay, bool repeat) {
      auto& loop = currentLoop();
      auto queue = timerQueueOf(loop);
      auto timer = std::make_shared<TimerState>();
      timer->queue = queue;
      timer->callback = std::move(callback);
      timer->delay = normalizeDelay(delay);
      timer->id = nextTimerId();
      timer->repeat = repeat;
      ++queue->active;
      ++queue->referenced;
      enqueue(*queue, timer, loop.clock().monotonicNow() + timer->delay);
      return Timeout(timer);
    }

    Timeout createExactTimer(std::function<void()> callback, double ms) {
      auto& loop = currentLoop();
      auto queue = timerQueueOf(loop);
      auto timer = std::make_shared<TimerState>();
      timer->queue = queue;
      timer->callback = std::move(callback);
      timer->delay = std::trunc(ms);
      timer->id = nextTimerId();
      ++queue->active;
      ++queue->referenced;
      enqueue(*queue, timer, loop.clock().monotonicNow() + timer->delay);
      return Timeout(timer);
    }

    std::optional<double> nextTimerDeadline(const TimerQueue& queue) {
      if (queue.queued.empty()) return std::nullopt;
      return queue.queued.begin()->first.first;
    }

    std::size_t referencedTimerCount(const TimerQueue& queue) noexcept { return queue.referenced; }
    std::size_t activeTimerCount(const TimerQueue& queue) noexcept { return queue.active; }

    bool fireDueTimer(TimerQueue& queue, double now) {
      if (queue.queued.empty()) return false;
      auto first = queue.queued.begin();
      if (first->first.first > now) return false;
      std::shared_ptr<TimerState> timer = first->second;
      queue.queued.erase(first);
      queue.publishMemory();
      timer->queued = false;
      // The callback is moved out while it runs: clearing the timer inside it
      // must not destroy the function being executed.
      std::function<void()> callback = std::move(timer->callback);
      timer->callback = nullptr;
      try {
        if (callback) callback();
      } catch (...) {
        reportUncaught(std::current_exception(), false);
      }
      if (!timer->armed) return true;
      if (timer->queued) {
        timer->callback = std::move(callback);
        return true;
      }
      if (timer->repeat) {
        timer->callback = std::move(callback);
        enqueue(queue, timer, now + timer->delay);
        return true;
      }
      disarm(queue, *timer);
      return true;
    }

    void clearAllTimers(TimerQueue& queue) noexcept {
      auto timers = std::exchange(queue.queued, {});
      queue.publishMemory();
      queue.active = 0;
      queue.referenced = 0;
      for (auto& entry : timers) {
        auto& timer = entry.second;
        timer->queued = false;
        timer->armed = false;
        auto callback = std::move(timer->callback);
        timer->callback = nullptr;
      }
    }
  }
}
