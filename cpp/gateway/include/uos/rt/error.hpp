#pragma once
#include <concepts>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace uos::rt {
  // A thrown JavaScript Error. `name` and `message` carry exactly the text the
  // TypeScript produces; `cause` mirrors `new Error(message, { cause })`.
  class JsError : public std::exception {
  public:
    std::string name;
    std::string message;
    std::shared_ptr<const JsError> cause;
    // `error.code` (std::nullopt is undefined): set it after construction for an Error the
    // TS gives a code (`Object.assign(new Error(m), { code })`, `class X extends Error {
    // code = "..." }`). Not part of String(error). The rt errors that already carried a code
    // keep their own `code` member, which hides this one (read them through their type):
    // fs::ErrnoError, CodedTypeError, CodedError, InvalidURLError, NodeURLError, SQLiteError.
    std::optional<std::string> code;
    // `error.cause` as any JavaScript value (C++ addition beside `cause`, which holds only Error
    // causes): std::nullopt is "no own `cause` property" (then `cause`, when set, is the cause);
    // a Reason (an rt::Reason, which is std::exception_ptr) is the value exactly as given: a null
    // Reason is `cause: undefined`, rt::thrown(json) a non-Error value, an Error's exception_ptr
    // that Error with its C++ type. Set both with rt::withCause; read either with rt::errorCause.
    // The error classes that already kept a `causeValue` member of their own
    // (ResponsesStreamError, CodexError, metered/auth_refresh ErrorWithCause, ...) hide this one:
    // read theirs through their type.
    std::optional<std::exception_ptr> causeValue;

    explicit JsError(std::string message_, std::string name_ = "Error", std::shared_ptr<const JsError> cause_ = nullptr)
      : name(std::move(name_)), message(std::move(message_)), cause(std::move(cause_)) {}
    const char* what() const noexcept override { return message.c_str(); }
    // `String(error)`: "Name: message", or just the name when the message is empty.
    std::string toString() const { return message.empty() ? name : name + ": " + message; }
  };

  struct TypeError : JsError {
    explicit TypeError(std::string message_, std::shared_ptr<const JsError> cause_ = nullptr) : JsError(std::move(message_), "TypeError", std::move(cause_)) {}
  };
  struct RangeError : JsError {
    explicit RangeError(std::string message_, std::shared_ptr<const JsError> cause_ = nullptr) : JsError(std::move(message_), "RangeError", std::move(cause_)) {}
  };
  struct SyntaxError : JsError {
    explicit SyntaxError(std::string message_, std::shared_ptr<const JsError> cause_ = nullptr) : JsError(std::move(message_), "SyntaxError", std::move(cause_)) {}
  };

  // `new E(message, { cause })` with any cause value: `error` with causeValue set to `cause`
  // exactly as given and, when `cause` holds an rt::JsError, `cause` set to a copy of it (what the
  // existing readers of JsError::cause expect). `cause` is an rt::Reason (std::exception_ptr: a
  // null one is `undefined`, rt::thrown(json) a non-Error value, rt::makeReason(error) an Error).
  //   new Error("Metered request failed", { cause: value })
  //     -> rt::withCause(rt::JsError("Metered request failed"), value)
  template <class E>
    requires std::derived_from<std::remove_cvref_t<E>, JsError>
  std::remove_cvref_t<E> withCause(E&& error, std::exception_ptr cause) {
    std::remove_cvref_t<E> out(std::forward<E>(error));
    if (cause) {
      try {
        std::rethrow_exception(cause);
      } catch (const JsError& inner) {
        out.cause = std::make_shared<JsError>(inner);
      } catch (...) {
      }
    }
    out.causeValue = std::move(cause);
    return out;
  }

  // `error.cause` read generically, for any rt::Reason (std::exception_ptr): std::nullopt when the
  // reason is not an rt::JsError or has no `cause` property (`"cause" in error` is false), else the
  // cause as a Reason: JsError::causeValue when set, otherwise JsError::cause as a copy (an
  // rt::JsError with the cause's name, message, code and causes; its C++ subclass is not kept).
  //   error.cause === undefined   ->  !rt::errorCause(e) || !*rt::errorCause(e)
  //   error.cause instanceof Error / .message / String(error.cause)
  //                               ->  rt::isError(*c) / rt::errorMessage(*c) / rt::reasonToString(*c)
  inline std::optional<std::exception_ptr> errorCause(const std::exception_ptr& reason) {
    if (!reason) return std::nullopt;
    try {
      std::rethrow_exception(reason);
    } catch (const JsError& error) {
      if (error.causeValue) return error.causeValue;
      if (error.cause) return std::make_exception_ptr(JsError(*error.cause));
    } catch (...) {
    }
    return std::nullopt;
  }
}
