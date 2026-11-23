// AbortController / AbortSignal / DOMException and the abort bridge. Expected
// values marked "bun" were recorded by running the same scenario under Bun 1.4.
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/test_async.hpp"
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace uos;

namespace {
  using Strings = std::vector<std::string>;
  using Log = std::shared_ptr<Strings>;
  Log makeLog() { return std::make_shared<Strings>(); }

  const rt::DOMException* domException(const rt::Reason& reason) {
    try {
      rt::rethrow(reason);
    } catch (const rt::DOMException& error) {
      static thread_local rt::DOMException copy;
      copy = error;
      return &copy;
    } catch (...) {
    }
    return nullptr;
  }
}

TEST("bun abort1: abort() reasons and DOMException defaults") {
  rt::AbortController c1;
  EXPECT(!c1.signal().aborted());
  EXPECT(!c1.signal().reason());
  c1.abort();
  EXPECT(c1.signal().aborted());
  const auto* reason = domException(c1.signal().reason());
  ASSERT(reason != nullptr);
  EXPECT_EQ(reason->name, "AbortError");
  EXPECT_EQ(reason->message, "The operation was aborted.");
  EXPECT_EQ(reason->code(), 20);
  EXPECT(rt::isError(c1.signal().reason()));
  EXPECT_EQ(rt::reasonToString(c1.signal().reason()), "AbortError: The operation was aborted.");
  bool same = false;
  try {
    c1.signal().throwIfAborted();
  } catch (...) {
    same = std::current_exception() == c1.signal().reason();
  }
  EXPECT(same);

  rt::AbortController c3;
  c3.abort(rt::thrown(json::value(nullptr)));
  EXPECT_EQ(rt::thrownValue(c3.signal().reason()), std::optional<json::value>(json::value(nullptr)));
  rt::AbortController c4;
  c4.abort(rt::thrown(json::value("str")));
  EXPECT_EQ(rt::reasonToString(c4.signal().reason()), "str");
  auto first = c4.signal().reason();
  c4.abort(rt::thrown(json::value("again")));
  EXPECT(c4.signal().reason() == first);

  auto aborted = rt::AbortSignal::abort();
  EXPECT(aborted.aborted());
  EXPECT_EQ(rt::reasonToString(aborted.reason()), "AbortError: The operation was aborted.");
  EXPECT_EQ(rt::reasonToString(rt::AbortSignal::abort(rt::thrown(json::value("x"))).reason()), "x");
  EXPECT_THROWS_MESSAGE(rt::AbortSignal::abort().throwIfAborted(), rt::DOMException, "The operation was aborted.");
  rt::AbortController fresh;
  fresh.signal().throwIfAborted();
  EXPECT(!fresh.signal().onabort().has_value());

  rt::DOMException empty;
  EXPECT_EQ(empty.name, "Error");
  EXPECT_EQ(empty.message, "");
  EXPECT_EQ(empty.code(), 0);
  EXPECT_EQ(empty.toString(), "Error");
  EXPECT_EQ(rt::DOMException("m").toString(), "Error: m");
  EXPECT_EQ(rt::DOMException("t", "TimeoutError").code(), 23);
  EXPECT_EQ(rt::DOMException("t", "WeirdName").code(), 0);
  EXPECT_EQ(rt::DOMException("t", "WeirdName").toString(), "WeirdName: t");
  EXPECT_EQ(rt::DOMException("t", "NotFoundError").code(), 8);
  EXPECT_EQ(rt::UncaughtError(rt::abortErrorReason(), true).report(), "AbortError: The operation was aborted.");
  EXPECT_EQ(rt::UncaughtError(rt::timeoutErrorReason(), true).report(), "TimeoutError: The operation timed out.");
}

TEST("bun misc1: DOMException legacy codes") {
  const std::vector<std::pair<std::string, int>> codes = {
    {"IndexSizeError", 1},          {"DOMStringSizeError", 0},      {"HierarchyRequestError", 3},   {"WrongDocumentError", 4},       {"InvalidCharacterError", 5},
    {"NoDataAllowedError", 0},      {"NoModificationAllowedError", 7}, {"NotFoundError", 8},        {"NotSupportedError", 9},        {"InUseAttributeError", 10},
    {"InvalidStateError", 11},      {"SyntaxError", 12},            {"InvalidModificationError", 13}, {"NamespaceError", 14},        {"InvalidAccessError", 15},
    {"ValidationError", 0},         {"TypeMismatchError", 17},      {"SecurityError", 18},          {"NetworkError", 19},            {"AbortError", 20},
    {"URLMismatchError", 21},       {"QuotaExceededError", 22},     {"TimeoutError", 23},           {"InvalidNodeTypeError", 24},    {"DataCloneError", 25},
    {"EncodingError", 0},           {"NotReadableError", 0},        {"UnknownError", 0},            {"ConstraintError", 0},          {"DataError", 0},
    {"TransactionInactiveError", 0}, {"ReadOnlyError", 0},          {"VersionError", 0},            {"OperationError", 0},           {"NotAllowedError", 0},
  };
  for (const auto& [name, code] : codes) {
    if (rt::DOMException("m", name).code() != code) std::fprintf(stderr, "    name %s\n", name.c_str());
    EXPECT_EQ(rt::DOMException("m", name).code(), code);
  }
}

TEST("abort dispatch retains the signal when a listener destroys its controller") {
  auto controller = std::make_unique<rt::AbortController>();
  Strings calls;
  controller->signal().addEventListener("abort", [&] {
    calls.push_back("destroy");
    controller.reset();
  });
  controller->signal().addEventListener("abort", [&] { calls.push_back("once"); }, {.once = true});

  controller->abort();
  EXPECT_EQ(calls, (Strings{"destroy", "once"}));
}

TEST("bun abort1: listeners run in registration order; once, dedup, onabort position, removal and re-abort during dispatch") {
  auto log = makeLog();
  rt::AbortController c5;
  auto signal = c5.signal();
  rt::EventListener a = [log] { log->push_back("a"); };
  rt::EventListener b = [log] { log->push_back("b"); };
  rt::EventListener d = [log] { log->push_back("d-should-not-run"); };
  signal.addEventListener("abort", a);
  signal.addEventListener("abort", a);
  signal.setOnabort(rt::EventListener([log] { log->push_back("onabort1"); }));
  signal.addEventListener("abort", b, {.once = true});
  signal.setOnabort(rt::EventListener([log] { log->push_back("onabort2"); }));
  signal.addEventListener("abort", rt::EventListener([log, signal, d, c5] {
    log->push_back("c-aborted=" + std::string(signal.aborted() ? "true" : "false") + " reasonName=" + rt::errorName(signal.reason()).value_or("?"));
    signal.removeEventListener("abort", d);
    c5.abort(rt::thrown(json::value("again")));
  }));
  signal.addEventListener("abort", d);
  signal.addEventListener("abort", rt::EventListener([log] { log->push_back("e"); }));
  signal.addEventListener("other", rt::EventListener([log] { log->push_back("other-type"); }));
  c5.abort();
  EXPECT_EQ(*log, (Strings{"a", "onabort2", "b", "c-aborted=true reasonName=AbortError", "e"}));
  EXPECT_EQ(rt::errorName(signal.reason()), std::optional<std::string>("AbortError"));
  EXPECT(signal.onabort().has_value());
  log->clear();
  signal.addEventListener("abort", rt::EventListener([log] { log->push_back("late"); }));
  EXPECT(log->empty());

  rt::AbortController removed;
  removed.signal().setOnabort(rt::EventListener([log] { log->push_back("handler"); }));
  removed.signal().setOnabort(std::nullopt);
  EXPECT(!removed.signal().onabort().has_value());
  rt::EventListener kept = [log] { log->push_back("kept"); };
  removed.signal().addEventListener("abort", kept);
  removed.signal().removeEventListener("abort", rt::EventListener([log] { log->push_back("kept"); }));
  removed.abort();
  EXPECT_EQ(*log, (Strings{"kept"}));
}

TEST("onabort assignment preserves handler identity and position before and after abort") {
  auto log = makeLog();
  rt::AbortController controller;
  const auto signal = controller.signal();
  rt::EventListener first = [log] { log->push_back("first"); };
  rt::EventListener second = [log] { log->push_back("second"); };
  rt::EventListener plain = [log] { log->push_back("plain"); };
  EXPECT(!signal.onabort().has_value());
  signal.setOnabort(first);
  EXPECT(signal.onabort() == std::optional<rt::EventListener>(first));
  signal.addEventListener("abort", plain);
  signal.setOnabort(first);
  signal.setOnabort(second);
  EXPECT(signal.onabort() == std::optional<rt::EventListener>(second));
  controller.abort();
  EXPECT_EQ(*log, (Strings{"second", "plain"}));
  EXPECT(signal.onabort() == std::optional<rt::EventListener>(second));
  signal.setOnabort(first);
  controller.abort();
  EXPECT_EQ(*log, (Strings{"second", "plain"}));
  EXPECT(signal.onabort() == std::optional<rt::EventListener>(first));
  signal.setOnabort(std::nullopt);
  EXPECT(!signal.onabort().has_value());
}

TEST("onabort normalizes an empty listener and reads a replacement during dispatch") {
  auto log = makeLog();
  rt::AbortController removed;
  removed.signal().setOnabort(rt::EventListener([log] { log->push_back("removed"); }));
  removed.signal().setOnabort(rt::EventListener{});
  EXPECT(!removed.signal().onabort().has_value());
  removed.signal().addEventListener("abort", rt::EventListener([log] { log->push_back("plain"); }));
  removed.abort();
  EXPECT_EQ(*log, (Strings{"plain"}));

  rt::AbortController during;
  const auto signal = during.signal();
  rt::EventListener replacement = [log] { log->push_back("second"); };
  signal.addEventListener("abort", rt::EventListener([log, signal, replacement] {
    log->push_back("change");
    signal.setOnabort(replacement);
  }));
  signal.setOnabort(rt::EventListener([log] { log->push_back("first"); }));
  during.abort();
  EXPECT_EQ(*log, (Strings{"plain", "change", "second"}));
  EXPECT(signal.onabort() == std::optional<rt::EventListener>(replacement));
}

TEST("onabort owns its callback through abort and releases replaced or cleared callbacks") {
  rt::AbortController controller;
  const auto signal = controller.signal();
  auto firstPayload = std::make_shared<int>(0);
  const std::weak_ptr<int> firstWeak = firstPayload;
  rt::EventListener first = [firstPayload] { ++*firstPayload; };
  signal.setOnabort(first);
  first = rt::EventListener{};
  firstPayload.reset();
  EXPECT(!firstWeak.expired());
  controller.abort();
  EXPECT(!firstWeak.expired());
  {
    const auto payload = firstWeak.lock();
    EXPECT(payload != nullptr);
    if (payload) EXPECT_EQ(*payload, 1);
  }

  auto secondPayload = std::make_shared<int>(0);
  const std::weak_ptr<int> secondWeak = secondPayload;
  rt::EventListener second = [secondPayload] { ++*secondPayload; };
  signal.setOnabort(second);
  second = rt::EventListener{};
  secondPayload.reset();
  EXPECT(firstWeak.expired());
  EXPECT(!secondWeak.expired());
  controller.abort();
  {
    const auto payload = secondWeak.lock();
    EXPECT(payload != nullptr);
    if (payload) EXPECT_EQ(*payload, 0);
  }
  signal.setOnabort(std::nullopt);
  EXPECT(secondWeak.expired());
}

TEST("bun abort1: AbortSignal.any aborts its dependents before the source's listeners run and dispatches to them afterwards") {
  auto log = makeLog();
  rt::AbortController s1;
  rt::AbortController s2;
  auto any = rt::AbortSignal::any({s1.signal(), s2.signal()});
  s1.signal().addEventListener("abort", rt::EventListener([log, any] { log->push_back("s1-listener anyAborted=" + std::string(any.aborted() ? "true" : "false")); }));
  any.addEventListener("abort", rt::EventListener([log] { log->push_back("any-listener"); }));
  s1.signal().addEventListener("abort", rt::EventListener([log, any] { log->push_back("s1-listener2 anyAborted=" + std::string(any.aborted() ? "true" : "false")); }));
  auto reason = rt::makeReason(rt::JsError("R"));
  s1.abort(reason);
  EXPECT_EQ(*log, (Strings{"s1-listener anyAborted=true", "s1-listener2 anyAborted=true", "any-listener"}));
  EXPECT(any.reason() == reason);
  EXPECT(!s2.signal().aborted());

  auto pre = rt::AbortSignal::abort(rt::thrown(json::value("pre")));
  auto anyPre = rt::AbortSignal::any({rt::AbortController().signal(), pre});
  EXPECT(anyPre.aborted());
  EXPECT_EQ(rt::reasonToString(anyPre.reason()), "pre");
  EXPECT(!rt::AbortSignal::any({}).aborted());

  log->clear();
  rt::AbortController n1;
  auto inner = rt::AbortSignal::any({n1.signal()});
  auto outer = rt::AbortSignal::any({inner});
  outer.addEventListener("abort", rt::EventListener([log] { log->push_back("outer"); }));
  inner.addEventListener("abort", rt::EventListener([log] { log->push_back("inner"); }));
  n1.signal().addEventListener("abort", rt::EventListener([log] { log->push_back("n1"); }));
  n1.abort();
  EXPECT_EQ(*log, (Strings{"n1", "inner", "outer"}));
}

TEST("AbortSignal.any keeps a dependent with listeners alive while a source can abort it (JS keeps it reachable)") {
  auto log = makeLog();
  rt::AbortController source;
  rt::AbortSignal::any({source.signal()}).addEventListener("abort", rt::EventListener([log] { log->push_back("dependent-listener"); }));
  source.abort();
  EXPECT_EQ(*log, (Strings{"dependent-listener"}));
}

TEST("bun abort1: an exception from a listener is uncaught; the remaining listeners still run and abort() returns") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  auto log = makeLog();
  bool raised = false;
  try {
    rt::blockOn(loop, rt::async([log]() -> rt::Task<void> {
      rt::AbortController c6;
      c6.signal().addEventListener("abort", rt::EventListener([log] {
        log->push_back("thrower");
        throw rt::JsError("listener-threw");
      }));
      c6.signal().addEventListener("abort", rt::EventListener([log] { log->push_back("after-thrower"); }));
      c6.abort();
      log->push_back("abort-returned");
      co_await rt::sleep(1);
    }));
  } catch (const rt::UncaughtError& error) {
    raised = true;
    EXPECT(!error.rejection);
    EXPECT_EQ(error.report(), "error: listener-threw");
  }
  EXPECT(raised);
  EXPECT_EQ(*log, (Strings{"thrower", "after-thrower", "abort-returned"}));
}

ASYNC_TEST("bun t5/misc1: AbortSignal.timeout aborts with a TimeoutError after ms; invalid ms is a TypeError") {
  auto log = makeLog();
  auto signal = rt::AbortSignal::timeout(5);
  signal.addEventListener("abort", rt::EventListener([log, signal] { log->push_back("fired@" + std::to_string(static_cast<int>(rt::performanceNow())) + " " + rt::reasonToString(signal.reason())); }));
  auto zero = rt::AbortSignal::timeout(0);
  auto fraction = rt::AbortSignal::timeout(1.5);
  co_await rt::sleep(10);
  EXPECT_EQ(*log, (Strings{"fired@5 TimeoutError: The operation timed out."}));
  EXPECT(zero.aborted());
  EXPECT(fraction.aborted());
  const auto* reason = domException(signal.reason());
  ASSERT(reason != nullptr);
  EXPECT_EQ(reason->code(), 23);
  EXPECT_THROWS_MESSAGE(rt::AbortSignal::timeout(-1), rt::TypeError, "Value -1 is outside the range [0, 9007199254740991]");
  EXPECT_THROWS_MESSAGE(rt::AbortSignal::timeout(std::nan("")), rt::TypeError, "Value NaN is outside the range [0, 9007199254740991]");
  EXPECT_THROWS_MESSAGE(rt::AbortSignal::timeout(std::numeric_limits<double>::infinity()), rt::TypeError, "Value Infinity is outside the range [0, 9007199254740991]");
}

TEST("bun timers2 timeout: AbortSignal.timeout uses an unreferenced timer that never keeps Loop::run alive") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  rt::LoopScope scope(loop);
  auto log = makeLog();
  rt::AbortSignal::timeout(50).addEventListener("abort", rt::EventListener([log] { log->push_back("abort-timeout-fired"); }));
  EXPECT_EQ(loop.timerCount(), 1u);
  loop.run();
  EXPECT(log->empty());
  EXPECT_EQ(clock.monotonicNow(), 0.0);
}

namespace {
  // A task blocked in an Asio IO wait, recording how the wait ended.
  rt::Task<int> ioWait(Log log, std::chrono::milliseconds duration) {
    boost::asio::steady_timer timer(rt::executor());
    timer.expires_after(duration);
    try {
      co_await timer.async_wait(boost::asio::use_awaitable);
    } catch (const boost::system::system_error& error) {
      log->push_back(error.code() == boost::asio::error::operation_aborted ? "io-cancelled" : "io-failed");
      throw;
    }
    log->push_back("io-finished");
    co_return 7;
  }
}

ASYNC_TEST("abortable: rejects with the signal's reason and cancels the task's pending Asio operation") {
  auto log = makeLog();
  rt::AbortController controller;
  rt::setTimeout([controller] { controller.abort(rt::makeReason(rt::DOMException("attempt abandoned", "AbortError"))); }, 5);
  bool rejected = false;
  try {
    co_await rt::abortable(controller.signal(), ioWait(log, std::chrono::seconds(30)));
  } catch (const rt::DOMException& error) {
    rejected = error.message == "attempt abandoned";
  }
  EXPECT(rejected);
  co_await rt::sleep(1);
  EXPECT_EQ(*log, (Strings{"io-cancelled"}));

  log->clear();
  rt::AbortController unused;
  int value = co_await rt::abortable(unused.signal(), ioWait(log, std::chrono::milliseconds(1)));
  EXPECT_EQ(value, 7);
  unused.abort();
  EXPECT_EQ(*log, (Strings{"io-finished"}));

  log->clear();
  auto started = std::make_shared<bool>(false);
  auto never = [started]() -> rt::Task<void> {
    *started = true;
    co_return;
  };
  auto aborted = rt::AbortSignal::abort(rt::thrown(json::value("early")));
  EXPECT_REJECTS_REASON(rt::abortable(aborted, never()), aborted.reason());
  EXPECT(!*started);
}

ASYNC_TEST("sleep(ms, signal) rejects with the reason as soon as the signal aborts and clears its timer") {
  rt::AbortController controller;
  rt::setTimeout([controller] { controller.abort(); }, 5);
  auto sleeping = rt::sleep(1000, controller.signal());
  EXPECT_EQ(rt::loop().timerCount(), 2u);
  EXPECT_REJECTS_MESSAGE(std::move(sleeping), rt::DOMException, "The operation was aborted.");
  EXPECT_EQ(rt::performanceNow(), 5.0);
  EXPECT_EQ(rt::loop().timerCount(), 0u);
  EXPECT_REJECTS_REASON(rt::sleep(10, controller.signal()), controller.signal().reason());
  rt::AbortController quiet;
  co_await rt::sleep(10, quiet.signal());
  EXPECT_EQ(rt::performanceNow(), 15.0);
}
