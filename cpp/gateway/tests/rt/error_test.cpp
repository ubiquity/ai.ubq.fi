// uos::rt JsError (uos/rt/error.hpp): `error.code`. The JS facts were recorded with bun 1.4:
// `new Error("m").code` is undefined, `Object.assign(new Error("m"), { code: "ERR_X" })` keeps
// String(error) "Error: m", a class field `code = "E_CLASS"` reads back on the instance, and a
// TypeError given a code still prints "TypeError: bad". `error.cause` (bun 1.4): `new Error("m", {
// cause: "x" })` keeps "x" (`"cause" in error`, String(error) "Error: m"); `new Error("m")` has no
// `cause` property; `{ cause: undefined }` makes one holding undefined; `new TypeError("t", { cause:
// new RangeError("r") })` keeps that RangeError; `new Error("outer", { cause: new Error("inner", {
// cause: 42 }) }).cause.cause` is 42.
#include "uos/rt/error.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/test.hpp"

#include <cerrno>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace rt = uos::rt;
namespace json = uos::json;

namespace {
  // class CodedError extends Error { code = "E_CLASS"; constructor(m) { super(m); this.name = "CodedError"; } }
  struct CodedError : rt::JsError {
    explicit CodedError(std::string message_) : rt::JsError(std::move(message_), "CodedError") { code = "E_CLASS"; }
  };
}

TEST("JsError::code is error.code: undefined unless set, and not part of String(error)") {
  const rt::JsError plain("m");
  EXPECT_EQ(plain.code, std::nullopt);
  rt::JsError coded("m");
  coded.code = "ERR_X";
  EXPECT_EQ(coded.code, std::optional<std::string>("ERR_X"));
  EXPECT_EQ(coded.toString(), std::string("Error: m"));
  const CodedError custom("boom");
  EXPECT_EQ(custom.code, std::optional<std::string>("E_CLASS"));
  EXPECT_EQ(custom.toString(), std::string("CodedError: boom"));
  rt::TypeError typed("bad");
  typed.code = "ERR_INVALID_STATE";
  EXPECT_EQ(typed.toString(), std::string("TypeError: bad"));
  EXPECT_EQ(std::string(typed.what()), std::string("bad"));
}

TEST("JsError::code survives a throw, a catch by the base class, an exception_ptr and a cause") {
  try {
    rt::RangeError error("out");
    error.code = "ERR_OUT_OF_RANGE";
    throw error;
  } catch (const rt::JsError& caught) {
    EXPECT_EQ(caught.name, std::string("RangeError"));
    EXPECT_EQ(caught.code, std::optional<std::string>("ERR_OUT_OF_RANGE"));
  }
  const std::exception_ptr reason = std::make_exception_ptr(CodedError("thrown"));
  try {
    std::rethrow_exception(reason);
  } catch (const rt::JsError& caught) {
    EXPECT_EQ(caught.code, std::optional<std::string>("E_CLASS"));
  }
  auto inner = std::make_shared<rt::JsError>("inner");
  inner->code = "ERR_INNER";
  const rt::JsError outer("outer", "Error", inner);
  ASSERT(outer.cause != nullptr);
  EXPECT_EQ(outer.cause->code, std::optional<std::string>("ERR_INNER"));
  EXPECT_EQ(outer.code, std::nullopt);
}

TEST("errors that already carried a code keep their own member, which hides the base field") {
  const rt::fs::ErrnoError missing = rt::fs::errnoError(ENOENT, "open", "/missing");
  EXPECT_EQ(missing.code, std::string("ENOENT"));
  EXPECT_EQ(static_cast<const rt::JsError&>(missing).code, std::nullopt);
  const rt::CodedTypeError used("Body already used", "ERR_BODY_ALREADY_USED");
  EXPECT_EQ(used.code, std::string("ERR_BODY_ALREADY_USED"));
  EXPECT_EQ(static_cast<const rt::JsError&>(used).code, std::nullopt);
}

TEST("error.cause with any value: withCause sets causeValue, errorCause reads it generically") {
  // new Error("m", { cause: "x" })
  const rt::JsError withString = rt::withCause(rt::JsError("m"), rt::thrown(json::value("x")));
  ASSERT(withString.causeValue.has_value());
  EXPECT_EQ(rt::thrownValue(*withString.causeValue), std::optional<json::value>(json::value("x")));
  EXPECT(withString.cause == nullptr);
  EXPECT_EQ(withString.toString(), std::string("Error: m"));
  const std::optional<rt::Reason> stringCause = rt::errorCause(rt::makeReason(withString));
  ASSERT(stringCause.has_value());
  EXPECT_EQ(rt::reasonToString(*stringCause), std::string("x"));
  EXPECT(!rt::isError(*stringCause));
  // new Error("m"): no `cause` property.
  EXPECT(!rt::JsError("m").causeValue.has_value());
  EXPECT(!rt::errorCause(rt::makeReason(rt::JsError("m"))).has_value());
  // new Error("m", { cause: undefined }): `"cause" in error`, the value undefined (a null Reason).
  const std::optional<rt::Reason> undefinedCause = rt::errorCause(rt::makeReason(rt::withCause(rt::JsError("m"), nullptr)));
  ASSERT(undefinedCause.has_value());
  EXPECT(*undefinedCause == nullptr);
  // new TypeError("t", { cause: new RangeError("r") }): causeValue keeps the RangeError's C++
  // type, `cause` a copy for its existing readers; the TypeError stays a TypeError.
  const rt::Reason range = rt::makeReason(rt::RangeError("r"));
  const rt::TypeError typed = rt::withCause(rt::TypeError("t"), range);
  ASSERT(typed.cause != nullptr);
  EXPECT_EQ(typed.cause->name, std::string("RangeError"));
  EXPECT_EQ(typed.cause->message, std::string("r"));
  ASSERT(typed.causeValue.has_value());
  EXPECT(*typed.causeValue == range);
  bool rangeKept = false;
  try {
    std::rethrow_exception(*rt::errorCause(rt::makeReason(typed)));
  } catch (const rt::RangeError& caught) {
    rangeKept = caught.message == "r";
  } catch (...) {
  }
  EXPECT(rangeKept);
  EXPECT_EQ(rt::errorName(rt::makeReason(typed)), std::optional<std::string>("TypeError"));
  // An Error cause set the existing way (JsError::cause) reads back as a JsError copy.
  const rt::JsError older("outer", "Error", std::make_shared<rt::TypeError>("inner"));
  const std::optional<rt::Reason> olderCause = rt::errorCause(rt::makeReason(older));
  ASSERT(olderCause.has_value());
  EXPECT_EQ(rt::errorName(*olderCause), std::optional<std::string>("TypeError"));
  EXPECT_EQ(rt::errorMessage(*olderCause), std::optional<std::string>("inner"));
  // new Error("outer", { cause: new Error("inner", { cause: 42 }) }).cause.cause === 42
  const rt::Reason innerError = rt::makeReason(rt::withCause(rt::JsError("inner"), rt::thrown(json::value(42))));
  const rt::Reason outerError = rt::makeReason(rt::withCause(rt::JsError("outer"), innerError));
  const std::optional<rt::Reason> first = rt::errorCause(outerError);
  ASSERT(first.has_value());
  const std::optional<rt::Reason> second = rt::errorCause(*first);
  ASSERT(second.has_value());
  EXPECT_EQ(rt::reasonToString(*second), std::string("42"));
  // Not an Error, or undefined: no cause.
  EXPECT(!rt::errorCause(rt::thrown(json::value("x"))).has_value());
  EXPECT(!rt::errorCause(nullptr).has_value());
  // Error classes keeping their own causeValue member hide the base slot (read them through
  // their type); the generic reader then falls back to JsError::cause.
  struct OwnCause : rt::JsError {
    rt::Reason causeValue;
    OwnCause(std::string message_, rt::Reason cause_) : rt::JsError(std::move(message_)), causeValue(std::move(cause_)) {}
  };
  const OwnCause own("own", rt::thrown(json::value("v")));
  EXPECT(own.causeValue != nullptr);
  EXPECT(!static_cast<const rt::JsError&>(own).causeValue.has_value());
  EXPECT(!rt::errorCause(rt::makeReason(own)).has_value());
}
