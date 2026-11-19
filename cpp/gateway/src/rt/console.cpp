#include "uos/rt/console.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include <unistd.h>
#include <algorithm>
#include <cerrno>

namespace uos::rt::console {
  namespace {
    constexpr std::string_view truncated = "[truncated]";
    constexpr std::size_t maxOutput = 256 * 1024;
    constexpr unsigned maxDepth = 16;

    // Bound the input to serialization before escaping can expand it.
    struct DiagnosticBudget {
      std::size_t remaining = 32 * 1024;

      bool take(std::size_t size) {
        if (size > remaining) {
          remaining = 0;
          return false;
        }
        remaining -= size;
        return true;
      }

      json::string text(std::string_view source) { return json::string(take(source.size()) ? source : truncated); }

      json::value project(const json::value& value, unsigned depth = 0) {
        if (!take(32) || depth >= maxDepth) return json::string(truncated);
        if (const auto* source = value.if_string()) return text(*source);
        if (const auto* source = value.if_object()) {
          json::object fields;
          for (const auto& member : *source) {
            if (remaining < 32 || !take(member.key().size())) break;
            fields.emplace(member.key(), project(member.value(), depth + 1));
          }
          return fields;
        }
        if (const auto* source = value.if_array()) {
          json::array elements;
          std::size_t i = 0;
          for (; i < source->size() && i < 100 && remaining >= 32; ++i) elements.push_back(project((*source)[i], depth + 1));
          if (i < source->size()) elements.emplace_back(truncated);
          return elements;
        }
        return value;
      }

      json::value project(const ErrorInfo& info, unsigned depth = 0) {
        if (!take(32) || depth >= maxDepth) return json::string(truncated);
        json::object fields{{"name", text(info.name)}, {"message", text(info.message)}};
        if (!info.properties.empty()) fields.emplace("properties", project(info.properties, depth + 1));
        if (info.cause) fields.emplace("cause", project(*info.cause, depth + 1));
        return fields;
      }
    };

    ErrorInfo fromException(const std::exception& error, unsigned depth = 0) {
      ErrorInfo info;
      if (const auto* js = dynamic_cast<const JsError*>(&error)) {
        info.name = js->name;
        info.message = js->message;
        if (js->code) info.properties.emplace("code", *js->code);
        if (js->cause && depth < maxDepth) info.cause = std::make_shared<const ErrorInfo>(fromException(*js->cause, depth + 1));
      } else info.message = error.what();
      if (const auto* system = dynamic_cast<const fs::ErrnoError*>(&error)) {
        if (system->path) info.properties.emplace("path", *system->path);
        if (system->dest) info.properties.emplace("dest", *system->dest);
        if (system->fd) info.properties.emplace("fd", *system->fd);
        if (!system->syscall.empty()) info.properties.emplace("syscall", system->syscall);
        if (system->errno_) info.properties.emplace("errno", *system->errno_);
        info.properties.insert_or_assign("code", system->code);
      }
      return info;
    }

    std::vector<Capture*>& captures() {
      static std::vector<Capture*> stack;
      return stack;
    }

    using CaptureObserver = std::function<void(const Call&)>;
    std::vector<std::pair<Capture*, std::shared_ptr<CaptureObserver>>>& captureObservers() {
      static std::vector<std::pair<Capture*, std::shared_ptr<CaptureObserver>>> observers;
      return observers;
    }

    std::shared_ptr<CaptureObserver> observerFor(Capture* capture) {
      const auto& observers = captureObservers();
      const auto entry = std::find_if(observers.begin(), observers.end(), [capture](const auto& item) { return item.first == capture; });
      return entry == observers.end() ? nullptr : entry->second;
    }

    void writeAll(int fd, std::string_view text) {
      std::size_t written = 0;
      while (written < text.size()) {
        const ssize_t count = ::write(fd, text.data() + written, text.size() - written);
        if (count < 0) {
          if (errno == EINTR) continue;
          return;
        }
        written += static_cast<std::size_t>(count);
      }
    }
  }

  Value::Value(const json::value& value) {
    if (const auto* text = value.if_string()) data_.emplace<std::string>(*text);
    else data_.emplace<json::value>(value, json::storage_ptr{});
  }

  Value::Value(const json::object& value) : data_(json::value(value, {})) {}
  Value::Value(const json::array& value) : data_(json::value(value, {})) {}
  Value::Value(const std::exception& error) : Value(fromException(error)) {}

  Value::Value(const std::exception_ptr& error) {
    if (!error) return;
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& caught) {
      *this = Value(caught);
    } catch (...) {
      *this = Value(ErrorInfo{});
    }
  }

  json::value Value::asField() && {
    if (const auto* text = std::get_if<std::string>(&data_)) return json::value(*text);
    if (auto* value = std::get_if<json::value>(&data_)) return std::move(*value);
    if (kind() == Kind::error) return DiagnosticBudget{}.project(errorInfo());
    return nullptr;
  }

  Value Value::object(Properties properties) {
    Value out;
    auto& fields = out.data_.emplace<json::value>(json::object_kind).get_object();
    for (auto& [key, value] : properties)
      if (value.kind() != Kind::undefined) fields.insert_or_assign(key, std::move(value).asField());
    return out;
  }

  Value Value::array(std::vector<Value> elements) {
    Value out;
    auto& array = out.data_.emplace<json::value>(json::array_kind).get_array();
    array.reserve(elements.size());
    for (auto& value : elements) array.push_back(std::move(value).asField());
    return out;
  }

  Value::Kind Value::kind() const noexcept {
    if (isString()) return Kind::string;
    if (const auto* value = std::get_if<json::value>(&data_)) {
      if (value->is_object()) return Kind::object;
      if (value->is_array()) return Kind::array;
      if (value->is_bool()) return Kind::boolean;
      return value->is_null() ? Kind::null : Kind::number;
    }
    return std::holds_alternative<std::shared_ptr<const ErrorInfo>>(data_) ? Kind::error : Kind::undefined;
  }

  const std::string& Value::string() const noexcept {
    static const std::string empty;
    const auto* text = std::get_if<std::string>(&data_);
    return text ? *text : empty;
  }

  const json::value& Value::jsonValue() const noexcept {
    static const json::value null;
    const auto* value = std::get_if<json::value>(&data_);
    return value ? *value : null;
  }

  double Value::number() const noexcept { return jsonValue().is_number() ? jsonValue().to_number<double>() : 0; }
  bool Value::boolean() const noexcept { return jsonValue().is_bool() && jsonValue().get_bool(); }

  std::vector<Value> Value::elements() const {
    if (const auto* array = jsonValue().if_array()) return {array->begin(), array->end()};
    return {};
  }

  Properties Value::properties() const {
    Properties out;
    if (const auto* fields = jsonValue().if_object())
      for (const auto& member : *fields) out.emplace_back(std::string(member.key()), member.value());
    return out;
  }

  const ErrorInfo& Value::errorInfo() const {
    static const ErrorInfo empty;
    const auto* error = std::get_if<std::shared_ptr<const ErrorInfo>>(&data_);
    return error && *error ? **error : empty;
  }

  std::string format(Method, const std::vector<Value>& args) {
    std::string out;
    for (const auto& arg : args) {
      if (out.size() == maxOutput) break;
      if (&arg != args.data()) out += ' ';
      std::string rendered;
      std::string_view text = arg.string();
      if (!arg.isString()) {
        if (arg.kind() == Value::Kind::undefined) text = "undefined";
        else {
          DiagnosticBudget budget;
          rendered = jsonStringify(arg.kind() == Value::Kind::error ? budget.project(arg.errorInfo()) : budget.project(arg.jsonValue()));
          text = rendered;
        }
      }
      if (text.size() > maxOutput - out.size()) {
        if (truncated.size() <= maxOutput - out.size()) out += truncated;
        break;
      }
      out += text;
    }
    return out;
  }

  void write(Method method, std::vector<Value> args) {
    Call call{method, std::move(args), {}};
    call.text = format(method, call.args);
    auto& stack = captures();
    for (auto entry = stack.rbegin(); entry != stack.rend(); ++entry) {
      if (!(*entry)->handles(method)) continue;
      if (!(*entry)->record(call)) return;
    }
    const int fd = method == Method::warn || method == Method::error ? STDERR_FILENO : STDOUT_FILENO;
    writeAll(fd, call.text + "\n");
  }

  Capture::Capture(std::vector<Method> methods, bool passthrough) : methods_(std::move(methods)), passthrough_(passthrough) {
    // Initialize registry lifetime before any static Capture destructor is registered.
    (void)captureObservers();
    captures().push_back(this);
  }

  Capture::~Capture() {
    setObserver({});
    auto& stack = captures();
    stack.erase(std::remove(stack.begin(), stack.end(), this), stack.end());
  }

  std::vector<Call> Capture::calls(Method method) const {
    std::vector<Call> out;
    for (const auto& call : calls_)
      if (call.method == method) out.push_back(call);
    return out;
  }

  void Capture::setObserver(std::function<void(const Call&)> observer) {
    auto replacement = observer ? std::make_shared<CaptureObserver>(std::move(observer)) : nullptr;
    auto& observers = captureObservers();
    const auto entry = std::find_if(observers.begin(), observers.end(), [this](const auto& item) { return item.first == this; });
    if (entry == observers.end()) {
      if (replacement) observers.emplace_back(this, std::move(replacement));
      return;
    }
    const auto retired = std::move(entry->second);
    if (replacement) entry->second = std::move(replacement);
    else observers.erase(entry);
  }

  bool Capture::record(const Call& call) {
    const auto observer = observerFor(this);
    if (!observer) {
      calls_.push_back(call);
      return passthrough_;
    }
    const bool forward = passthrough_;
    const Call observed = call;
    calls_.push_back(observed);
    (*observer)(observed);
    return forward;
  }

  bool Capture::handles(Method method) const noexcept { return std::find(methods_.begin(), methods_.end(), method) != methods_.end(); }
}
