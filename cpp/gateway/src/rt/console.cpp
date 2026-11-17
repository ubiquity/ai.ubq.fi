#include "uos/rt/console.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/number.hpp"
#include <unistd.h>
#include <algorithm>
#include <cerrno>
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

    // JSON.stringify(string) for a WTF-8 string.
    std::string jsonQuote(std::string_view text) {
      static constexpr char HEX[] = "0123456789abcdef";
      std::string out = "\"";
      auto unitAt = [&](std::size_t at) -> unsigned {
        return ((static_cast<unsigned char>(text[at]) & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[at + 1]) & 0x3Fu) << 6) |
               (static_cast<unsigned char>(text[at + 2]) & 0x3Fu);
      };
      auto surrogateAt = [&](std::size_t at) {
        return at + 2 < text.size() && static_cast<unsigned char>(text[at]) == 0xED && static_cast<unsigned char>(text[at + 1]) >= 0xA0;
      };
      for (std::size_t index = 0; index < text.size();) {
        const auto c = static_cast<unsigned char>(text[index]);
        if (surrogateAt(index)) {
          const unsigned high = unitAt(index);
          if (high <= 0xDBFF && surrogateAt(index + 3) && unitAt(index + 3) >= 0xDC00) {
            const unsigned codePoint = 0x10000 + ((high - 0xD800) << 10) + (unitAt(index + 3) - 0xDC00);
            out += static_cast<char>(0xF0 | (codePoint >> 18));
            out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codePoint & 0x3F));
            index += 6;
          } else {
            out += "\\u";
            for (int shift = 12; shift >= 0; shift -= 4) out += HEX[(high >> shift) & 15];
            index += 3;
          }
          continue;
        }
        switch (c) {
          case '"': out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\b': out += "\\b"; break;
          case '\f': out += "\\f"; break;
          case '\n': out += "\\n"; break;
          case '\r': out += "\\r"; break;
          case '\t': out += "\\t"; break;
          default:
            if (c < 0x20) {
              out += "\\u00";
              out += HEX[c >> 4];
              out += HEX[c & 15];
            } else {
              out += static_cast<char>(c);
            }
        }
        ++index;
      }
      return out + "\"";
    }

    bool isIdentifier(std::string_view key) {
      if (key.empty()) return false;
      auto start = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; };
      if (!start(key[0])) return false;
      for (char c : key.substr(1))
        if (!start(c) && !(c >= '0' && c <= '9')) return false;
      return true;
    }

    // A canonical array index, which JS enumerates first in ascending order.
    std::optional<std::uint64_t> arrayIndex(std::string_view key) {
      if (key.empty() || key.size() > 10 || (key.size() > 1 && key[0] == '0')) return std::nullopt;
      std::uint64_t value = 0;
      for (char c : key) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
      }
      if (value > 4294967294ULL) return std::nullopt;
      return value;
    }

    std::vector<const std::pair<std::string, Value>*> ordered(const Properties& properties) {
      std::vector<const std::pair<std::string, Value>*> indexed, named;
      for (const auto& property : properties) (arrayIndex(property.first) ? indexed : named).push_back(&property);
      std::stable_sort(indexed.begin(), indexed.end(), [](auto* left, auto* right) { return *arrayIndex(left->first) < *arrayIndex(right->first); });
      indexed.insert(indexed.end(), named.begin(), named.end());
      return indexed;
    }

    std::string inspectNumber(double value) {
      if (value == 0 && std::signbit(value)) return "-0";
      return numberToString(value);
    }

    std::string errorToString(const ErrorInfo& error) {
      if (error.name.empty()) return error.message;
      if (error.message.empty()) return error.name;
      return error.name + ": " + error.message;
    }

    // `String(value)`, as %s prints it.
    std::string jsString(const Value& value) {
      switch (value.kind()) {
        case Value::Kind::undefined: return "undefined";
        case Value::Kind::null: return "null";
        case Value::Kind::boolean: return value.boolean() ? "true" : "false";
        case Value::Kind::number: return numberToString(value.number());
        case Value::Kind::string: return value.string();
        case Value::Kind::array: {
          std::string out;
          bool first = true;
          for (const auto& element : value.elements()) {
            if (!first) out += ',';
            first = false;
            if (element.kind() != Value::Kind::undefined && element.kind() != Value::Kind::null) out += jsString(element);
          }
          return out;
        }
        case Value::Kind::object: return "[object Object]";
        case Value::Kind::error: return errorToString(value.errorInfo());
      }
      return "";
    }

    // `Number(value)`.
    double jsNumber(const Value& value) {
      switch (value.kind()) {
        case Value::Kind::undefined: return std::numeric_limits<double>::quiet_NaN();
        case Value::Kind::null: return 0;
        case Value::Kind::boolean: return value.boolean() ? 1 : 0;
        case Value::Kind::number: return value.number();
        case Value::Kind::string: return stringToNumber(value.string());
        case Value::Kind::array: return stringToNumber(jsString(value));
        default: return std::numeric_limits<double>::quiet_NaN();
      }
    }

    // %d / %i: truncation saturated to 64 bits; beyond 1e21 the leading
    // integer digits of String(n).
    std::string formatInteger(double value) {
      if (std::isnan(value)) return "NaN";
      if (std::fabs(value) >= 1e21) {
        const std::string text = numberToString(value);
        std::size_t index = 0;
        std::string out;
        if (index < text.size() && text[index] == '-') out += text[index++];
        const std::size_t digits = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') out += text[index++];
        if (index == digits) return "NaN";
        return out;
      }
      const double truncated = std::trunc(value);
      if (truncated >= 9223372036854775807.0) return "9223372036854775807";
      if (truncated <= -9223372036854775808.0) return "-9223372036854775808";
      return std::to_string(static_cast<long long>(truncated));
    }

    // JSON.stringify(value); std::nullopt is undefined (prints nothing).
    std::optional<std::string> jsonStringify(const Value& value) {
      switch (value.kind()) {
        case Value::Kind::undefined: return std::nullopt;
        case Value::Kind::null: return "null";
        case Value::Kind::boolean: return value.boolean() ? "true" : "false";
        case Value::Kind::number: return std::isfinite(value.number()) ? numberToString(value.number()) : "null";
        case Value::Kind::string: return jsonQuote(value.string());
        case Value::Kind::array: {
          std::string out = "[";
          bool first = true;
          for (const auto& element : value.elements()) {
            if (!first) out += ',';
            first = false;
            out += jsonStringify(element).value_or("null");
          }
          return out + "]";
        }
        case Value::Kind::object:
        case Value::Kind::error: {
          const auto& errorProperties = value.errorInfo().properties;
          const Properties& properties = value.kind() == Value::Kind::object ? value.properties() : errorProperties ? *errorProperties : emptyProperties();
          std::string out = "{";
          bool first = true;
          for (const auto* property : ordered(properties)) {
            auto text = jsonStringify(property->second);
            if (!text) continue;
            if (!first) out += ',';
            first = false;
            out += jsonQuote(property->first) + ":" + *text;
          }
          return out + "}";
        }
      }
      return std::nullopt;
    }

    class Printer {
    public:
      explicit Printer(Method method_) : method(method_) {}
      std::string out;

      void put(std::string_view text) {
        out += text;
        const auto newline = text.rfind('\n');
        if (newline != std::string_view::npos) lineStart = out.size() - (text.size() - newline - 1);
      }

      void value(const Value& item, int depth) {
        switch (item.kind()) {
          case Value::Kind::undefined: put("undefined"); break;
          case Value::Kind::null: put("null"); break;
          case Value::Kind::boolean: put(item.boolean() ? "true" : "false"); break;
          case Value::Kind::number: put(inspectNumber(item.number())); break;
          case Value::Kind::string: put(jsonQuote(item.string())); break;
          case Value::Kind::array: array(item.elements(), depth); break;
          case Value::Kind::object: object(item.properties(), depth); break;
          case Value::Kind::error: error(item.errorInfo()); break;
        }
      }

      // A format string followed by `args[next...]`; returns the next unused index.
      std::size_t format(std::string_view text, const std::vector<Value>& args, std::size_t next) {
        std::string literal;
        auto flush = [&] {
          put(literal);
          literal.clear();
        };
        for (std::size_t index = 0; index < text.size(); ++index) {
          if (text[index] != '%' || index + 1 >= text.size()) {
            literal += text[index];
            continue;
          }
          const char specifier = text[index + 1];
          if (specifier == '%') {
            literal += '%';
            ++index;
            continue;
          }
          if (std::string_view("sdifjoOc").find(specifier) == std::string_view::npos || next >= args.size()) {
            literal += '%';
            continue;
          }
          ++index;
          const Value& arg = args[next++];
          switch (specifier) {
            case 's': literal += jsString(arg); break;
            case 'd':
            case 'i': literal += formatInteger(jsNumber(arg)); break;
            case 'f': literal += numberToString(jsNumber(arg)); break;
            case 'j': literal += jsonStringify(arg).value_or(""); break;
            case 'o':
            case 'O':
              flush();
              value(arg, 0);
              break;
            default: break;
          }
        }
        flush();
        return next;
      }

    private:
      Method method;
      std::size_t lineStart = 0;

      std::size_t lineLength() const { return out.size() - lineStart; }

      void newline(int indent) {
        put("\n");
        put(std::string(static_cast<std::size_t>(indent) * 2, ' '));
      }

      // An element that forces its array onto several lines: an object that
      // is not collapsed, any array or Error, a string with a line break.
      static bool expands(const Value& item, int depth) {
        switch (item.kind()) {
          case Value::Kind::object: return depth < 3;
          case Value::Kind::array:
          case Value::Kind::error: return true;
          case Value::Kind::string: return item.string().find('\n') != std::string::npos;
          default: return false;
        }
      }

      void array(const std::vector<Value>& elements, int depth) {
        if (elements.empty()) {
          put("[]");
          return;
        }
        bool multiline = elements.size() > 10;
        for (const auto& element : elements) multiline = multiline || expands(element, depth + 1);
        if (!multiline) {
          put("[ ");
          for (std::size_t index = 0; index < elements.size(); ++index) {
            if (index) put(", ");
            value(elements[index], depth + 1);
          }
          put(" ]");
          return;
        }
        put("[");
        newline(depth + 1);
        const std::size_t shown = std::min<std::size_t>(elements.size(), 100);
        for (std::size_t index = 0; index < shown; ++index) {
          value(elements[index], depth + 1);
          if (index + 1 < shown) {
            put(",");
            if (lineLength() > 80) newline(depth + 1);
            else put(" ");
          }
        }
        if (elements.size() > shown) {
          put(",");
          newline(depth + 1);
          put("... " + std::to_string(elements.size() - shown) + " more items");
        }
        newline(depth);
        put("]");
      }

      void object(const Properties& properties, int depth) {
        if (properties.empty()) {
          put("{}");
          return;
        }
        if (depth >= 3) {
          put("[Object ...]");
          return;
        }
        put("{");
        for (const auto* property : ordered(properties)) {
          newline(depth + 1);
          put(isIdentifier(property->first) ? property->first : jsonQuote(property->first));
          put(": ");
          value(property->second, depth + 1);
          put(",");
        }
        newline(depth);
        put("}");
      }

      void error(const ErrorInfo& info) {
        if (info.native) put(info.message);
        else if ((info.name == "Error" || info.name.empty()) && !info.message.empty()) put(std::string(method == Method::warn ? "warn" : "error") + ": " + info.message);
        else put(info.name + ": " + info.message);
        put("\n");
        if (info.properties && !info.properties->empty()) {
          std::vector<const std::pair<std::string, Value>*> lines;
          const std::pair<std::string, Value>* code = nullptr;
          for (const auto* property : ordered(*info.properties)) {
            if (property->first == "code") code = property;
            else lines.push_back(property);
          }
          if (code) lines.push_back(code);
          std::size_t width = 0;
          for (const auto* line : lines) width = std::max(width, line->first.size());
          for (std::size_t index = 0; index < lines.size(); ++index) {
            put(std::string(width - lines[index]->first.size() + 1, ' ') + lines[index]->first + ": ");
            value(lines[index]->second, 0);
            put(lines[index]->first == "code" ? "\n" : ",\n");
          }
          put("\n");
        }
        if (info.cause) {
          put("\n");
          error(*info.cause);
        }
      }
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

  std::string format(Method method, const std::vector<Value>& args) {
    Printer printer(method);
    std::size_t index = 0;
    while (index < args.size()) {
      if (index > 0) printer.put(" ");
      const Value& arg = args[index++];
      if (arg.isString()) {
        if (index < args.size()) index = printer.format(arg.string(), args, index);
        else printer.put(arg.string());
      } else {
        printer.value(arg, 0);
      }
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
