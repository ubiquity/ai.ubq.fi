#pragma once
// Console arguments render as literal text or compact JSON, joined by spaces.
#include "uos/rt/prelude.hpp"
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uos::rt::console {
  enum class Method { log, info, warn, error, debug };

  class Value;
  using Properties = std::vector<std::pair<std::string, Value>>;

  struct ErrorInfo {
    std::string name = "Error";
    std::string message;
    json::object properties;  // Explicit diagnostics such as code, path and errno.
    std::shared_ptr<const ErrorInfo> cause;
  };

  class Value {
  public:
    enum class Kind { undefined, null, boolean, number, string, array, object, error };

    Value() noexcept = default;
    Value(std::nullptr_t) : data_(json::value(nullptr)) {}
    template <class N> requires std::is_arithmetic_v<N>
    Value(N value) : data_(json::value(value)) {}
    Value(std::string_view text) : data_(std::string(text)) {}
    Value(const char* text) : Value(std::string_view(text)) {}
    Value(std::string text) : data_(std::move(text)) {}
    Value(const json::string& text) : Value(std::string_view(text)) {}
    Value(const json::value& value);
    Value(const json::object& value);
    Value(const json::array& value);
    Value(const std::exception& error);
    Value(const std::exception_ptr& error);
    explicit Value(ErrorInfo error) : data_(std::make_shared<const ErrorInfo>(std::move(error))) {}
    template <class T> Value(const std::optional<T>& value) : Value(value ? Value(*value) : Value()) {}

    // Existing mixed argument builders project nested errors into JSON fields.
    static Value object(Properties properties);
    static Value array(std::vector<Value> elements);

    Kind kind() const noexcept;
    bool isString() const noexcept { return std::holds_alternative<std::string>(data_); }
    const std::string& string() const noexcept;
    double number() const noexcept;
    bool boolean() const noexcept;
    const json::value& jsonValue() const noexcept;
    std::vector<Value> elements() const;
    Properties properties() const;
    const ErrorInfo& errorInfo() const;

    friend bool operator==(const Value& value, std::string_view text) noexcept { return value.isString() && value.string() == text; }

  private:
    json::value asField() &&;
    std::variant<std::monostate, std::string, json::value, std::shared_ptr<const ErrorInfo>> data_;
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

  // One recorded call: the captured arguments and the text sent to the sink.
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
