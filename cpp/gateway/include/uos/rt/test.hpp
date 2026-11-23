#pragma once
// Minimal test harness for ported bun tests. Include from exactly one
// translation unit per test binary; it defines main().
//   TEST("name") { EXPECT(...); EXPECT_EQ(actual, expected); EXPECT_THROWS(expr, Type); }
//   EXPECT_THROWS_MATCHING(expr, "pattern")   node's assert.throws(() => expr, /pattern/)
// Run `<binary> <substring>` to select tests by name. Asynchronous tests use
// ASYNC_TEST from uos/rt/test_async.hpp.
#include "uos/rt/error.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/regex.hpp"
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace uos::rt::test {
  struct Case {
    std::string name;
    std::function<void()> body;
  };
  inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
  }
  struct Registrar {
    Registrar(std::string name, std::function<void()> body) { registry().push_back({std::move(name), std::move(body)}); }
  };
  struct Failure {
    std::string text;
  };
  inline int& failures() {
    static int count = 0;
    return count;
  }

  inline std::string show(const std::string& value) { return json::serialize(json::value(value)); }
  inline std::string show(std::string_view value) { return show(std::string(value)); }
  inline std::string show(json::string_view value) { return show(std::string(value.data(), value.size())); }
  inline std::string show(const char* value) { return show(std::string(value)); }
  inline std::string show(bool value) { return value ? "true" : "false"; }
  inline std::string show(std::nullopt_t) { return "undefined"; }
  inline std::string show(std::nullptr_t) { return "null"; }
  inline std::string show(const json::value& value) { return json::serialize(value); }
  inline std::string show(const json::object& value) { return json::serialize(value); }
  inline std::string show(const json::array& value) { return json::serialize(value); }
  inline std::string show(const json::string& value) { return json::serialize(value); }
  template <class T>
    requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
  std::string show(T value) {
    std::ostringstream out;
    out.precision(17);
    out << +value;
    return out.str();
  }
  template <class T>
    requires std::is_enum_v<T>
  std::string show(T value) {
    return "enum(" + std::to_string(static_cast<long long>(value)) + ")";
  }
  template <class T> std::string show(const std::optional<T>& value);
  template <class T> std::string show(const std::vector<T>& value);
  template <class T>
  std::string show(const T&)
    requires(!std::is_arithmetic_v<T> && !std::is_enum_v<T> && !std::is_convertible_v<const T&, std::string_view> && !std::is_convertible_v<const T&, json::value>)
  {
    return "<value>";
  }
  template <class T> std::string show(const std::optional<T>& value) { return value ? show(*value) : "undefined"; }
  template <class T> std::string show(const std::vector<T>& value) {
    std::string out = "[";
    for (std::size_t index = 0; index < value.size(); ++index) out += (index ? ", " : "") + show(value[index]);
    return out + "]";
  }

  inline void report(const char* file, int line, const std::string& text, bool fatal) {
    ++failures();
    std::fprintf(stderr, "    %s:%d: %s\n", file, line, text.c_str());
    if (fatal) throw Failure{text};
  }
  template <class A, class B> void expectEqual(const A& actual, const B& expected, const char* actualText, const char* expectedText, const char* file, int line, bool fatal) {
    if (!(actual == expected)) report(file, line, std::string("expected ") + actualText + " == " + expectedText + "\n      actual:   " + show(actual) + "\n      expected: " + show(expected), fatal);
  }

  inline int run(int argc, char** argv) {
    std::string_view filter = argc > 1 ? argv[1] : "";
    int ran = 0, failed = 0;
    for (auto& entry : registry()) {
      if (!filter.empty() && entry.name.find(filter) == std::string::npos) continue;
      ++ran;
      int before = failures();
      try {
        entry.body();
      } catch (const Failure&) {
      } catch (const std::exception& error) {
        ++failures();
        std::fprintf(stderr, "    uncaught exception: %s\n", error.what());
      } catch (...) {
        ++failures();
        std::fprintf(stderr, "    uncaught non-standard exception\n");
      }
      bool ok = failures() == before;
      if (!ok) ++failed;
      std::printf("%s %s\n", ok ? "ok  " : "FAIL", entry.name.c_str());
    }
    std::printf("%d run, %d passed, %d failed\n", ran, ran - failed, failed);
    return failed == 0 && ran > 0 ? 0 : 1;
  }
}

#define UOS_TEST_JOIN2(a, b) a##b
#define UOS_TEST_JOIN(a, b) UOS_TEST_JOIN2(a, b)
#define UOS_TEST_IMPL(name, id) \
  static void UOS_TEST_JOIN(uosTestBody, id)(); \
  static const ::uos::rt::test::Registrar UOS_TEST_JOIN(uosTestRegistrar, id)(name, &UOS_TEST_JOIN(uosTestBody, id)); \
  static void UOS_TEST_JOIN(uosTestBody, id)()
#define TEST(name) UOS_TEST_IMPL(name, __COUNTER__)

// EXPECT* record a failure and continue; ASSERT* abort the current test.
#define EXPECT(condition) \
  do { if (!(condition)) ::uos::rt::test::report(__FILE__, __LINE__, "expected " #condition, false); } while (0)
#define ASSERT(condition) \
  do { if (!(condition)) ::uos::rt::test::report(__FILE__, __LINE__, "expected " #condition, true); } while (0)
#define EXPECT_EQ(actual, expected) ::uos::rt::test::expectEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__, false)
#define ASSERT_EQ(actual, expected) ::uos::rt::test::expectEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__, true)
#define EXPECT_THROWS(expression, Type) \
  do { \
    bool uosThrew = false; \
    try { (void)(expression); } catch (const Type&) { uosThrew = true; } catch (...) { ::uos::rt::test::report(__FILE__, __LINE__, "expected " #expression " to throw " #Type ", threw another type", false); uosThrew = true; } \
    if (!uosThrew) ::uos::rt::test::report(__FILE__, __LINE__, "expected " #expression " to throw " #Type, false); \
  } while (0)
// Asserts the thrown rt::JsError (or subclass) carries exactly this message.
#define EXPECT_THROWS_MESSAGE(expression, Type, text) \
  do { \
    bool uosThrew = false; \
    try { (void)(expression); } catch (const Type& uosError) { uosThrew = true; EXPECT_EQ(std::string(uosError.what()), std::string(text)); } catch (...) { ::uos::rt::test::report(__FILE__, __LINE__, "expected " #expression " to throw " #Type ", threw another type", false); uosThrew = true; } \
    if (!uosThrew) ::uos::rt::test::report(__FILE__, __LINE__, "expected " #expression " to throw " #Type, false); \
  } while (0)

namespace uos::rt {
  // `String(reason)`, declared by uos/rt/async.hpp and defined in src/rt/async.cpp.
  std::string reasonToString(const std::exception_ptr& reason);
}

namespace uos::rt::test {
  // The regular expression of EXPECT_THROWS_MATCHING: a pattern source without flags
  // ("not found") or an rt::Regex (flags; a g/y regex starts at its lastIndex, as exec does).
  class ThrowsPattern {
  public:
    ThrowsPattern(const char* source) : regex_(source) {}
    ThrowsPattern(std::string_view source) : regex_(source) {}
    ThrowsPattern(const std::string& source) : regex_(source) {}
    ThrowsPattern(const Regex& regex) : regex_(regex) {}
    const Regex& regex() const noexcept { return regex_; }

  private:
    Regex regex_;
  };

  // String(thrown) of the exception being handled, as node:assert computes it: an Error is
  // "Name: message" (the message alone when the name is "", the name alone when the message
  // is), any other value String(value).
  inline std::string thrownText() {
    try {
      throw;
    } catch (const JsError& error) {
      return error.name.empty() ? error.message : error.toString();
    } catch (...) {
      return reasonToString(std::current_exception());
    }
  }
}

// `assert.throws(() => expression, /pattern/)`: the expression must throw, and the pattern
// must match String(thrown) (node:assert runs RegExp.prototype.exec on it), so it sees the
// name ("TypeError: ...") and any value, not only Errors. Declared at global scope for the
// macro's unqualified call; every parameter is a user-defined or integral conversion, so a
// test file's own expectThrowsMatching (the earlier local helpers) stays the better match.
template <class = void>
void expectThrowsMatching(const std::function<void()>& body, const ::uos::rt::test::ThrowsPattern& pattern, std::string_view text, std::string_view file,
                          long line) {
  const std::string where(file);
  try {
    body();
  } catch (const ::uos::rt::test::Failure&) {
    throw;
  } catch (...) {
    const std::string actual = ::uos::rt::test::thrownText();
    if (!pattern.regex().test(actual)) {
      ::uos::rt::test::report(where.c_str(), static_cast<int>(line),
                              std::string(text) + " threw " + ::uos::rt::test::show(actual) + ", which does not match " + pattern.regex().toString(), false);
    }
    return;
  }
  ::uos::rt::test::report(where.c_str(), static_cast<int>(line), "expected " + std::string(text) + " to throw", false);
}
// Token for token the definition the test files that wrote it locally use, so theirs stays a
// valid redefinition.
#ifndef EXPECT_THROWS_MATCHING
#define EXPECT_THROWS_MATCHING(expression, pattern) expectThrowsMatching([&] { (void)(expression); }, pattern, #expression, __FILE__, __LINE__)
#endif

int main(int argc, char** argv) { return ::uos::rt::test::run(argc, argv); }
