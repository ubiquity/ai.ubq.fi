// rt::Task / rt::Promise / microtasks / combinators / generators. Expected
// orders marked "bun" were recorded by running the same scenario under Bun 1.4.
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/weak.hpp"
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace uos;

namespace {
  using Strings = std::vector<std::string>;
  using Log = std::shared_ptr<Strings>;
  Log makeLog() { return std::make_shared<Strings>(); }

  // `(async () => { for (let i = 1; i <= n; i++) { await null; log.push(prefix + i); } })()`
  void ticks(const Log& log, int n, std::string prefix = "tick") {
    rt::spawn(rt::async([log, n, prefix]() -> rt::Task<void> {
      for (int i = 1; i <= n; ++i) {
        co_await rt::microtask();
        log->push_back(prefix + std::to_string(i));
      }
    }));
  }

  rt::Task<int> inner(Log log) {
    log->push_back("inner-body");
    co_return 1;
  }
  rt::Task<int> innerAwait(Log log) {
    log->push_back("innerAwait-body");
    co_await rt::microtask();
    log->push_back("innerAwait-resumed");
    co_return 2;
  }
  // `async () => { ...; return Promise.resolve(3); }`: returning a promise from an
  // async function settles it two turns after that promise.
  rt::Task<int> returnsPromise(Log log) {
    log->push_back("rp-body");
    auto promise = rt::Promise<int>::resolve(3);
    co_await rt::microtask();
    int value = co_await promise.wait();
    co_return value;
  }

  rt::Task<int> failWith(std::string message) {
    co_await rt::microtask();
    throw rt::TypeError(std::move(message));
  }
}

ASYNC_TEST("bun order1: microtasks drain between timers; await of a settled promise takes one turn") {
  auto log = makeLog();
  rt::setTimeout([log] {
    log->push_back("t1");
    (void)rt::Promise<void>::resolve().then([log] { log->push_back("m1"); });
    rt::queueMicrotask([log] { log->push_back("q1"); });
  }, 0);
  rt::setTimeout([log] { log->push_back("t2"); }, 0);
  rt::setTimeout([log] { log->push_back("t3-1ms"); }, 1);
  rt::setTimeout([log] { log->push_back("t-neg"); }, -5);
  rt::setTimeout([log] { log->push_back("t-nan"); }, std::nan(""));
  auto settled = rt::Promise<int>::resolve(1);
  rt::spawn(rt::async([log, settled]() -> rt::Task<void> {
    log->push_back("a-start");
    co_await settled.wait();
    log->push_back("a-after1");
    co_await settled.wait();
    log->push_back("a-after2");
  }));
  rt::spawn(rt::async([log]() -> rt::Task<void> {
    log->push_back("b-start");
    co_await rt::microtask();
    log->push_back("b-after1");
    co_await rt::microtask();
    log->push_back("b-after2");
  }));
  rt::queueMicrotask([log] { log->push_back("qm"); });
  (void)rt::Promise<void>::resolve().then([log] { log->push_back("then1"); }).then([log] { log->push_back("then2"); });
  log->push_back("sync-end");
  co_await rt::sleep(20);
  EXPECT_EQ(*log, (Strings{"a-start", "b-start", "sync-end", "a-after1", "b-after1", "qm", "then1", "a-after2", "b-after2", "then2", "t1", "m1", "q1", "t2", "t3-1ms", "t-neg", "t-nan"}));
}

ASYNC_TEST("bun order2: `await f()` is co_await rt::Promise(f()), or co_await f() then one microtask") {
  for (int form = 0; form < 2; ++form) {
    auto log = makeLog();
    auto await = [form](rt::Task<int> task) -> rt::Task<int> {
      if (form == 0) {
        int value = co_await rt::Promise(std::move(task)).wait();
        co_return value;
      }
      int value = co_await std::move(task);
      co_await rt::microtask();
      co_return value;
    };
    rt::spawn(rt::async([log, await]() -> rt::Task<void> {
      co_await await(inner(log));
      log->push_back("A:after-inner");
    }));
    rt::spawn(rt::async([log, await]() -> rt::Task<void> {
      co_await await(innerAwait(log));
      log->push_back("B:after-innerAwait");
    }));
    rt::spawn(rt::async([log, await]() -> rt::Task<void> {
      co_await await(returnsPromise(log));
      log->push_back("C:after-returnsPromise");
    }));
    ticks(log, 6);
    co_await rt::sleep(20);
    EXPECT_EQ(*log, (Strings{"inner-body", "innerAwait-body", "rp-body", "A:after-inner", "innerAwait-resumed", "tick1", "B:after-innerAwait", "tick2", "C:after-returnsPromise", "tick3",
                             "tick4", "tick5", "tick6"}));
  }
}

ASYNC_TEST("co_await of a Task runs the callee inline and resumes the caller without a microtask turn") {
  auto log = makeLog();
  ticks(log, 2);
  int value = co_await inner(log);
  log->push_back("after-inner=" + std::to_string(value));
  value = co_await innerAwait(log);
  log->push_back("after-innerAwait=" + std::to_string(value));
  co_await rt::sleep(1);
  EXPECT_EQ(*log, (Strings{"inner-body", "after-inner=1", "innerAwait-body", "tick1", "innerAwait-resumed", "after-innerAwait=2", "tick2"}));
}

ASYNC_TEST("bun order3: executor runs now, resolve(promise) adopts two turns later, all/race settle a turn after their inputs") {
  auto log = makeLog();
  auto p = rt::newPromise<int>([&](auto resolve, auto) {
    log->push_back("executor");
    resolve(1);
    log->push_back("after-resolve");
  });
  (void)p.then([log](int) { log->push_back("p.then"); });
  auto q = rt::newPromise<int>([](auto resolve, auto) { resolve(rt::Promise<int>::resolve(2)); });
  (void)q.then([log](int) { log->push_back("q.then(resolved-with-promise)"); });
  ticks(log, 5);
  (void)rt::all(std::vector<rt::Promise<int>>{rt::Promise<int>::resolve(1), rt::Promise<int>::resolve(2)}).then([log](const std::vector<int>& values) {
    log->push_back("all:[" + std::to_string(values[0]) + "," + std::to_string(values[1]) + "]");
  });
  (void)rt::race(rt::Promise<std::string>::resolve("r1"), rt::Promise<std::string>::resolve("r2")).then([log](const std::string& value) { log->push_back("race:" + value); });
  co_await rt::sleep(20);
  EXPECT_EQ(*log, (Strings{"executor", "after-resolve", "p.then", "tick1", "tick2", "all:[1,2]", "race:r1", "q.then(resolved-with-promise)", "tick3", "tick4", "tick5"}));
}

ASYNC_TEST("bun: then/catch/finally turns (then1.ts)") {
  auto log = makeLog();
  auto source = rt::Promise<int>::resolve(1);
  (void)source.then([log](int) { log->push_back("then"); });
  (void)source.finally_([log] { log->push_back("finally-callback"); }).then([log](int value) { log->push_back("after-finally=" + std::to_string(value)); });
  (void)rt::Promise<int>::reject(rt::thrown(json::value(5))).catch_([log](const rt::Reason& reason) {
    log->push_back("catch=" + rt::reasonToString(reason));
    return 6;
  });
  (void)source.then([](int value) -> rt::Task<int> { co_return value + 1; }).then([log](int value) { log->push_back("async-then=" + std::to_string(value)); });
  ticks(log, 5);
  co_await rt::sleep(1);
  EXPECT_EQ(*log, (Strings{"then", "finally-callback", "catch=5", "tick1", "after-finally=1", "tick2", "tick3", "async-then=2", "tick4", "tick5"}));
}

ASYNC_TEST("bun: finally/then/resolve variants settle in Bun's turns (fin.ts)") {
  struct Case {
    std::string label;
    std::function<void(const Log&)> make;
    Strings expected;
  };
  std::vector<Case> cases;
  auto g = [](const Log& log) { return [log](int value) { log->push_back("g=" + std::to_string(value)); }; };
  auto gr = [](const Log& log) { return [log](const rt::Reason& reason) { log->push_back("g=" + rt::reasonToString(reason)); }; };
  cases.push_back({"sync-finally", [g](const Log& log) { (void)rt::Promise<int>::resolve(1).finally_([log] { log->push_back("f"); }).then(g(log)); }, {"f", "t1", "g=1", "t2", "t3"}});
  cases.push_back({"async-finally",
                   [g](const Log& log) {
                     (void)rt::Promise<int>::resolve(1)
                         .finally_([log]() -> rt::Task<void> {
                           log->push_back("f");
                           co_return;
                         })
                         .then(g(log));
                   },
                   {"f", "t1", "t2", "g=1", "t3"}});
  cases.push_back({"reject-finally", [gr](const Log& log) { (void)rt::Promise<int>::reject(rt::thrown(json::value(2))).finally_([log] { log->push_back("f"); }).catch_(gr(log)); }, {"f", "t1", "g=2", "t2", "t3"}});
  cases.push_back({"throw-finally",
                   [gr](const Log& log) {
                     (void)rt::Promise<int>::resolve(1)
                         .finally_([log] {
                           log->push_back("f");
                           rt::rethrow(rt::thrown(json::value(3)));
                         })
                         .catch_(gr(log));
                   },
                   {"f", "t1", "g=3", "t2", "t3"}});
  cases.push_back({"then-plain",
                   [g](const Log& log) {
                     (void)rt::Promise<int>::resolve(1)
                         .then([log](int value) {
                           log->push_back("f");
                           return value;
                         })
                         .then(g(log));
                   },
                   {"f", "t1", "g=1", "t2", "t3"}});
  cases.push_back({"then-returns-promise",
                   [g](const Log& log) {
                     (void)rt::Promise<int>::resolve(1)
                         .then([log](int value) {
                           log->push_back("f");
                           return rt::Promise<int>::resolve(value);
                         })
                         .then(g(log));
                   },
                   {"f", "t1", "t2", "t3", "g=1", "t4"}});
  cases.push_back({"then-async",
                   [g](const Log& log) {
                     (void)rt::Promise<int>::resolve(1)
                         .then([log](int value) -> rt::Task<int> {
                           log->push_back("f");
                           co_return value;
                         })
                         .then(g(log));
                   },
                   {"f", "t1", "t2", "t3", "g=1", "t4"}});
  cases.push_back({"catch-returns",
                   [g](const Log& log) {
                     (void)rt::Promise<int>::reject(rt::thrown(json::value(1)))
                         .catch_([log](const rt::Reason&) {
                           log->push_back("f");
                           return 7;
                         })
                         .then(g(log));
                   },
                   {"f", "t1", "g=7", "t2", "t3"}});
  cases.push_back({"new-promise-resolve-promise", [g](const Log& log) { (void)rt::newPromise<int>([](auto resolve, auto) { resolve(rt::Promise<int>::resolve(4)); }).then(g(log)); }, {"t1", "t2", "g=4", "t3"}});
  cases.push_back({"withResolvers-resolve-promise",
                   [g](const Log& log) {
                     auto resolvers = rt::withResolvers<int>();
                     (void)resolvers.promise.then(g(log));
                     resolvers.resolve(rt::Promise<int>::resolve(5));
                   },
                   {"t1", "t2", "g=5", "t3"}});
  cases.push_back({"async-return-value", [g](const Log& log) { (void)rt::Promise(rt::async([]() -> rt::Task<int> { co_return 6; })).then(g(log)); }, {"g=6", "t1", "t2", "t3"}});
  for (const auto& entry : cases) {
    auto log = makeLog();
    entry.make(log);
    ticks(log, static_cast<int>(entry.expected.size()), "t");
    co_await rt::sleep(5);
    log->resize(entry.expected.size());
    if (*log != entry.expected) std::fprintf(stderr, "    case %s\n", entry.label.c_str());
    EXPECT_EQ(*log, entry.expected);
  }
}

ASYNC_TEST("a promise is a value: stored in a map, awaited by several consumers, before and after it settles") {
  auto resolvers = rt::withResolvers<std::string>();
  std::map<std::string, rt::Promise<std::string>> inflight;
  inflight.emplace("key", resolvers.promise);
  auto log = makeLog();
  for (int consumer = 1; consumer <= 2; ++consumer) {
    rt::spawn(rt::async([log, consumer, promise = inflight.at("key")]() -> rt::Task<void> {
      std::string value = co_await promise.wait();
      log->push_back("consumer" + std::to_string(consumer) + "=" + value);
    }));
  }
  EXPECT(inflight.at("key").pending());
  co_await rt::sleep(5);
  EXPECT_EQ(log->size(), 0u);
  resolvers.resolve("done");
  resolvers.resolve("ignored");
  resolvers.reject(rt::makeReason(rt::TypeError("ignored")));
  EXPECT(inflight.at("key").fulfilled());
  EXPECT_EQ(inflight.at("key").value(), "done");
  co_await rt::microtask();
  EXPECT_EQ(*log, (Strings{"consumer1=done", "consumer2=done"}));
  std::string again = co_await inflight.at("key").wait();
  EXPECT_EQ(again, "done");
  again = co_await inflight.at("key").wait();
  EXPECT_EQ(again, "done");
}

ASYNC_TEST("spawn runs a fire-and-forget call synchronously up to its first suspension") {
  auto log = makeLog();
  rt::spawn(rt::async([log]() -> rt::Task<void> {
    log->push_back("started");
    co_await rt::sleep(10);
    log->push_back("finished");
  }));
  log->push_back("after-spawn");
  co_await rt::sleep(10);
  log->push_back("caller-woke");
  EXPECT_EQ(*log, (Strings{"started", "after-spawn", "finished", "caller-woke"}));
}

ASYNC_TEST("exceptions propagate through co_await as the thrown rt::JsError") {
  EXPECT_REJECTS_MESSAGE(failWith("boom"), rt::TypeError, "boom");
  bool caught = false;
  try {
    co_await rt::Promise(failWith("again")).wait();
  } catch (const rt::JsError& error) {
    caught = true;
    EXPECT_EQ(error.name, "TypeError");
    EXPECT_EQ(error.message, "again");
  }
  EXPECT(caught);
  auto rejected = rt::Promise<int>::reject(nullptr);
  bool undefinedThrown = false;
  try {
    co_await rejected.wait();
  } catch (const rt::ThrownValue& value) {
    undefinedThrown = !value.value.has_value();
  }
  EXPECT(undefinedThrown);
}

ASYNC_TEST("rt::all rejects with the first rejection while the other inputs keep running") {
  auto log = makeLog();
  auto slow = rt::Promise(rt::async([log]() -> rt::Task<int> {
    co_await rt::sleep(10);
    log->push_back("slow-finished");
    co_return 2;
  }));
  std::vector<rt::Promise<int>> inputs{rt::Promise(failWith("first")), slow, rt::Promise(failWith("second"))};
  EXPECT_REJECTS_MESSAGE(rt::all(inputs).wait(), rt::TypeError, "first");
  EXPECT(slow.pending());
  co_await rt::sleep(10);
  EXPECT_EQ(*log, (Strings{"slow-finished"}));
  inputs = {rt::Promise<int>::resolve(1), slow};
  auto values = co_await rt::all(inputs).wait();
  EXPECT_EQ(values, (std::vector<int>{1, 2}));
  auto empty = co_await rt::all(std::vector<rt::Promise<int>>()).wait();
  EXPECT(empty.empty());
  std::vector<rt::Promise<void>> voids{rt::Promise<void>::resolve()};
  co_await rt::all(voids).wait();
  auto tuple = co_await rt::all(rt::Promise<int>::resolve(7), rt::Promise<void>::resolve(), rt::Promise<std::string>::resolve(std::string("x"))).wait();
  EXPECT_EQ(std::get<0>(tuple), 7);
  EXPECT(std::get<1>(tuple) == std::monostate{});
  EXPECT_EQ(std::get<2>(tuple), "x");
}

ASYNC_TEST("rt::allSettled, rt::race and rt::any follow Promise.allSettled/race/any") {
  std::vector<rt::Promise<int>> inputs{rt::Promise<int>::resolve(1), rt::Promise(failWith("no"))};
  auto settled = co_await rt::allSettled(inputs).wait();
  ASSERT_EQ(settled.size(), 2u);
  EXPECT_EQ(settled[0].status, "fulfilled");
  EXPECT_EQ(*settled[0].value, 1);
  EXPECT_EQ(settled[1].status, "rejected");
  EXPECT_EQ(rt::reasonToString(settled[1].reason), "TypeError: no");

  auto loser = rt::Promise(rt::async([]() -> rt::Task<int> {
    co_await rt::sleep(50);
    co_return 2;
  }));
  auto winner = rt::Promise(rt::async([]() -> rt::Task<int> {
    co_await rt::sleep(5);
    co_return 1;
  }));
  int first = co_await rt::race(loser, winner).wait();
  EXPECT_EQ(first, 1);
  EXPECT(loser.pending());
  co_await rt::sleep(50);
  EXPECT(loser.fulfilled());

  inputs = {rt::Promise(failWith("a")), rt::Promise<int>::resolve(9)};
  int any = co_await rt::any(inputs).wait();
  EXPECT_EQ(any, 9);
  bool aggregated = false;
  inputs = {rt::Promise(failWith("a")), rt::Promise<int>::reject(rt::thrown(json::value("b")))};
  try {
    co_await rt::any(inputs).wait();
  } catch (const rt::AggregateError& error) {
    aggregated = true;
    EXPECT_EQ(error.name, "AggregateError");
    EXPECT_EQ(error.message, "");
    ASSERT_EQ(error.errors.size(), 2u);
    EXPECT_EQ(rt::reasonToString(error.errors[0]), "TypeError: a");
    EXPECT_EQ(rt::reasonToString(error.errors[1]), "b");
  }
  EXPECT(aggregated);
  EXPECT_REJECTS_MESSAGE(rt::any(std::vector<rt::Promise<int>>()).wait(), rt::AggregateError, "");
}

TEST("an unhandled rejection is raised at the end of the checkpoint; one handled in the same checkpoint is not") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  auto handledLater = rt::blockOn(loop, rt::async([]() -> rt::Task<bool> {
    auto rejected = rt::Promise<int>::reject(rt::makeReason(rt::JsError("late")));
    rt::queueMicrotask([rejected] { (void)rejected.catch_(rt::ignoreRejection); });
    co_await rt::sleep(1);
    co_return true;
  }));
  EXPECT(handledLater);
  bool raised = false;
  try {
    rt::blockOn(loop, rt::async([]() -> rt::Task<void> {
      rt::spawn(failWith("unhandled"));
      co_await rt::sleep(5);
    }));
  } catch (const rt::UncaughtError& error) {
    raised = true;
    EXPECT(error.rejection);
    EXPECT_EQ(error.report(), "TypeError: unhandled");
  }
  EXPECT(raised);
}

namespace {
  std::string stepJson(const rt::IteratorResult<int>& step) { return step.done ? std::string("{\"done\":true}") : "{\"value\":" + std::to_string(*step.value) + ",\"done\":false}"; }

  // gen.ts: try { "body-start"; yield 1; "after-yield-1"; await sleep(5); yield 2; "after-yield-2" } finally { "finally" }
  rt::Task<void> genBody(Log log, rt::Yield<int> yield) {
    auto cleanup = rt::scopeExit([log] { log->push_back("finally"); });
    log->push_back("body-start");
    co_await yield(1);
    log->push_back("after-yield-1");
    co_await rt::sleep(5);
    co_await yield(2);
    log->push_back("after-yield-2");
  }
  rt::AsyncGenerator<int> makeGen(Log log) {
    return rt::AsyncGenerator<int>([log](rt::Yield<int> yield) { return genBody(log, std::move(yield)); });
  }
}

ASYNC_TEST("bun gen.ts: generator requests queue; return() waits for the running body; throw rejects once then done; yield* forwards return") {
  auto log = makeLog();
  auto g = makeGen(log);
  auto n1 = g.next();
  log->push_back("n1-called");
  auto step = co_await std::move(n1);
  log->push_back("n1=" + stepJson(step));
  auto n2 = g.next();
  auto r = g.return_();
  log->push_back("n2-r-called");
  step = co_await std::move(n2);
  log->push_back("n2=" + stepJson(step));
  step = co_await std::move(r);
  log->push_back("r=" + stepJson(step));
  step = co_await g.next();
  log->push_back("n3=" + stepJson(step));
  auto g2 = makeGen(log);
  step = co_await g2.return_();
  log->push_back("g2.return=" + stepJson(step));
  rt::AsyncGenerator<int> g3([log](rt::Yield<int> yield) -> rt::Task<void> {
    log->push_back("thrower-start");
    co_await yield(1);
    throw rt::TypeError("gen-fail");
  });
  step = co_await g3.next();
  log->push_back("g3.n1=" + stepJson(step));
  try {
    co_await g3.next();
  } catch (const rt::JsError& error) {
    log->push_back("g3.n2 threw " + error.toString());
  }
  step = co_await g3.next();
  log->push_back("g3.n3=" + stepJson(step));
  rt::AsyncGenerator<int> outer([log](rt::Yield<int> yield) -> rt::Task<void> {
    auto cleanup = rt::scopeExit([log] { log->push_back("outer-finally"); });
    auto delegate = makeGen(log);
    co_await yield.from(delegate);
  });
  auto loopBody = [log](int value) -> rt::Task<bool> {
    log->push_back("for-await " + std::to_string(value));
    co_return false;
  };
  co_await rt::forAwait(outer, loopBody);
  EXPECT_EQ(*log, (Strings{"body-start", "n1-called", "n1={\"value\":1,\"done\":false}", "after-yield-1", "n2-r-called", "n2={\"value\":2,\"done\":false}", "finally", "r={\"done\":true}",
                           "n3={\"done\":true}", "g2.return={\"done\":true}", "thrower-start", "g3.n1={\"value\":1,\"done\":false}", "g3.n2 threw TypeError: gen-fail", "g3.n3={\"done\":true}",
                           "body-start", "for-await 1", "finally", "outer-finally"}));
}

ASYNC_TEST("bun gen2.ts: generator steps settle in Bun's turns") {
  struct Case {
    std::string label;
    std::function<void(const Log&)> make;
    Strings expected;
  };
  std::vector<Case> cases;
  cases.push_back({"first-next",
                   [](const Log& log) {
                     auto g = rt::AsyncGenerator<int>([log](rt::Yield<int> yield) -> rt::Task<void> {
                       log->push_back("body");
                       co_await yield(1);
                     });
                     rt::spawn(rt::async([log, g]() -> rt::Task<void> {
                       co_await g.next();
                       log->push_back("next");
                     }));
                   },
                   {"body", "t1", "next", "t2"}});
  cases.push_back({"second-next",
                   [](const Log& log) {
                     auto g = rt::AsyncGenerator<int>([log](rt::Yield<int> yield) -> rt::Task<void> {
                       co_await yield(1);
                       log->push_back("body2");
                       co_await yield(2);
                     });
                     rt::spawn(rt::async([log, g]() -> rt::Task<void> {
                       co_await g.next();
                       log->push_back("n1");
                       co_await g.next();
                       log->push_back("n2");
                     }));
                   },
                   {"t1", "n1", "body2", "t2", "t3", "n2", "t4"}});
  cases.push_back({"return-at-start",
                   [](const Log& log) {
                     auto g = rt::AsyncGenerator<int>([](rt::Yield<int> yield) -> rt::Task<void> { co_await yield(1); });
                     rt::spawn(rt::async([log, g]() -> rt::Task<void> {
                       co_await g.return_();
                       log->push_back("ret");
                     }));
                   },
                   {"t1", "ret", "t2"}});
  cases.push_back({"next-after-done",
                   [](const Log& log) {
                     auto g = rt::AsyncGenerator<int>([](rt::Yield<int>) -> rt::Task<void> { co_return; });
                     rt::spawn(rt::async([log, g]() -> rt::Task<void> {
                       co_await g.next();
                       log->push_back("n1");
                       co_await g.next();
                       log->push_back("n2");
                     }));
                   },
                   {"n1", "t1", "n2", "t2"}});
  cases.push_back({"return-suspended",
                   [](const Log& log) {
                     auto g = rt::AsyncGenerator<int>([log](rt::Yield<int> yield) -> rt::Task<void> {
                       auto cleanup = rt::scopeExit([log] { log->push_back("finally"); });
                       co_await yield(1);
                     });
                     rt::spawn(rt::async([log, g]() -> rt::Task<void> {
                       co_await g.next();
                       log->push_back("n1");
                       co_await g.return_();
                       log->push_back("ret");
                     }));
                   },
                   {"t1", "n1", "t2", "finally", "t3", "ret", "t4"}});
  cases.push_back({"throw-in-body",
                   [](const Log& log) {
                     auto g = rt::AsyncGenerator<int>([log](rt::Yield<int>) -> rt::Task<void> {
                       log->push_back("body");
                       throw rt::JsError("x");
                       co_return;
                     });
                     rt::spawn(rt::async([log, g]() -> rt::Task<void> {
                       try {
                         co_await g.next();
                       } catch (const rt::JsError&) {
                         log->push_back("rejected");
                       }
                     }));
                   },
                   {"body", "rejected", "t1", "t2"}});
  cases.push_back({"body-returns",
                   [](const Log& log) {
                     auto g = rt::AsyncGenerator<int, int>([log](rt::Yield<int>) -> rt::Task<int> {
                       log->push_back("body");
                       co_return 5;
                     });
                     rt::spawn(rt::async([log, g]() -> rt::Task<void> {
                       auto step = co_await g.next();
                       log->push_back("next done=" + std::string(step.done ? "true" : "false") + " value=" + std::to_string(g.returnValue().value_or(-1)));
                     }));
                   },
                   {"body", "t1", "next done=true value=5", "t2"}});
  for (const auto& entry : cases) {
    auto log = makeLog();
    entry.make(log);
    ticks(log, static_cast<int>(entry.expected.size()), "t");
    co_await rt::sleep(5);
    log->resize(entry.expected.size());
    if (*log != entry.expected) std::fprintf(stderr, "    case %s\n", entry.label.c_str());
    EXPECT_EQ(*log, entry.expected);
  }
}

ASYNC_TEST("forAwait: a throwing body awaits return() and rethrows the body's error") {
  auto log = makeLog();
  bool caught = false;
  auto throwing = [log](int value) -> rt::Task<void> {
    log->push_back("body " + std::to_string(value));
    throw rt::RangeError("stop");
    co_return;
  };
  try {
    co_await rt::forAwait(makeGen(log), throwing);
  } catch (const rt::RangeError& error) {
    caught = error.message == "stop";
  }
  EXPECT(caught);
  EXPECT_EQ(*log, (Strings{"body-start", "body 1", "finally"}));
  log->clear();
  auto collect = [log](int value) -> rt::Task<void> {
    log->push_back("body " + std::to_string(value));
    co_return;
  };
  co_await rt::forAwait(makeGen(log), collect);
  EXPECT_EQ(*log, (Strings{"body-start", "body 1", "after-yield-1", "body 2", "after-yield-2", "finally"}));
}

ASYNC_TEST("dropping a suspended generator destroys its frame without resuming it") {
  auto log = makeLog();
  {
    rt::AsyncGenerator<int> g([log](rt::Yield<int> yield) -> rt::Task<void> {
      auto cleanup = rt::scopeExit([log] { log->push_back("frame-destroyed"); });
      try {
        co_await yield(1);
        log->push_back("resumed");
      } catch (...) {
        log->push_back("caught");
        throw;
      }
    });
    auto step = co_await g.next();
    EXPECT(!step.done);
  }
  co_await rt::sleep(1);
  EXPECT_EQ(*log, (Strings{"frame-destroyed"}));
}

ASYNC_TEST("withFinally runs the cleanup after success, failure and early return; a cleanup error replaces the outcome") {
  auto log = makeLog();
  auto cleanup1 = [log]() -> rt::Task<void> {
    co_await rt::microtask();
    log->push_back("cleanup-1");
  };
  int value = co_await rt::withFinally(inner(log), cleanup1);
  EXPECT_EQ(value, 1);
  auto cleanup2 = [log] { log->push_back("cleanup-2"); };
  EXPECT_REJECTS_MESSAGE(rt::withFinally(failWith("body"), cleanup2), rt::TypeError, "body");
  auto failingCleanup = [] { throw rt::RangeError("cleanup"); };
  EXPECT_REJECTS_MESSAGE(rt::withFinally(inner(log), failingCleanup), rt::RangeError, "cleanup");
  auto early = [](Log trace, bool stop) -> rt::Task<int> {
    if (stop) co_return 0;
    trace->push_back("not-stopped");
    co_return 1;
  };
  auto cleanup3 = [log] { log->push_back("cleanup-3"); };
  value = co_await rt::withFinally(early(log, true), cleanup3);
  EXPECT_EQ(value, 0);
  EXPECT_EQ(*log, (Strings{"inner-body", "cleanup-1", "cleanup-2", "inner-body", "cleanup-3"}));
}

ASYNC_TEST("rt::async keeps a coroutine lambda's captures alive across suspensions") {
  auto task = [] {
    std::string captured = "still-alive";
    return rt::async([captured]() -> rt::Task<std::string> {
      co_await rt::sleep(5);
      co_return captured;
    });
  }();
  std::string result = co_await std::move(task);
  EXPECT_EQ(result, "still-alive");
}

TEST("reason helpers follow String(), instanceof Error and Bun's uncaught report line (bun unh1/unh5/unh14)") {
  using rt::thrown;
  EXPECT_EQ(rt::reasonToString(nullptr), "undefined");
  EXPECT_EQ(rt::reasonToString(thrown(json::value(nullptr))), "null");
  EXPECT_EQ(rt::reasonToString(thrown(json::value(42))), "42");
  EXPECT_EQ(rt::reasonToString(thrown(json::value(1.5))), "1.5");
  EXPECT_EQ(rt::reasonToString(thrown(json::value("str"))), "str");
  EXPECT_EQ(rt::reasonToString(thrown(json::parse(R"({"a":1})"))), "[object Object]");
  EXPECT_EQ(rt::reasonToString(thrown(json::parse(R"([1,null,"x",[2,3]])"))), "1,,x,2,3");
  EXPECT_EQ(rt::reasonToString(rt::makeReason(rt::JsError("m"))), "Error: m");
  EXPECT_EQ(rt::reasonToString(rt::makeReason(rt::JsError(""))), "Error");
  EXPECT_EQ(rt::reasonToString(rt::makeReason(rt::AggregateError({}, ""))), "AggregateError");
  EXPECT(!rt::isError(nullptr));
  EXPECT(!rt::isError(thrown(json::value(1))));
  EXPECT(rt::isError(rt::makeReason(rt::TypeError("t"))));
  EXPECT_EQ(rt::errorName(rt::makeReason(rt::TypeError("t"))), std::optional<std::string>("TypeError"));
  EXPECT_EQ(rt::errorMessage(rt::makeReason(rt::TypeError("t"))), std::optional<std::string>("t"));
  EXPECT_EQ(rt::errorName(thrown(json::value(1))), std::nullopt);
  EXPECT_EQ(rt::thrownValue(thrown(json::value(42))), std::optional<json::value>(json::value(42)));
  EXPECT_EQ(rt::thrownValue(rt::makeReason(rt::JsError("e"))), std::nullopt);
  auto report = [](rt::Reason reason) { return rt::UncaughtError(std::move(reason), true).report(); };
  EXPECT_EQ(report(nullptr), "error: undefined");
  EXPECT_EQ(report(thrown(json::value(nullptr))), "error: null");
  EXPECT_EQ(report(thrown(json::value(42))), "error: 42");
  EXPECT_EQ(report(thrown(json::value("plain-string-reason"))), "error: plain-string-reason");
  EXPECT_EQ(report(thrown(json::parse(R"({"a":1})"))), "error");
  EXPECT_EQ(report(rt::makeReason(rt::JsError("boom-unhandled"))), "error: boom-unhandled");
  EXPECT_EQ(report(rt::makeReason(rt::JsError(""))), "Error: ");
  EXPECT_EQ(report(rt::makeReason(rt::JsError("over", "QuotaError"))), "QuotaError: over");
  EXPECT_EQ(report(rt::makeReason(rt::RangeError("r"))), "RangeError: r");
  EXPECT_EQ(report(rt::makeReason(rt::TypeError("async-fail"))), "TypeError: async-fail");
}

TEST("UncaughtPolicy::report prints the report line and keeps the loop running (bun with a pending top-level await)") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  loop.setUncaughtPolicy(rt::UncaughtPolicy::report);
  EXPECT(loop.uncaughtPolicy() == rt::UncaughtPolicy::report);
  auto log = makeLog();
  int result = rt::blockOn(loop, rt::async([log]() -> rt::Task<int> {
    rt::spawn(failWith("first"));
    rt::setTimeout([] { throw rt::JsError("from-timer"); }, 1);
    rt::queueMicrotask([] { throw rt::RangeError("from-microtask"); });
    co_await rt::sleep(10);
    log->push_back("continued");
    co_return 7;
  }));
  EXPECT_EQ(result, 7);
  EXPECT_EQ(loop.uncaughtCount(), 3u);
  EXPECT_EQ(*log, (Strings{"continued"}));
}

TEST("UncaughtPolicy::raise: an exception escaping a timer callback ends blockOn with UncaughtError (bun unh4)") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  auto log = makeLog();
  bool raised = false;
  try {
    rt::blockOn(loop, rt::async([log]() -> rt::Task<void> {
      rt::setTimeout([] { throw rt::JsError("thrown-in-timer"); }, 0);
      rt::setTimeout([log] { log->push_back("second-timer"); }, 20);
      co_await rt::sleep(30);
    }));
  } catch (const rt::UncaughtError& error) {
    raised = true;
    EXPECT(!error.rejection);
    EXPECT_EQ(error.report(), "error: thrown-in-timer");
  }
  EXPECT(raised);
  EXPECT(log->empty());
}

TEST("blockOn throws std::logic_error when the loop runs out of work with the task pending, and inside a running loop") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  auto never = rt::withResolvers<int>();
  EXPECT_THROWS(rt::blockOn(loop, never.promise.wait()), std::logic_error);
  bool nested = rt::blockOn(loop, rt::async([&loop]() -> rt::Task<bool> {
    try {
      rt::blockOn(loop, rt::microtask());
    } catch (const std::logic_error&) {
      co_return true;
    }
    co_return false;
  }));
  EXPECT(nested);
}

TEST("Loop::run drains microtasks and referenced timers; unreferenced timers only fire while something else keeps it alive") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  rt::LoopScope scope(loop);
  auto log = makeLog();
  rt::setTimeout([log] { log->push_back("ref-10"); }, 10);
  rt::setTimeout([log] { log->push_back("unref-5"); }, 5).unref();
  rt::setTimeout([log] { log->push_back("unref-50"); }, 50).unref();
  rt::queueMicrotask([log] { log->push_back("micro"); });
  EXPECT_EQ(loop.timerCount(), 3u);
  EXPECT_EQ(loop.microtaskCount(), 1u);
  loop.run();
  EXPECT_EQ(*log, (Strings{"micro", "unref-5", "ref-10"}));
  EXPECT_EQ(loop.timerCount(), 1u);
  EXPECT_EQ(clock.monotonicNow(), 10.0);
}

TEST("destroying a loop destroys coroutines suspended on promises nobody can settle (scope guards run, catch blocks do not)") {
  auto log = makeLog();
  {
    rt::FakeClock clock;
    rt::Loop loop(clock);
    rt::LoopScope scope(loop);
    auto never = rt::withResolvers<void>();
    rt::spawn(rt::async([log, promise = never.promise]() -> rt::Task<void> {
      auto guard = rt::scopeExit([log] { log->push_back("guard"); });
      try {
        co_await promise.wait();
        log->push_back("resumed");
      } catch (...) {
        log->push_back("caught");
      }
    }));
    loop.run();
    EXPECT(log->empty());
  }
  EXPECT_EQ(*log, (Strings{"guard"}));
}

TEST("a promise created outside any running loop starts its task synchronously on the default loop") {
  auto log = makeLog();
  auto promise = rt::Promise(rt::async([log]() -> rt::Task<int> {
    log->push_back("started");
    co_await rt::microtask();
    log->push_back("resumed");
    co_return 3;
  }));
  EXPECT_EQ(*log, (Strings{"started"}));
  int value = rt::blockOn(promise.wait());
  EXPECT_EQ(value, 3);
  EXPECT_EQ(*log, (Strings{"started", "resumed"}));
}

TEST("destroyed coroutine frames cannot post microtasks into an enclosing loop") {
  rt::FakeClock clock;
  rt::Loop outer(clock);
  rt::LoopScope ambient(outer);
  auto log = makeLog();
  {
    rt::Loop inner(clock);
    rt::LoopScope scope(inner);
    auto never = rt::withResolvers();
    rt::spawn(rt::async([log, waiting = never.promise]() -> rt::Task<void> {
      auto cleanup = rt::scopeExit([log] {
        log->push_back("cleanup");
        rt::queueMicrotask([log] { log->push_back("orphaned microtask"); });
      });
      co_await waiting.wait();
    }));
    inner.run();
  }
  EXPECT_EQ(outer.microtaskCount(), 0u);
  outer.run();
  EXPECT_EQ(*log, (Strings{"cleanup"}));
}

namespace {
  struct Counted {
    std::shared_ptr<int> token;
    int value = 0;
  };
  rt::Task<Counted> makeCounted(std::shared_ptr<int> token) {
    co_await rt::microtask();
    co_return Counted{token, 5};
  }
}

ASYNC_TEST("aggregate values pass through Promise, async, withFinally and generators with exact reference counts") {
  auto token = std::make_shared<int>(1);
  {
    auto promise = rt::Promise(makeCounted(token));
    Counted first = co_await promise.wait();
    Counted second = co_await promise.wait();
    EXPECT_EQ(first.value + second.value, 10);
    EXPECT_EQ(token.use_count(), 4);
  }
  EXPECT_EQ(token.use_count(), 1);
  {
    auto task = rt::async([token]() -> rt::Task<Counted> { co_return Counted{token, 6}; });
    Counted held = co_await std::move(task);
    auto noCleanup = [] {};
    Counted kept = co_await rt::withFinally(makeCounted(token), noCleanup);
    EXPECT_EQ(held.value + kept.value, 11);
    EXPECT_EQ(token.use_count(), 3);
  }
  EXPECT_EQ(token.use_count(), 1);
  {
    rt::AsyncGenerator<Counted> generator([token](rt::Yield<Counted> yield) -> rt::Task<void> {
      Counted chunk{token, 7};
      co_await yield(chunk);
    });
    auto step = co_await generator.next();
    EXPECT(!step.done);
    EXPECT_EQ(step.value->value, 7);
    step = co_await generator.next();
    EXPECT(step.done);
  }
  EXPECT_EQ(token.use_count(), 1);
}

ASYNC_TEST("a generator body can borrow its yield handle across suspension") {
  rt::AsyncGenerator<std::string> generator([](const rt::Yield<std::string>& yield) -> rt::Task<void> {
    co_await rt::microtask();
    auto yielding = yield("value");
    co_await std::move(yielding);
  });
  const auto first = co_await generator.next();
  ASSERT(first.value.has_value());
  EXPECT_EQ(*first.value, "value");
  const auto last = co_await generator.next();
  EXPECT(last.done);
}

ASYNC_TEST("generator steps keep the generator alive until their results settle") {
  auto makeGenerator = [] {
    return rt::AsyncGenerator<int>([](rt::Yield<int> yield) -> rt::Task<void> {
      auto yielding = yield(7);
      co_await std::move(yielding);
    });
  };
  rt::Task<rt::IteratorResult<int>> next;
  rt::Task<rt::IteratorResult<int>> returning;
  {
    auto generator = makeGenerator();
    next = generator.next();
  }
  {
    auto generator = makeGenerator();
    auto discarded = generator.next();
    returning = generator.return_();
  }
  rt::Promise nextResult(std::move(next));
  rt::Promise returnResult(std::move(returning));

  // The timer runs after the microtask checkpoint. A stranded step fails
  // here without parking the test on a Promise that never settles.
  co_await rt::sleep(1);
  ASSERT(nextResult.fulfilled());
  ASSERT(returnResult.fulfilled());
  EXPECT(!nextResult.value().done);
  ASSERT(nextResult.value().value.has_value());
  EXPECT_EQ(*nextResult.value().value, 7);
  EXPECT(returnResult.value().done);
}

ASYNC_TEST("resolving functions: resolve(promise) locks the promise so a later reject is ignored; a throwing executor rejects") {
  auto [promise, resolve, reject] = rt::withResolvers<int>();
  auto source = rt::withResolvers<int>();
  resolve(source.promise);
  reject(rt::makeReason(rt::JsError("ignored")));
  resolve(5);
  EXPECT(promise.pending());
  source.resolve(9);
  int value = co_await promise.wait();
  EXPECT_EQ(value, 9);
  auto failing = rt::newPromise<int>([](auto, auto) { throw rt::RangeError("executor"); });
  EXPECT(failing.rejected());
  EXPECT_EQ(rt::reasonToString(failing.reason()), "RangeError: executor");
  (void)failing.catch_(rt::ignoreRejection);
  auto voidPromise = rt::newPromise([](auto resolveVoid, auto) { resolveVoid(); });
  EXPECT(voidPromise.fulfilled());
}

ASYNC_TEST("Promise callback Tasks retain the borrowed fulfillment or rejection") {
  auto gate = rt::withResolvers();
  const auto failure = rt::makeReason(rt::TypeError("early rejection"));
  auto late = [ready = gate.promise](const std::string& value) -> rt::Task<std::string> {
    co_await ready.wait();
    co_return value;
  };
  const std::string payload(128, 'x');
  auto pending = rt::Promise<std::string>::resolve(payload).then(late);
  std::vector inputs{rt::Promise<std::string>::reject(failure), pending};
  auto combined = rt::all(std::move(inputs));
  EXPECT_REJECTS_REASON(combined.wait(), failure);
  EXPECT(pending.pending());
  gate.resolve();
  const auto value = co_await pending.wait();
  EXPECT_EQ(value, payload);

  auto onRejected = [](const rt::Reason& reason) -> rt::Task<std::string> {
    co_await rt::microtask();
    co_return rt::reasonToString(reason);
  };
  auto rejected = rt::Promise<std::string>::reject(failure).then(late, onRejected);
  auto recovered = rt::Promise<std::string>::reject(failure).catch_(onRejected);
  const auto fromThen = co_await rejected.wait();
  const auto fromCatch = co_await recovered.wait();
  EXPECT_EQ(fromThen, "TypeError: early rejection");
  EXPECT_EQ(fromCatch, fromThen);
}

ASYNC_TEST("then(onFulfilled, onRejected), catch_ returning a value, and rejections passing through then") {
  auto log = makeLog();
  auto onValue = [log](int value) { return "value=" + std::to_string(value); };
  auto onError = [log](const rt::Reason& reason) { return "error=" + rt::reasonToString(reason); };
  std::string fulfilled = co_await rt::Promise<int>::resolve(3).then(onValue, onError).wait();
  std::string rejected = co_await rt::Promise<int>::reject(rt::makeReason(rt::TypeError("t"))).then(onValue, onError).wait();
  EXPECT_EQ(fulfilled, "value=3");
  EXPECT_EQ(rejected, "error=TypeError: t");
  auto recover = [](const rt::Reason&) { return 11; };
  int recovered = co_await rt::Promise<int>::reject(nullptr).catch_(recover).wait();
  EXPECT_EQ(recovered, 11);
  int passed = co_await rt::Promise<int>::resolve(12).catch_(recover).wait();
  EXPECT_EQ(passed, 12);
  auto sourceReason = rt::thrown(json::value("no"));
  auto passthrough = rt::Promise<int>::reject(sourceReason).then(onValue);
  EXPECT(passthrough.pending());
  EXPECT_REJECTS_REASON(passthrough.wait(), sourceReason);
  std::string reason = co_await passthrough.catch_(onError).wait();
  EXPECT_EQ(reason, "error=no");
}

ASYNC_TEST("spawn(task, onRejected) handles the rejection instead of reporting it") {
  auto log = makeLog();
  rt::spawn(failWith("handled"), [log](const rt::Reason& reason) { log->push_back(rt::reasonToString(reason)); });
  co_await rt::sleep(1);
  EXPECT_EQ(*log, (Strings{"TypeError: handled"}));
}

// ---- Identity (`===`, WeakMap / WeakSet keys) and `Promise.resolve` behind a Task ----------------
// bun: `p === copy`, `p.then(f) !== p`; a WeakMap keyed by p finds it through any copy and not
// through a derived promise; `delete` is true once.

ASYNC_TEST("a Promise is === its copies and keys rt::WeakMap and rt::WeakSet by identity") {
  auto promise = rt::Promise<int>::resolve(1);
  const rt::Promise<int> copy = promise;
  auto same = [](int value) { return value; };
  auto derived = promise.then(same);
  EXPECT(copy == promise);
  EXPECT(copy.identity() == promise.identity());
  EXPECT(promise.identity() == promise.state().get());
  EXPECT(!(derived == promise));
  EXPECT(derived.identity() != promise.identity());
  rt::WeakMap<rt::Promise<int>, std::string> tasks;
  tasks.set(promise, "a");
  EXPECT_EQ(tasks.get(copy), std::optional<std::string>("a"));
  EXPECT(!tasks.has(derived));
  EXPECT(tasks.delete_(promise));
  EXPECT(!tasks.delete_(copy));
  rt::WeakSet<rt::Promise<int>> seen;
  seen.add(derived);
  EXPECT(seen.has(derived));
  EXPECT(!seen.has(promise));
  const int value = co_await derived.wait();
  EXPECT_EQ(value, 1);
}

TEST("a WeakMap<Promise, T> entry dies with the last reference to its promise") {
  rt::WeakMap<rt::Promise<void>, int> tails;
  std::weak_ptr<const void> gone;
  {
    const rt::Promise<void> pending;
    tails.set(pending, 1);
    gone = pending.weakIdentity();
    EXPECT(tails.has(pending));
    EXPECT(!gone.expired());
  }
  EXPECT(gone.expired());
  const rt::Promise<void> later;
  EXPECT(!tails.has(later));
  EXPECT(!tails.delete_(later));
}

ASYNC_TEST("fulfilledTask is Promise.resolve(value) behind a Task: awaiting it takes no turn") {
  // bun, the same body with `await Promise.resolve(...)`: tick1, value v, void, awaited w. A
  // Task completes inline (docs/rt/async.md section 5); Promise<T>::resolve(v).wait() takes the
  // turn JS takes.
  auto log = makeLog();
  ticks(log, 1);
  auto valueTask = rt::fulfilledTask(std::string("v"));
  const std::string value = co_await std::move(valueTask);
  log->push_back("value " + value);
  co_await rt::fulfilledTask();
  log->push_back("void");
  const auto resolved = rt::Promise<std::string>::resolve(std::string("w"));
  const std::string awaited = co_await resolved.wait();
  log->push_back("awaited " + awaited);
  EXPECT_EQ(*log, (Strings{"value v", "void", "tick1", "awaited w"}));
  auto exactLog = makeLog();
  ticks(exactLog, 1);
  const auto first = rt::Promise<std::string>::resolve(std::string("v"));
  const std::string exact = co_await first.wait();
  exactLog->push_back("value " + exact);
  EXPECT_EQ(*exactLog, (Strings{"tick1", "value v"}));
}
