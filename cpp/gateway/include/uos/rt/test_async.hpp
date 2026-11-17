#pragma once
// Coroutine tests on top of uos/rt/test.hpp (include this instead of it):
//   ASYNC_TEST("name") { co_await ...; EXPECT_EQ(...); }
// Each body runs to completion with rt::blockOn on a fresh Loop driven by a
// fresh FakeClock (rt::test::clock()): Date.now() starts at the real current
// time, performance.now() at 0, and an idle loop jumps straight to the next
// timer. An unhandled rejection or a throwing timer callback fails the test
// (UncaughtPolicy::raise). The test ends when its body completes; timers still
// pending then are dropped with the loop. rt::globalDateNow(),
// rt::globalSetTimeout() and rt::globalClearTimeout() are restored afterwards.
// Inside ASYNC_TEST bodies:
//   EXPECT_REJECTS(task, Type);                  task rejects with a Type
//   EXPECT_REJECTS_MESSAGE(task, Type, message); ... whose message is exactly `message`
//   EXPECT_REJECTS_REASON(task, reason);         ... with exactly this Reason object
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/test.hpp"
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace uos::rt::test {
  inline FakeClock*& currentClock() {
    static FakeClock* clock = nullptr;
    return clock;
  }
  // The fake clock of the running ASYNC_TEST.
  inline FakeClock& clock() {
    if (!currentClock()) throw std::logic_error("rt::test::clock() outside an ASYNC_TEST");
    return *currentClock();
  }

  inline void runAsync(Task<void> (*body)()) {
    struct Restore {
      FakeClock* savedClock = currentClock();
      std::function<double()> savedDateNow = globalDateNow();
      SetTimeoutFunction savedSetTimeout = globalSetTimeout();
      ClearTimeoutFunction savedClearTimeout = globalClearTimeout();
      ~Restore() {
        currentClock() = savedClock;
        globalDateNow() = savedDateNow;
        globalSetTimeout() = savedSetTimeout;
        globalClearTimeout() = savedClearTimeout;
      }
    } restore;
    FakeClock fake;
    Loop loop(fake);
    currentClock() = &fake;
    blockOn(loop, body());
  }

  template <class E, class T>
  Task<void> expectRejects(Task<T> task, const char* text, const char* type, const char* message, const char* file, int line) {
    bool rejected = false;
    try {
      co_await std::move(task);
    } catch (const E& error) {
      rejected = true;
      if (message) {
        std::string actual;
        if constexpr (std::is_base_of_v<JsError, E>) actual = error.message;
        else actual = error.what();
        if (actual != message) report(file, line, std::string("expected ") + text + " to reject with message " + show(message) + "\n      actual:   " + show(actual), false);
      }
    } catch (...) {
      rejected = true;
      report(file, line, std::string("expected ") + text + " to reject with " + type + ", rejected with " + show(reasonToString(std::current_exception())), false);
    }
    if (!rejected) report(file, line, std::string("expected ") + text + " to reject with " + type, false);
  }

  template <class T> Task<void> expectRejectsReason(Task<T> task, Reason expected, const char* text, const char* file, int line) {
    bool rejected = false;
    try {
      co_await std::move(task);
    } catch (...) {
      rejected = true;
      if (std::current_exception() != expected)
        report(file, line, std::string("expected ") + text + " to reject with the given reason, rejected with " + show(reasonToString(std::current_exception())), false);
    }
    if (!rejected) report(file, line, std::string("expected ") + text + " to reject", false);
  }
}

#define UOS_ASYNC_TEST_IMPL(name, id) \
  static ::uos::rt::Task<void> UOS_TEST_JOIN(uosAsyncTestBody, id)(); \
  TEST(name) { ::uos::rt::test::runAsync(&UOS_TEST_JOIN(uosAsyncTestBody, id)); } \
  static ::uos::rt::Task<void> UOS_TEST_JOIN(uosAsyncTestBody, id)()
#define ASYNC_TEST(name) UOS_ASYNC_TEST_IMPL(name, __COUNTER__)

#define EXPECT_REJECTS(task, Type) co_await ::uos::rt::test::expectRejects<Type>((task), #task, #Type, nullptr, __FILE__, __LINE__)
#define EXPECT_REJECTS_MESSAGE(task, Type, text) co_await ::uos::rt::test::expectRejects<Type>((task), #task, #Type, static_cast<const char*>(text), __FILE__, __LINE__)
#define EXPECT_REJECTS_REASON(task, reason) co_await ::uos::rt::test::expectRejectsReason((task), (reason), #task, __FILE__, __LINE__)
