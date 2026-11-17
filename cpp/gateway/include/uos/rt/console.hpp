#pragma once
// console.log / info / debug (stdout) and console.warn / error (stderr) with
// Bun 1.4's formatting, plus a capture hook for ported tests. Each call writes
// one line: the arguments formatted and joined by " ", then "\n".
//
// Formatting (Bun's): a top-level string prints raw; a string followed by more
// arguments is a format string (%s %d %i %f %j %o %O %c %%, consuming the next
// arguments; anything else stays literal). Other values print like Bun's
// inspect: numbers as JS (-0 is "-0"), objects multi-line with two-space
// indent and a comma after every property, keys quoted unless ASCII
// identifiers, nested strings JSON-quoted, arrays on one line up to 10
// primitive elements, else multi-line wrapped after 80 columns (100 elements,
// then "... N more items"), objects nested 3 or more levels deep as
// "[Object ...]" (arrays never collapse). Errors print as Bun prints an Error
// without stack frames: "error: <message>" (console.warn: "warn: <message>")
// for name "Error" with a message, else "<name>: <message>"; own properties
// follow on their own lines (keys right-aligned, a comma after each but
// `code`, which comes last), then the cause chain.
// Bun additionally prints a source excerpt and stack frames for Errors that
// have them and dumps DOMException as an object; the port cannot reproduce
// those and prints the header form (documented in docs/rt/platform.md).
#include "uos/rt/prelude.hpp"
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uos::rt::console {
  enum class Method { log, info, warn, error, debug };

  class Value;
  using Properties = std::vector<std::pair<std::string, Value>>;

  // An Error as console prints it.
  struct ErrorInfo {
    std::string name = "Error";
    std::string message;
    // Bun's own system errors (rt::fs::ErrnoError): the header is the message alone.
    bool native = false;
    // Own enumerable properties other than name/message/stack/cause, in order.
    std::shared_ptr<const Properties> properties;
    std::shared_ptr<const ErrorInfo> cause;
  };

  // One console argument: undefined, null, a boolean, a number, a string, an
  // array, an object (ordered properties), or an Error.
  class Value {
  public:
    enum class Kind { undefined, null, boolean, number, string, array, object, error };

    // undefined
    Value() noexcept = default;
    Value(std::nullopt_t) noexcept {}
    // null
    Value(std::nullptr_t) noexcept : kind_(Kind::null) {}
    Value(bool value) noexcept : kind_(Kind::boolean), number_(value ? 1 : 0) {}
    template <class N>
      requires(std::is_arithmetic_v<N> && !std::is_same_v<N, bool> && !std::is_same_v<N, char>)
    Value(N value) noexcept : kind_(Kind::number), number_(static_cast<double>(value)) {}
    Value(std::string_view text) : kind_(Kind::string), text_(text) {}
    Value(const char* text) : Value(std::string_view(text)) {}
    Value(const std::string& text) : Value(std::string_view(text)) {}
    // A parsed JSON value; a JSON string is a string.
    Value(const json::value& value);
    Value(const json::object& value);
    Value(const json::array& value);
    Value(const json::string& value) : Value(std::string_view(value)) {}
    // An Error: rt::JsError (name, message, cause chain), rt::fs::ErrnoError
    // (Bun's native fields), or any other exception as Error(what()).
    Value(const std::exception& error);
    // A caught value (rt::Reason): a null pointer is undefined, an exception an Error.
    // A thrown non-Error (rt::ThrownValue) prints its what(); pass
    // rt::thrownValue(reason) instead to print the value itself.
    Value(const std::exception_ptr& error);
    template <class T> Value(const std::optional<T>& value) : Value(value ? Value(*value) : Value()) {}

    // `{ key: value, ... }` with values that JSON cannot hold (Errors, undefined).
    static Value object(Properties properties);
    static Value array(std::vector<Value> elements);
    // An Error with own properties, as a custom error class carries them
    // (`class CodexError extends Error { code; status }`).
    static Value error(const std::exception& error, Properties properties);
    static Value error(ErrorInfo info);

    Kind kind() const noexcept { return kind_; }
    bool isString() const noexcept { return kind_ == Kind::string; }
    // The string (Kind::string), else "".
    const std::string& string() const noexcept { return text_; }
    double number() const noexcept { return number_; }
    bool boolean() const noexcept { return number_ != 0; }
    const std::vector<Value>& elements() const;
    const Properties& properties() const;
    const ErrorInfo& errorInfo() const;

    // `args[i] === "text"` in a captured call.
    friend bool operator==(const Value& value, std::string_view text) noexcept { return value.kind_ == Kind::string && value.text_ == text; }

  private:
    Kind kind_ = Kind::undefined;
    double number_ = 0;
    std::string text_;
    std::shared_ptr<const std::vector<Value>> elements_;
    std::shared_ptr<const Properties> properties_;
    std::shared_ptr<const ErrorInfo> error_;
  };

  // The text one call prints, without the trailing newline.
  std::string format(Method method, const std::vector<Value>& args);
  // Formats and writes one call (or hands it to the active Capture).
  void write(Method method, std::vector<Value> args);

  template <class... A> void log(const A&... args) { write(Method::log, {Value(args)...}); }
  template <class... A> void info(const A&... args) { write(Method::info, {Value(args)...}); }
  template <class... A> void warn(const A&... args) { write(Method::warn, {Value(args)...}); }
  template <class... A> void error(const A&... args) { write(Method::error, {Value(args)...}); }
  template <class... A> void debug(const A&... args) { write(Method::debug, {Value(args)...}); }

  // One recorded call: the arguments as the TS mock receives them and the line
  // Bun would print.
  struct Call {
    Method method;
    std::vector<Value> args;
    std::string text;
  };

  // Test hook for the TS patterns `console.info = (...args) => logs.push(args)`,
  // `console.warn = () => {}` and `spyOn(console, "error").mockImplementation(...)`:
  // while alive, calls to `methods` are recorded instead of printed. With
  // `passthrough` they are also passed on (to an enclosing Capture or the
  // terminal), like a replacement that calls the original. Nested captures:
  // the innermost one for a method sees the call first.
  class Capture {
  public:
    explicit Capture(std::vector<Method> methods = {Method::log, Method::info, Method::warn, Method::error, Method::debug}, bool passthrough = false);
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    const std::vector<Call>& calls() const noexcept { return calls_; }
    // The recorded calls of one method.
    std::vector<Call> calls(Method method) const;
    void clear() noexcept { calls_.clear(); }

    // Optional synchronous notification after storage and before passthrough. An empty
    // observer clears it. The Call stays stable for the callback; copy it to retain it.
    // Reentrant logging and observer replacement/clear are supported. Exceptions propagate
    // after recording and prevent forwarding. Keep Capture lifetimes/stack unchanged
    // during callbacks. Observer state is external so the Capture layout is unchanged.
    void setObserver(std::function<void(const Call&)> observer);

    // Internal: offers a call; true when it must also be passed on.
    bool record(const Call& call);
    bool handles(Method method) const noexcept;

  private:
    std::vector<Method> methods_;
    bool passthrough_;
    std::vector<Call> calls_;
  };
}
