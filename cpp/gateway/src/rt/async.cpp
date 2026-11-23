#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/memory.hpp"
#include "uos/rt/number.hpp"
#include <boost/asio/async_result.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/execution.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace uos::rt {
  namespace asio = boost::asio;

  namespace {
    // `String(value)` for a thrown non-Error value.
    std::string jsString(const json::value& value) {
      switch (value.kind()) {
        case json::kind::null:
          return "null";
        case json::kind::bool_:
          return value.get_bool() ? "true" : "false";
        case json::kind::int64:
          return numberToString(static_cast<double>(value.get_int64()));
        case json::kind::uint64:
          return numberToString(static_cast<double>(value.get_uint64()));
        case json::kind::double_:
          return numberToString(value.get_double());
        case json::kind::string:
          return std::string(value.get_string());
        case json::kind::array: {
          std::string out;
          bool first = true;
          for (const auto& element : value.get_array()) {
            if (!first) out += ',';
            first = false;
            if (!element.is_null()) out += jsString(element);
          }
          return out;
        }
        case json::kind::object:
          return "[object Object]";
      }
      return "";
    }
  }

  ThrownValue::ThrownValue(std::optional<json::value> value_) : value(std::move(value_)), text_(value ? jsString(*value) : "undefined") {}

  AggregateError::AggregateError(std::vector<Reason> errors_, std::string message_) : JsError(std::move(message_), "AggregateError"), errors(std::move(errors_)) {}

  UncaughtError::UncaughtError(Reason reason_, bool rejection_) : reason(std::move(reason_)), rejection(rejection_), text_(report()) {}

  std::string UncaughtError::report() const {
    if (!reason) return "error: undefined";
    try {
      std::rethrow_exception(reason);
    } catch (const ThrownValue& value) {
      if (!value.value) return "error: undefined";
      if (value.value->is_object() || value.value->is_array()) return "error";
      return "error: " + jsString(*value.value);
    } catch (const JsError& error) {
      if (error.name == "Error" && !error.message.empty()) return "error: " + error.message;
      return error.name + ": " + error.message;
    } catch (const std::exception& error) {
      return std::string("error: ") + error.what();
    } catch (...) {
      return "error";
    }
  }

  Reason thrown(std::optional<json::value> value) { return std::make_exception_ptr(ThrownValue(std::move(value))); }

  void rethrow(const Reason& reason) {
    if (!reason) throw ThrownValue(std::nullopt);
    std::rethrow_exception(reason);
  }

  bool isError(const Reason& reason) {
    if (!reason) return false;
    try {
      std::rethrow_exception(reason);
    } catch (const ThrownValue&) {
      return false;
    } catch (...) {
      return true;
    }
  }

  std::optional<std::string> errorName(const Reason& reason) {
    if (!reason) return std::nullopt;
    try {
      std::rethrow_exception(reason);
    } catch (const ThrownValue&) {
      return std::nullopt;
    } catch (const JsError& error) {
      return error.name;
    } catch (...) {
      return std::string("Error");
    }
  }

  std::optional<std::string> errorMessage(const Reason& reason) {
    if (!reason) return std::nullopt;
    try {
      std::rethrow_exception(reason);
    } catch (const ThrownValue&) {
      return std::nullopt;
    } catch (const JsError& error) {
      return error.message;
    } catch (const std::exception& error) {
      return std::string(error.what());
    } catch (...) {
      return std::string();
    }
  }

  std::optional<json::value> thrownValue(const Reason& reason) {
    if (!reason) return std::nullopt;
    try {
      std::rethrow_exception(reason);
    } catch (const ThrownValue& value) {
      return value.value;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::string reasonToString(const Reason& reason) {
    if (!reason) return "undefined";
    try {
      std::rethrow_exception(reason);
    } catch (const ThrownValue& value) {
      return value.what();
    } catch (const JsError& error) {
      return error.toString();
    } catch (const std::exception& error) {
      return std::string("Error: ") + error.what();
    } catch (...) {
      return "Error";
    }
  }

  namespace {
    struct WaitingPromises : detail::WaitingList {
      std::size_t entries = 0;
      std::size_t reactionSlots = 0;
      memory::ScopedCache memory{memory::Cache::runtime_pending_promises};

      void publishMemory() noexcept {
        memory.set(entries, entries * sizeof(detail::PromiseCore) + reactionSlots * sizeof(detail::Job));
      }
    };
  }

  struct Loop::Impl {
    Loop& owner;
    Clock* clock;
    // Optional so the destructor decides when pending handlers are destroyed.
    std::optional<asio::io_context> context;
    std::deque<detail::Job> microtasks;
    WaitingPromises waiting;
    // Promises rejected without a handler during the current checkpoint.
    std::vector<std::shared_ptr<detail::PromiseCore>> rejections;
    std::shared_ptr<detail::TimerQueue> timers;
    UncaughtPolicy policy = UncaughtPolicy::raise;
    std::optional<UncaughtError> pendingError;
    std::size_t reported = 0;
    bool draining = false;
    bool running = false;
    bool shuttingDown = false;

    Impl(Loop& owner_, Clock& clock_) : owner(owner_), clock(&clock_), timers(detail::makeTimerQueue()) { context.emplace(1); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void shutdown();
    void uncaught(Reason reason, bool rejection);
    void checkpoint();
    bool turn(const detail::PromiseCore* until);
    bool idle();
    void runLoop(const detail::PromiseCore* until, std::optional<Task<void>> first);
  };

  namespace {
    thread_local Loop* scopedLoop = nullptr;

    // Runs a handler inline when its loop is the current loop of this thread
    // (blocking.possibly), and posts it to the io_context otherwise or when
    // blocking.never is required. Once the loop's io_context is gone, handlers
    // are destroyed without running.
    class LoopExecutor {
    public:
      LoopExecutor(Loop::Impl* impl, bool never) noexcept : impl_(impl), never_(never) {}

      asio::execution_context& query(asio::execution::context_t) const noexcept { return *impl_->context; }
      asio::execution::blocking_t query(asio::execution::blocking_t) const noexcept {
        return never_ ? asio::execution::blocking_t(asio::execution::blocking.never) : asio::execution::blocking_t(asio::execution::blocking.possibly);
      }
      LoopExecutor require(asio::execution::blocking_t::never_t) const noexcept { return LoopExecutor(impl_, true); }
      LoopExecutor require(asio::execution::blocking_t::possibly_t) const noexcept { return LoopExecutor(impl_, false); }

      template <class F> void execute(F&& function) const {
        if (impl_->shuttingDown || !impl_->context) {
          std::decay_t<F> dropped(std::forward<F>(function));
          return;
        }
        if (!never_ && &detail::currentLoop().impl() == impl_) {
          std::decay_t<F> local(std::forward<F>(function));
          local();
          return;
        }
        asio::post(*impl_->context, std::forward<F>(function));
      }

      friend bool operator==(const LoopExecutor& left, const LoopExecutor& right) noexcept { return left.impl_ == right.impl_ && left.never_ == right.never_; }

    private:
      Loop::Impl* impl_;
      bool never_;
    };

    std::chrono::microseconds toDuration(double ms) { return std::chrono::microseconds(static_cast<std::int64_t>(std::ceil(ms * 1000))); }

    template <class Handler> detail::Job resumeJob(Handler handler) {
      return detail::Job([handler = std::move(handler)]() mutable { std::move(handler)(); });
    }

    void linkWaiting(WaitingPromises& list, detail::PromiseCore& core) {
      core.waitingList = &list;
      core.previousWaiting = nullptr;
      core.nextWaiting = list.head;
      if (list.head) list.head->previousWaiting = &core;
      list.head = &core;
      ++list.entries;
      list.reactionSlots += core.reactions.capacity();
      list.publishMemory();
    }

    void unlinkWaiting(detail::PromiseCore& core) {
      if (!core.waitingList) return;
      auto& list = static_cast<WaitingPromises&>(*core.waitingList);
      if (core.previousWaiting) core.previousWaiting->nextWaiting = core.nextWaiting;
      else core.waitingList->head = core.nextWaiting;
      if (core.nextWaiting) core.nextWaiting->previousWaiting = core.previousWaiting;
      core.waitingList = nullptr;
      core.previousWaiting = nullptr;
      core.nextWaiting = nullptr;
      --list.entries;
      list.reactionSlots -= core.reactions.capacity();
      list.publishMemory();
    }

    // Drops the reactions of every waiting promise; true when there were any.
    bool dropWaiting(detail::WaitingList& list) {
      bool dropped = false;
      while (list.head) {
        dropped = true;
        detail::PromiseCore& core = *list.head;
        auto keep = core.weak_from_this().lock();
        unlinkWaiting(core);
        auto reactions = std::exchange(core.reactions, {});
        reactions.clear();
      }
      return dropped;
    }
  }

  void Loop::Impl::shutdown() {
    shuttingDown = true;
    // Timers and microtasks own callbacks and suspended coroutines; dropping
    // them can queue more work (a destroyed frame posts its unwinding), so
    // repeat until quiet, then destroy the io_context and what it still holds.
    for (int round = 0; round < 16; ++round) {
      detail::clearAllTimers(*timers);
      auto jobs = std::exchange(microtasks, {});
      auto candidates = std::exchange(rejections, {});
      bool dropped = !jobs.empty() || !candidates.empty();
      jobs.clear();
      candidates.clear();
      dropped = dropWaiting(waiting) || dropped;
      if (!dropped && microtasks.empty() && rejections.empty()) break;
    }
    context.reset();
    detail::clearAllTimers(*timers);
    dropWaiting(waiting);
    microtasks.clear();
    rejections.clear();
  }

  void Loop::Impl::uncaught(Reason reason, bool rejection) {
    UncaughtError error(std::move(reason), rejection);
    if (policy == UncaughtPolicy::report) {
      ++reported;
      std::fprintf(stderr, "%s\n", error.report().c_str());
      std::fflush(stderr);
      return;
    }
    if (!pendingError) pendingError.emplace(std::move(error));
  }

  void Loop::Impl::checkpoint() {
    if (draining) return;
    draining = true;
    {
      auto reset = scopeExit([this] { draining = false; });
      while (!microtasks.empty()) {
        detail::Job job = std::move(microtasks.front());
        microtasks.pop_front();
        try {
          job();
        } catch (...) {
          uncaught(std::current_exception(), false);
        }
      }
      auto candidates = std::exchange(rejections, {});
      for (const auto& core : candidates) {
        if (!core->handled) uncaught(core->error, true);
      }
    }
    if (pendingError) {
      UncaughtError error = std::move(*pendingError);
      pendingError.reset();
      throw error;
    }
  }

  // One loop iteration: the timers due now, then the IO handlers that are ready
  // (bounded, so a handler that keeps posting cannot starve the timers). Each
  // macrotask is followed by a microtask checkpoint.
  bool Loop::Impl::turn(const detail::PromiseCore* until) {
    bool ran = false;
    const double now = clock->monotonicNow();
    while (detail::fireDueTimer(*timers, now)) {
      ran = true;
      checkpoint();
      if (until && !until->pending()) return true;
    }
    if (context->stopped()) context->restart();
    for (int count = 0; count < 256 && context->poll_one() > 0; ++count) {
      ran = true;
      checkpoint();
      if (until && !until->pending()) return true;
    }
    return ran;
  }

  // Nothing is ready: waits for IO or the next timer. Returns false when no
  // work is left (no pending IO operation and no referenced timer).
  bool Loop::Impl::idle() {
    if (!microtasks.empty() || !rejections.empty()) {
      checkpoint();
      return true;
    }
    const bool io = !context->stopped();
    const std::optional<double> deadline = detail::nextTimerDeadline(*timers);
    if (!io && detail::referencedTimerCount(*timers) == 0) return false;
    const double now = clock->monotonicNow();
    if (!io) {
      if (*deadline > now) {
        if (clock->simulated()) clock->advanceTo(*deadline);
        else std::this_thread::sleep_for(toDuration(*deadline - now));
      }
      return true;
    }
    if (!deadline) {
      if (context->run_one() > 0) checkpoint();
      return true;
    }
    if (*deadline > now) {
      // While IO is pending a fake clock does not jump: the next timer fires
      // only after as much real time has passed without an IO completion.
      if (context->run_one_for(toDuration(*deadline - now)) > 0) {
        checkpoint();
        return true;
      }
      if (clock->simulated()) clock->advanceTo(*deadline);
    }
    return true;
  }

  void Loop::Impl::runLoop(const detail::PromiseCore* until, std::optional<Task<void>> first) {
    if (running) throw std::logic_error("rt::Loop::run or rt::blockOn called inside the running loop");
    running = true;
    auto reset = scopeExit([this] { running = false; });
    LoopScope scope(owner);
    if (first) detail::start(owner, std::move(*first));
    checkpoint();
    for (;;) {
      if (until && !until->pending()) return;
      if (turn(until)) continue;
      if (!idle()) break;
    }
    if (until && until->pending()) throw std::logic_error("rt::blockOn: the loop ran out of work while the task was still pending");
  }

  Loop::Loop() : Loop(systemClock()) {}
  Loop::Loop(Clock& clock) : impl_(std::make_unique<Impl>(*this, clock)) {}
  Loop::~Loop() {
    LoopScope scope(*this);
    impl_->shutdown();
  }

  asio::io_context& Loop::context() noexcept { return *impl_->context; }
  Executor Loop::executor() noexcept { return Executor(LoopExecutor(impl_.get(), false)); }
  Clock& Loop::clock() noexcept { return *impl_->clock; }
  void Loop::run() { impl_->runLoop(nullptr, std::nullopt); }
  std::size_t Loop::timerCount() const noexcept { return detail::activeTimerCount(*impl_->timers); }
  std::size_t Loop::microtaskCount() const noexcept { return impl_->microtasks.size(); }
  void Loop::setUncaughtPolicy(UncaughtPolicy policy) noexcept { impl_->policy = policy; }
  UncaughtPolicy Loop::uncaughtPolicy() const noexcept { return impl_->policy; }
  std::size_t Loop::uncaughtCount() const noexcept { return impl_->reported; }

  Loop& loop() { return detail::currentLoop(); }
  Executor executor() { return loop().executor(); }

  LoopScope::LoopScope(Loop& loop_) : previous_(scopedLoop) { scopedLoop = &loop_; }
  LoopScope::~LoopScope() { scopedLoop = previous_; }

  namespace detail {
    Loop& currentLoop() {
      if (scopedLoop) return *scopedLoop;
      static Loop* const process = new Loop();
      return *process;
    }

    std::shared_ptr<TimerQueue> timerQueueOf(Loop& loop) { return loop.impl().timers; }

    PromiseCore::~PromiseCore() { unlinkWaiting(*this); }

    void PromiseCore::react(Job reaction) {
      handled = true;
      auto& impl = currentLoop().impl();
      if (impl.shuttingDown) return;
      if (pending()) {
        const auto capacity = reactions.capacity();
        reactions.push_back(std::move(reaction));
        if (!waitingList) linkWaiting(impl.waiting, *this);
        else if (reactions.capacity() != capacity) {
          auto& list = static_cast<WaitingPromises&>(*waitingList);
          list.reactionSlots += reactions.capacity() - capacity;
          list.publishMemory();
        }
      } else {
        impl.microtasks.push_back(std::move(reaction));
      }
    }

    void PromiseCore::settle(Status outcome) {
      status = outcome;
      unlinkWaiting(*this);
      auto& impl = currentLoop().impl();
      if (impl.shuttingDown) {
        auto dropped = std::exchange(reactions, {});
        return;
      }
      for (auto& reaction : reactions) impl.microtasks.push_back(std::move(reaction));
      reactions.clear();
      if (outcome == Status::rejected && !handled) {
        if (auto self = weak_from_this().lock()) impl.rejections.push_back(std::move(self));
      }
    }

    void PromiseCore::reject(Reason reason) {
      if (!pending()) return;
      error = std::move(reason);
      settle(Status::rejected);
    }

    void start(Loop& loop, Task<void> task) {
      asio::co_spawn(loop.executor(), std::move(task), [](std::exception_ptr error) {
        if (error) reportUncaught(error, false);
      });
    }

    void queueMicrotask(Loop& loop, Job job) {
      auto& impl = loop.impl();
      if (!impl.shuttingDown) impl.microtasks.push_back(std::move(job));
    }

    void reportUncaught(Reason reason, bool rejection) { currentLoop().impl().uncaught(std::move(reason), rejection); }

    // The initiations are named locals: GCC 12 destroys a lambda temporary
    // created inside a co_await expression twice (docs/rt/async.md).
    Task<void> awaitSettled(std::shared_ptr<PromiseCore> core) {
      auto initiation = [core](auto handler) { core->react(resumeJob(std::move(handler))); };
      co_await asio::async_initiate<const asio::use_awaitable_t<>&, void()>(initiation, asio::use_awaitable);
    }

    void runUntilSettled(Loop& loop, Task<void> task, PromiseCore& core) { loop.impl().runLoop(&core, std::move(task)); }

    Task<void> sleepOn(std::weak_ptr<Waker> waker) {
      auto initiation = [waker](auto handler) {
        auto slot = waker.lock();
        if (!slot) return;
        if (slot->woken) {
          slot->woken = false;
          queueMicrotask(currentLoop(), resumeJob(std::move(handler)));
          return;
        }
        slot->waiter = resumeJob(std::move(handler));
      };
      co_await asio::async_initiate<const asio::use_awaitable_t<>&, void()>(initiation, asio::use_awaitable);
    }

    Task<void> abandon() {
      auto initiation = [](auto) {};
      co_await asio::async_initiate<const asio::use_awaitable_t<>&, void()>(initiation, asio::use_awaitable);
    }
  }

  Task<void> microtask() {
    auto initiation = [](auto handler) { detail::queueMicrotask(detail::currentLoop(), resumeJob(std::move(handler))); };
    co_await asio::async_initiate<const asio::use_awaitable_t<>&, void()>(initiation, asio::use_awaitable);
  }

  void queueMicrotask(std::function<void()> callback) {
    detail::queueMicrotask(detail::currentLoop(), [callback = std::move(callback)] { callback(); });
  }

  Promise<void> all(std::vector<Promise<void>> promises) {
    Promise<void> combined;
    auto out = combined.state();
    auto remaining = std::make_shared<std::size_t>(promises.size());
    if (promises.empty()) out->fulfill();
    for (auto& promise : promises) {
      auto source = promise.state();
      source->react([source, out, remaining] {
        if (source->status == detail::PromiseCore::Status::rejected) {
          out->reject(source->error);
          return;
        }
        if (--*remaining == 0) out->fulfill();
      });
    }
    return combined;
  }
}
