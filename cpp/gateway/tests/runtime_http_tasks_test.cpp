// Differential cases for src/runtime/http-tasks.ts (no direct bun test): the
// expected event logs were recorded by running the same scenarios under bun 1.4.0.
#include "uos/runtime/http_tasks.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/test_async.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rt = uos::rt;
using uos::runtime::http_tasks::createRequestTaskDrain;
using uos::runtime::http_tasks::trackRequestTask;

namespace {
  // Counts microtask turns: `tick` advances once per turn, and the callbacks
  // scheduled for a tick run inside that turn.
  struct Ticker {
    int tick = 0;
    std::string events;
    std::map<int, std::vector<std::function<void()>>> at;
    void log(const std::string& event) { events += (events.empty() ? "" : " ") + event + "@" + std::to_string(tick); }
    void schedule(int when, std::function<void()> callback) { at[when].push_back(std::move(callback)); }
  };

  void tickOnce(const std::shared_ptr<Ticker>& ticker, int turns) {
    ticker->tick += 1;
    if (const auto callbacks = ticker->at.find(ticker->tick); callbacks != ticker->at.end())
      for (const auto& callback : callbacks->second) callback();
    if (ticker->tick < turns) rt::queueMicrotask([ticker, turns] { tickOnce(ticker, turns); });
  }

  // `queueMicrotask(ticker)`: one tick per microtask turn, up to `turns`.
  void startTicker(const std::shared_ptr<Ticker>& ticker, int turns) {
    rt::queueMicrotask([ticker, turns] { tickOnce(ticker, turns); });
  }
}

ASYNC_TEST("request task drains wait for every tracked task, including ones tracked while draining") {
  auto ticker = std::make_shared<Ticker>();
  auto completed = rt::withResolvers<void>();
  const auto drain = createRequestTaskDrain(completed.promise);
  auto a = rt::withResolvers<void>();
  auto b = rt::withResolvers<void>();
  auto c = rt::withResolvers<void>();
  trackRequestTask(completed.promise, a.promise);
  trackRequestTask(completed.promise, b.promise);
  trackRequestTask(completed.promise, a.promise);
  trackRequestTask(std::nullopt, rt::withResolvers<void>().promise);
  trackRequestTask(rt::withResolvers<void>().promise, rt::withResolvers<void>().promise);
  trackRequestTask(completed.promise, rt::Promise<void>::resolve());
  // The callbacks run inside tickOnce, which holds the ticker.
  Ticker* const recorder = ticker.get();
  ticker->schedule(2, [recorder, a] {
    recorder->log("resolve a");
    a.resolve();
  });
  ticker->schedule(4, [recorder, completed, c] {
    recorder->log("track c");
    trackRequestTask(completed.promise, c.promise);
  });
  ticker->schedule(5, [recorder, b] {
    recorder->log("reject b");
    b.reject(rt::makeReason(rt::JsError("b failed")));
  });
  ticker->schedule(9, [recorder, c] {
    recorder->log("resolve c");
    c.resolve();
  });
  startTicker(ticker, 80);
  ticker->log("drain start");
  const rt::Promise<void> first = drain().then([ticker] { ticker->log("drained"); });
  co_await first.wait();
  ticker->log("after first");
  trackRequestTask(completed.promise, rt::withResolvers<void>().promise);
  const rt::Promise<void> again = drain().then([ticker] { ticker->log("drained again"); });
  co_await again.wait();
  const auto empty = createRequestTaskDrain(rt::withResolvers<void>().promise);
  const rt::Promise<void> emptied = empty().then([ticker] { ticker->log("empty drained"); });
  co_await emptied.wait();
  auto settledEarly = rt::withResolvers<void>();
  const auto early = createRequestTaskDrain(settledEarly.promise);
  const rt::Promise<void> done = rt::Promise<void>::resolve();
  trackRequestTask(settledEarly.promise, done);
  const rt::Promise<void> earlyDrained = early().then([ticker] { ticker->log("early drained"); });
  co_await earlyDrained.wait();
  EXPECT_EQ(ticker->events, std::string("drain start@0 resolve a@2 track c@4 reject b@5 resolve c@9 drained@11 after first@12 drained again@13 empty drained@15 early drained@19"));
}
