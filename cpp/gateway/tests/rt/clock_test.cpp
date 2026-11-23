// rt::Clock, timers and sleep. Expected orders marked "bun" were recorded by
// running the same scenario under Bun 1.4 (a busy wait there is
// FakeClock::advance here).
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/test_async.hpp"
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace uos;

namespace {
  using Strings = std::vector<std::string>;
  using Log = std::shared_ptr<Strings>;
  Log makeLog() { return std::make_shared<Strings>(); }
  std::string at(const std::string& label) { return label + "@" + std::to_string(static_cast<long long>(rt::performanceNow())); }
}

ASYNC_TEST("bun t3: timers fire in deadline order, equal deadlines in creation order, with a checkpoint after each") {
  auto log = makeLog();
  rt::setTimeout([log] { log->push_back("st-2"); }, 2);
  rt::setTimeout([log] { log->push_back("st-5"); }, 5);
  rt::setTimeout([log] { log->push_back("st-4"); }, 4);
  rt::setTimeout([log] { log->push_back("st-1"); }, 1);
  co_await rt::sleep(10);
  EXPECT_EQ(*log, (Strings{"st-1", "st-2", "st-4", "st-5"}));
  log->clear();
  rt::setTimeout([log] { log->push_back("A-10"); }, 10);
  rt::test::clock().advance(5);
  rt::setTimeout([log] { log->push_back("B-3"); }, 3);
  rt::test::clock().advance(20);
  co_await rt::sleep(1);
  EXPECT_EQ(*log, (Strings{"B-3", "A-10"}));
  log->clear();
  rt::setTimeout([log] {
    log->push_back("t1");
    rt::setTimeout([log] { log->push_back("t1-child-0"); }, 0);
    (void)rt::Promise<void>::resolve().then([log] { log->push_back("t1-micro"); });
  }, 1);
  rt::setTimeout([log] { log->push_back("t2"); }, 1);
  rt::test::clock().advance(10);
  co_await rt::sleep(5);
  EXPECT_EQ(*log, (Strings{"t1", "t1-micro", "t2", "t1-child-0"}));
}

ASYNC_TEST("bun t4: delays are truncated to whole milliseconds") {
  auto log = makeLog();
  rt::setTimeout([log] { log->push_back("A-2.9"); }, 2.9);
  rt::setTimeout([log] { log->push_back("B-2"); }, 2);
  rt::setTimeout([log] { log->push_back("C-1.5"); }, 1.5);
  rt::setTimeout([log] { log->push_back("D-1"); }, 1);
  rt::test::clock().advance(10);
  co_await rt::sleep(20);
  EXPECT_EQ(*log, (Strings{"C-1.5", "D-1", "A-2.9", "B-2"}));
}

ASYNC_TEST("bun timers1: overflow and zero delays become 1 ms; clearing inside callbacks; intervals reschedule after their run") {
  auto log = makeLog();
  rt::setTimeout([log] { log->push_back(at("overflow-fired")); }, std::pow(2.0, 31));
  auto max = rt::setTimeout([log] { log->push_back("max-fired"); }, std::pow(2.0, 31) - 1);
  max.unref();
  rt::setTimeout([log] { log->push_back(at("1.7")); }, 1.7);
  rt::setTimeout([log] { log->push_back(at("1.2")); }, 1.2);
  rt::setTimeout([log] { log->push_back(at("2")); }, 2);
  auto runs = std::make_shared<int>(0);
  auto interval = std::make_shared<rt::Timeout>();
  *interval = rt::setInterval([log, runs, interval] {
    ++*runs;
    log->push_back(at("iv" + std::to_string(*runs)));
    if (*runs == 3) rt::clearInterval(*interval);
  }, 2);
  auto self = std::make_shared<rt::Timeout>();
  *self = rt::setTimeout([log, self] {
    log->push_back(at("self-clear"));
    rt::clearTimeout(*self);
  }, 3);
  auto b = std::make_shared<rt::Timeout>();
  rt::setTimeout([log, b] {
    log->push_back(at("A clears B"));
    rt::clearTimeout(*b);
  }, 4);
  *b = rt::setTimeout([log] { log->push_back("B-should-not-run"); }, 4);
  co_await rt::sleep(30);
  EXPECT_EQ(*log, (Strings{"overflow-fired@1", "1.7@1", "1.2@1", "2@2", "iv1@2", "self-clear@3", "A clears B@4", "iv2@4", "iv3@6"}));
  EXPECT(max.active());
  EXPECT(!interval->active());
}

ASYNC_TEST("bun timers2: handles have numeric ids; clearTimeout and clearInterval are interchangeable; hasRef follows the timer's life") {
  auto log = makeLog();
  auto a = rt::setTimeout([] {}, 10);
  auto b = rt::setTimeout([] {}, 10);
  auto c = rt::setInterval([] {}, 10);
  EXPECT_EQ(b.id(), a.id() + 1);
  EXPECT_EQ(c.id(), b.id() + 1);
  rt::clearTimeout(b);
  rt::clearInterval(c);
  rt::clearTimeout(a);
  auto d = rt::setTimeout([log] { log->push_back("d fired"); }, 5);
  rt::clearInterval(d);
  auto e = std::make_shared<rt::Timeout>();
  *e = rt::setInterval([log, e] {
    log->push_back(at("e fired"));
    rt::clearTimeout(*e);
  }, 5);
  auto t = std::make_shared<rt::Timeout>();
  *t = rt::setTimeout([log, t] { log->push_back("fired, hasRef=" + std::string(t->hasRef() ? "true" : "false")); }, 5);
  auto u = rt::setTimeout([] {}, 5);
  rt::clearTimeout(u);
  EXPECT(!u.hasRef());
  EXPECT(!u.active());
  rt::clearTimeout(u);
  rt::clearTimeout(rt::Timeout());
  rt::clearTimeout(std::optional<rt::Timeout>());
  co_await rt::sleep(20);
  EXPECT_EQ(*log, (Strings{"e fired@5", "fired, hasRef=true"}));
  EXPECT(!t->hasRef());
  EXPECT(!t->active());
}

ASYNC_TEST("bun timers2 refresh: refresh() restarts the countdown of an armed timer") {
  auto log = makeLog();
  auto timer = std::make_shared<rt::Timeout>();
  *timer = rt::setTimeout([log] { log->push_back(at("refreshed fired")); }, 20);
  rt::setTimeout([log, timer] {
    log->push_back(at("refresh"));
    timer->refresh();
  }, 10);
  co_await rt::sleep(40);
  EXPECT_EQ(*log, (Strings{"refresh@10", "refreshed fired@30"}));
  timer->refresh();
  EXPECT(!timer->active());
  co_await rt::sleep(25);
  EXPECT_EQ(log->size(), 2u);
}

TEST("bun timers2 unref: an unreferenced timer does not keep Loop::run alive; ref() restores it; an unref'd interval runs while a referenced timer waits") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  rt::LoopScope scope(loop);
  auto log = makeLog();
  auto unreferenced = rt::setTimeout([log] { log->push_back("unref-fired"); }, 50);
  unreferenced.unref();
  loop.run();
  EXPECT(log->empty());
  EXPECT_EQ(clock.monotonicNow(), 0.0);
  rt::clearTimeout(unreferenced);
  auto timer = rt::setTimeout([log] { log->push_back(at("reref-fired")); }, 50);
  timer.unref();
  EXPECT(!timer.hasRef());
  timer.ref();
  EXPECT(timer.hasRef());
  loop.run();
  EXPECT_EQ(*log, (Strings{"reref-fired@50"}));
  log->clear();
  auto interval = rt::setInterval([log] { log->push_back(at("iv")); }, 10);
  interval.unref();
  rt::setTimeout([log] { log->push_back(at("ref-timer-35")); }, 35);
  loop.run();
  EXPECT_EQ(*log, (Strings{"iv@60", "iv@70", "iv@80", "ref-timer-35@85"}));
}

TEST("FakeClock: Date.now and performance.now advance with the timers; sleep jumps over idle time") {
  rt::FakeClock clock(1700000000000.0, 100);
  rt::Loop loop(clock);
  auto [date0, perf0, date1, perf1] = rt::blockOn(loop, rt::async([]() -> rt::Task<std::tuple<double, double, double, double>> {
    double startDate = rt::dateNow();
    double startPerf = rt::performanceNow();
    co_await rt::sleep(5000);
    co_return std::tuple<double, double, double, double>{startDate, startPerf, rt::dateNow(), rt::performanceNow()};
  }));
  EXPECT_EQ(date0, 1700000000000.0);
  EXPECT_EQ(perf0, 100.0);
  EXPECT_EQ(date1, 1700000005000.0);
  EXPECT_EQ(perf1, 5100.0);
  clock.advance(0.5);
  EXPECT_EQ(clock.dateNow(), 1700000005000.0);
  EXPECT_EQ(clock.performanceNow(), 5100.5);
}

ASYNC_TEST("globalDateNow replaces Date.now process-wide, like `Date.now = () => clock` in the TS tests") {
  auto realNow = rt::globalDateNow();
  double clock = 42;
  rt::globalDateNow() = [&clock] { return clock; };
  EXPECT_EQ(rt::dateNow(), 42.0);
  clock = 43;
  EXPECT_EQ(rt::dateNow(), 43.0);
  rt::globalDateNow() = realNow;
  EXPECT_EQ(rt::dateNow(), rt::test::clock().dateNow());
  co_return;
}

ASYNC_TEST("globalSetTimeout/globalClearTimeout intercept timers, like `globalThis.setTimeout = ...` in the TS tests") {
  auto originalSetTimeout = rt::globalSetTimeout();
  auto originalClearTimeout = rt::globalClearTimeout();
  auto armed = std::make_shared<std::set<double>>();
  std::function<void()> leaseExpiry;
  rt::globalSetTimeout() = [&, armed](std::function<void()> handler, double timeout) {
    if (timeout >= 60000) {
      leaseExpiry = handler;
      return originalSetTimeout([] {}, timeout);
    }
    auto handle = originalSetTimeout(std::move(handler), timeout);
    armed->insert(handle.id());
    return handle;
  };
  rt::globalClearTimeout() = [&, armed](const rt::Timeout& handle) {
    armed->erase(handle.id());
    originalClearTimeout(handle);
  };
  auto log = makeLog();
  auto first = rt::setTimeout([log] { log->push_back("short"); }, 10);
  auto lease = rt::setTimeout([log] { log->push_back("never-called-directly"); }, 300000);
  EXPECT_EQ(armed->size(), 1u);
  rt::clearTimeout(first);
  EXPECT_EQ(armed->size(), 0u);
  ASSERT(static_cast<bool>(leaseExpiry));
  leaseExpiry();
  rt::clearTimeout(lease);
  rt::globalSetTimeout() = originalSetTimeout;
  rt::globalClearTimeout() = originalClearTimeout;
  EXPECT_EQ(*log, (Strings{"never-called-directly"}));
  co_return;
}

TEST("SystemClock: timers wait for real time and Date.now is whole milliseconds") {
  rt::Loop loop;
  EXPECT(!loop.clock().simulated());
  double date = loop.clock().dateNow();
  EXPECT_EQ(date, std::floor(date));
  auto elapsed = rt::blockOn(loop, rt::async([]() -> rt::Task<double> {
    double start = rt::performanceNow();
    co_await rt::sleep(5);
    co_return rt::performanceNow() - start;
  }));
  EXPECT(elapsed >= 4.5);
  EXPECT(elapsed < 1000);
}

TEST("FakeClock: while an IO operation is pending the next fake timer waits for real time instead of jumping") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  auto log = makeLog();
  const auto realStart = std::chrono::steady_clock::now();
  auto realMs = [realStart] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - realStart).count(); };
  double realAtTimer = -1;
  rt::blockOn(loop, rt::async([&]() -> rt::Task<void> {
    boost::asio::steady_timer io(rt::executor());
    io.expires_after(std::chrono::milliseconds(40));
    rt::setTimeout([&] {
      realAtTimer = realMs();
      log->push_back(at("fake-10"));
    }, 10);
    co_await io.async_wait(boost::asio::use_awaitable);
    log->push_back("io-40");
  }));
  EXPECT_EQ(*log, (Strings{"fake-10@10", "io-40"}));
  EXPECT(realAtTimer >= 9.0);
  EXPECT(realMs() >= 39.0);
}
