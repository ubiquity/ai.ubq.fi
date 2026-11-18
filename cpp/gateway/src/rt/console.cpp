#include "uos/rt/console.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/text_codec.hpp"
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

namespace uos::rt::console {
  namespace {
    const std::vector<Value>& emptyElements() {
      static const std::vector<Value> empty;
      return empty;
    }
    const Properties& emptyProperties() {
      static const Properties empty;
      return empty;
    }

    constexpr std::string_view truncated = "[truncated]";
    constexpr std::size_t maxOutput = 256 * 1024;
    constexpr unsigned maxDepth = 16;

    std::string scalarNumber(double number) {
      char buffer[64];
      const auto result = std::to_chars(buffer, buffer + sizeof buffer, number);
      if (result.ec != std::errc{}) return std::string(truncated);
      return std::string(buffer, result.ptr);
    }

    double numericArgument(const Value& value) {
      if (value.kind() == Value::Kind::number) return value.number();
      if (value.kind() == Value::Kind::boolean) return value.boolean() ? 1 : 0;
      if (value.kind() == Value::Kind::null) return 0;
      if (value.isString() && !value.string().empty()) {
        const auto& text = value.string();
        double number = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
        if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) return number;
      }
      return std::numeric_limits<double>::quiet_NaN();
    }

    class JsonProjection {
    public:
      json::value value(const Value& item, unsigned depth = 0) {
        using Kind = Value::Kind;
        if (remaining < 32) return json::string(truncated);
        remaining -= 32;
        if (depth >= maxDepth) return json::string(truncated);
        switch (item.kind()) {
          case Kind::undefined:
          case Kind::null: return nullptr;
          case Kind::boolean: return item.boolean();
          case Kind::number: {
            const double number = item.number();
            if (!std::isfinite(number)) return nullptr;
            if (number >= -0x1p63 && number < 0x1p63 && std::trunc(number) == number) return static_cast<std::int64_t>(number);
            return number;
          }
          case Kind::string: return string(item.string());
          case Kind::object: return object(item.properties(), depth);
          case Kind::error: return error(item.errorInfo(), depth);
          case Kind::array: {
            json::array array;
            const auto& elements = item.elements();
            std::size_t index = 0;
            for (; index < elements.size() && index < 100 && remaining >= 32; ++index) array.push_back(value(elements[index], depth + 1));
            if (index < elements.size()) array.emplace_back(truncated);
            return array;
          }
        }
        return nullptr;
      }

    private:
      // Charge bytes for text and for each projected value before building the JSON tree.
      std::size_t remaining = 32 * 1024;

      json::string string(std::string_view text) {
        if (text.size() > remaining) {
          remaining = 0;
          return json::string(truncated);
        }
        remaining -= text.size();
        if (rt::isUtf8(text)) return json::string(text);
        return json::string(rt::decodeUtf8(text, {.ignoreBOM = true}));
      }

      json::object object(const Properties& properties, unsigned depth) {
        json::object object;
        for (const auto& [key, item] : properties) {
          if (remaining < 32) break;
          if (item.kind() == Value::Kind::undefined) continue;
          const auto member = string(key);
          object.insert_or_assign(member, value(item, depth + 1));
        }
        return object;
      }

      json::value error(const ErrorInfo& info, unsigned depth) {
        if (depth >= maxDepth || remaining < 32) return json::string(truncated);
        remaining -= 32;
        json::object error{{"name", string(info.name)}, {"message", string(info.message)}};
        if (info.properties && !info.properties->empty()) error.emplace("properties", object(*info.properties, depth));
        if (info.cause) error.emplace("cause", this->error(*info.cause, depth + 1));
        return error;
      }
    };

    class Printer {
    public:
      std::string out;
      bool full() const { return exhausted || out.size() == maxOutput; }

      void put(std::string_view text) {
        if (full()) return;
        if (text.size() <= maxOutput - out.size()) out += text;
        else {
          if (truncated.size() <= maxOutput - out.size()) out += truncated;
          exhausted = true;
        }
      }

      void value(const Value& item, bool quoteString = false) {
        if (item.kind() == Value::Kind::undefined) put("undefined");
        else if (item.isString() && !quoteString) put(item.string());
        else if (item.kind() == Value::Kind::number) put(scalarNumber(item.number()));
        else put(json::serialize(JsonProjection{}.value(item)));
      }

      std::size_t format(std::string_view text, const std::vector<Value>& args, std::size_t next) {
        while (!text.empty() && !full()) {
          const auto percent = text.find('%');
          put(text.substr(0, percent));
          if (percent == std::string_view::npos || full()) break;
          text.remove_prefix(percent + 1);
          const char token = text.empty() ? '\0' : text.front();
          if (token == '%') {
            put("%");
            text.remove_prefix(1);
            continue;
          }
          if (next == args.size() || std::string_view("sdifjoOc").find(token) == std::string_view::npos) {
            put("%");
            continue;
          }
          text.remove_prefix(1);
          const Value& item = args[next++];
          switch (token) {
            case 's': value(item); break;
            case 'o':
            case 'O': value(item, true); break;
            case 'j':
              if (item.kind() != Value::Kind::undefined) put(json::serialize(JsonProjection{}.value(item)));
              break;
            case 'd':
            case 'i': put(scalarNumber(std::trunc(numericArgument(item)))); break;
            case 'f': put(scalarNumber(numericArgument(item))); break;
            default: break;
          }
        }
        return next;
      }

    private:
      bool exhausted = false;
    };

    std::shared_ptr<const ErrorInfo> fromJsError(const JsError& error) {
      auto info = std::make_shared<ErrorInfo>();
      info->name = error.name;
      info->message = error.message;
      if (const auto* system = dynamic_cast<const fs::ErrnoError*>(&error)) {
        info->native = system->name == "Error";
        auto properties = std::make_shared<Properties>();
        if (system->path) properties->emplace_back("path", Value(*system->path));
        if (system->dest) properties->emplace_back("dest", Value(*system->dest));
        if (system->fd) properties->emplace_back("fd", Value(*system->fd));
        if (!system->syscall.empty()) properties->emplace_back("syscall", Value(system->syscall));
        if (system->errno_) properties->emplace_back("errno", Value(*system->errno_));
        properties->emplace_back("code", Value(system->code));
        info->properties = std::move(properties);
      }
      if (error.cause) info->cause = fromJsError(*error.cause);
      return info;
    }

    std::shared_ptr<const ErrorInfo> fromException(const std::exception& error) {
      if (const auto* js = dynamic_cast<const JsError*>(&error)) return fromJsError(*js);
      auto info = std::make_shared<ErrorInfo>();
      info->message = error.what();
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
    switch (value.kind()) {
      case json::kind::null: kind_ = Kind::null; break;
      case json::kind::bool_:
        kind_ = Kind::boolean;
        number_ = value.get_bool() ? 1 : 0;
        break;
      case json::kind::int64:
      case json::kind::uint64:
      case json::kind::double_:
        kind_ = Kind::number;
        number_ = value.to_number<double>();
        break;
      case json::kind::string:
        kind_ = Kind::string;
        text_ = std::string(value.get_string());
        break;
      case json::kind::array: *this = Value(value.get_array()); break;
      case json::kind::object: *this = Value(value.get_object()); break;
    }
  }

  Value::Value(const json::object& value) : kind_(Kind::object) {
    auto properties = std::make_shared<Properties>();
    for (const auto& member : value) properties->emplace_back(std::string(member.key()), Value(member.value()));
    properties_ = std::move(properties);
  }

  Value::Value(const json::array& value) : kind_(Kind::array) {
    auto elements = std::make_shared<std::vector<Value>>();
    for (const auto& element : value) elements->emplace_back(element);
    elements_ = std::move(elements);
  }

  Value::Value(const std::exception& error) : kind_(Kind::error), error_(fromException(error)) {}

  Value::Value(const std::exception_ptr& error) {
    if (!error) return;
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& caught) {
      kind_ = Kind::error;
      error_ = fromException(caught);
    } catch (...) {
      kind_ = Kind::error;
      error_ = std::make_shared<ErrorInfo>();
    }
  }

  Value Value::object(Properties properties) {
    Value out;
    out.kind_ = Kind::object;
    out.properties_ = std::make_shared<Properties>(std::move(properties));
    return out;
  }

  Value Value::array(std::vector<Value> elements) {
    Value out;
    out.kind_ = Kind::array;
    out.elements_ = std::make_shared<std::vector<Value>>(std::move(elements));
    return out;
  }

  Value Value::error(const std::exception& error, Properties properties) {
    ErrorInfo info = *fromException(error);
    auto merged = info.properties ? std::make_shared<Properties>(*info.properties) : std::make_shared<Properties>();
    for (auto& property : properties) merged->push_back(std::move(property));
    info.properties = std::move(merged);
    return Value::error(std::move(info));
  }

  Value Value::error(ErrorInfo info) {
    Value out;
    out.kind_ = Kind::error;
    if (!info.properties) info.properties = std::make_shared<Properties>();
    out.error_ = std::make_shared<const ErrorInfo>(std::move(info));
    return out;
  }

  const std::vector<Value>& Value::elements() const { return elements_ ? *elements_ : emptyElements(); }
  const Properties& Value::properties() const { return properties_ ? *properties_ : emptyProperties(); }
  const ErrorInfo& Value::errorInfo() const {
    static const ErrorInfo none;
    return error_ ? *error_ : none;
  }

  std::string format(Method, const std::vector<Value>& args) {
    Printer printer;
    for (std::size_t index = 0; index < args.size() && !printer.full();) {
      if (index > 0) printer.put(" ");
      const Value& arg = args[index++];
      if (arg.isString() && index < args.size()) index = printer.format(arg.string(), args, index);
      else printer.value(arg);
    }
    return std::move(printer.out);
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
