#include "uos/rt/json.hpp"

#include "uos/rt/error.hpp"

#if defined(UOS_GATEWAY_SIMDJSON)
#include "json_simdjson.hpp"
#else
#include <boost/version.hpp>
#if BOOST_VERSION == 109200
#include <boost/json/basic_parser_impl.hpp>
#include <boost/json/value_stack.hpp>
#endif
#endif

#include <algorithm>
#include <charconv>
#include <limits>
#include <memory>

namespace uos::rt {
  namespace {
    constexpr double kMaxSafeInteger = 9007199254740991.0;
    constexpr std::int64_t kMaxSafeInt64 = 9007199254740991;
    constexpr char kHexDigits[] = "0123456789abcdef";

    [[noreturn]] void throwStackOverflow() { throw RangeError("Maximum call stack size exceeded."); }

    std::string_view view(json::string_view text) noexcept { return {text.data(), text.size()}; }
    std::string_view view(const json::string& text) noexcept { return {text.data(), text.size()}; }

    double numberOf(const json::value& value) noexcept {
      switch (value.kind()) {
        case json::kind::int64:
          return static_cast<double>(value.get_int64());
        case json::kind::uint64:
          return static_cast<double>(value.get_uint64());
        case json::kind::double_:
          return value.get_double();
        default:
          return std::numeric_limits<double>::quiet_NaN();
      }
    }

    bool isCanonicalNumber(const json::value& value) noexcept {
      switch (value.kind()) {
        case json::kind::int64:
          return value.get_int64() >= -kMaxSafeInt64 && value.get_int64() <= kMaxSafeInt64;
        case json::kind::uint64:
          return false;
        case json::kind::double_: {
          const double number = value.get_double();
          return !(number >= -kMaxSafeInteger && number <= kMaxSafeInteger && std::trunc(number) == number && !(number == 0 && std::signbit(number)));
        }
        default:
          return true;
      }
    }

    void canonicalizeTop(json::value& value) {
      if (!isCanonicalNumber(value)) value = makeNumber(numberOf(value));
    }

    std::uint32_t indexValue(std::string_view key) noexcept {
      std::uint32_t value = 0;
      for (const char c : key) value = value * 10u + static_cast<std::uint32_t>(c - '0');
      return value;
    }

    // The members of an object in JS enumeration order.
    template <class Pair, class Object>
    std::vector<Pair*> orderedMembers(Object& object) {
      std::vector<Pair*> members;
      members.reserve(object.size());
      bool anyIndex = false;
      for (auto& member : object) {
        members.push_back(&member);
        if (!anyIndex && isArrayIndex(view(member.key()))) anyIndex = true;
      }
      if (anyIndex) {
        const auto split = std::stable_partition(members.begin(), members.end(), [](Pair* member) { return isArrayIndex(view(member->key())); });
        std::sort(members.begin(), split, [](Pair* a, Pair* b) { return indexValue(view(a->key())) < indexValue(view(b->key())); });
      }
      return members;
    }

    bool hasIndexKey(const json::object& object) noexcept {
      for (const auto& member : object)
        if (isArrayIndex(view(member.key()))) return true;
      return false;
    }

    // Rewrites the storage order of an object into JS order.
    void restoreOrder(json::object& object) {
      const auto members = orderedMembers<json::key_value_pair>(object);
      json::object rebuilt(object.storage());
      rebuilt.reserve(object.size());
      for (auto* member : members) rebuilt.emplace(member->key(), std::move(member->value()));
      object = std::move(rebuilt);
    }

    void defineMember(json::object& object, std::string_view key, json::value value) {
      canonicalizeTop(value);
      if (const auto found = object.find(key); found != object.end()) {
        found->value() = std::move(value);
        return;
      }
      if (!isArrayIndex(key)) {
        object.emplace(key, std::move(value));
        return;
      }
      const std::uint32_t index = indexValue(key);
      std::size_t position = 0;
      for (const auto& member : object) {
        const std::string_view existing = view(member.key());
        if (!isArrayIndex(existing) || indexValue(existing) > index) break;
        ++position;
      }
      if (position == object.size()) {
        object.emplace(key, std::move(value));
        return;
      }
      json::object rebuilt(object.storage());
      rebuilt.reserve(object.size() + 1);
      std::size_t cursor = 0;
      for (auto& member : object) {
        if (cursor++ == position) rebuilt.emplace(key, std::move(value));
        rebuilt.emplace(member.key(), std::move(member.value()));
      }
      object = std::move(rebuilt);
    }

    void appendNumber(std::string& out, const json::value& value) {
      if (value.is_int64() && value.get_int64() >= -kMaxSafeInt64 && value.get_int64() <= kMaxSafeInt64) {
        char buffer[24];
        const auto result = std::to_chars(buffer, buffer + sizeof buffer, value.get_int64());
        out.append(buffer, result.ptr);
        return;
      }
      const double number = numberOf(value);
      if (!std::isfinite(number)) out += "null";
      else out += numberToString(number);
    }

    void appendUnicodeEscape(std::string& out, std::uint32_t unit) {
      out += "\\u";
      out += kHexDigits[(unit >> 12) & 0xF];
      out += kHexDigits[(unit >> 8) & 0xF];
      out += kHexDigits[(unit >> 4) & 0xF];
      out += kHexDigits[unit & 0xF];
    }

    // The escape of a code unit below 0x100, or 0 when it is written as is.
    char shortEscape(std::uint32_t unit) noexcept {
      switch (unit) {
        case 0x08:
          return 'b';
        case 0x09:
          return 't';
        case 0x0A:
          return 'n';
        case 0x0C:
          return 'f';
        case 0x0D:
          return 'r';
        case '"':
          return '"';
        case '\\':
          return '\\';
        default:
          return unit < 0x20 ? 'u' : '\0';
      }
    }

    void appendEscapedUnit(std::string& out, std::uint32_t unit) {
      const char escape = shortEscape(unit);
      if (escape == 'u') {
        appendUnicodeEscape(out, unit);
      } else {
        out += '\\';
        out += escape;
      }
    }

    // WTF::appendQuotedJSONString over the UTF-16 code units of the string.
    void appendQuoted(std::string& out, std::string_view text) {
      out += '"';
      if (isAscii(text)) {
        std::size_t runStart = 0;
        for (std::size_t index = 0; index < text.size(); ++index) {
          const auto unit = static_cast<unsigned char>(text[index]);
          if (unit >= 0x20 && unit != '"' && unit != '\\') continue;
          out.append(text.substr(runStart, index - runStart));
          appendEscapedUnit(out, unit);
          runStart = index + 1;
        }
        out.append(text.substr(runStart));
      } else {
        const std::u16string units = toUtf16(text);
        std::size_t runStart = 0;
        const auto flush = [&](std::size_t until) {
          if (until > runStart) out += fromUtf16(std::u16string_view(units).substr(runStart, until - runStart));
        };
        for (std::size_t index = 0; index < units.size(); ++index) {
          const char16_t unit = units[index];
          if (unit < 0x100) {
            if (!shortEscape(unit)) continue;
            flush(index);
            appendEscapedUnit(out, unit);
            runStart = index + 1;
            continue;
          }
          if (unit < 0xD800 || unit > 0xDFFF) continue;
          if (unit <= 0xDBFF && index + 1 < units.size() && units[index + 1] >= 0xDC00 && units[index + 1] <= 0xDFFF) {
            ++index;
            continue;
          }
          flush(index);
          appendUnicodeEscape(out, unit);
          runStart = index + 1;
        }
        flush(units.size());
      }
      out += '"';
    }

    // JSON.stringify with an explicit stack of open arrays and objects (no recursion).
    class Serializer {
    public:
      Serializer(std::string gap, const JsonReplacer* replacer) : gap_(std::move(gap)), replacer_(replacer) {}

      // SerializeJSONProperty of the root (key ""): false when the result is undefined.
      bool root(const json::value& value) {
        if (!replacer_) {
          emit(value, nullptr);
        } else {
          std::optional<json::value> replaced = (*replacer_)("", value);
          if (!replaced) return false;
          emitOwned(std::move(*replaced));
        }
        run();
        return true;
      }

      std::string out;

    private:
      struct Frame {
        const json::array* array = nullptr;
        const json::object* object = nullptr;
        std::vector<const json::key_value_pair*> ordered;  // objects with array-index keys
        std::size_t index = 0;
        std::size_t start = 0;  // output size before '{'
        bool any = false;
        std::unique_ptr<json::value> holder;  // a replacer result being serialised
      };

      void newline() {
        if (gap_.empty()) return;
        out += '\n';
        out += indent_;
      }

      // Writes a primitive, or opens a frame for a non-empty array or an object.
      void emit(const json::value& value, std::unique_ptr<json::value> holder) {
        switch (value.kind()) {
          case json::kind::null:
            out += "null";
            return;
          case json::kind::bool_:
            out += value.get_bool() ? "true" : "false";
            return;
          case json::kind::int64:
          case json::kind::uint64:
          case json::kind::double_:
            appendNumber(out, value);
            return;
          case json::kind::string:
            appendQuoted(out, view(value.get_string()));
            return;
          case json::kind::array:
          case json::kind::object:
            break;
        }
        if (frames_.size() >= kJsonMaxDepth) throwStackOverflow();
        if (value.is_array() && value.get_array().empty()) {
          out += "[]";
          return;
        }
        Frame frame;
        frame.holder = std::move(holder);
        frame.start = out.size();
        if (value.is_array()) {
          frame.array = &value.get_array();
          out += '[';
        } else {
          frame.object = &value.get_object();
          if (hasIndexKey(*frame.object)) frame.ordered = orderedMembers<const json::key_value_pair>(*frame.object);
          out += '{';
        }
        indent_ += gap_;
        frames_.push_back(std::move(frame));
      }

      void emitOwned(json::value value) {
        if (!value.is_array() && !value.is_object()) {
          emit(value, nullptr);
          return;
        }
        auto holder = std::make_unique<json::value>(std::move(value));
        const json::value* owned = holder.get();
        emit(*owned, std::move(holder));
      }

      void close(char bracket) {
        indent_.resize(indent_.size() - gap_.size());
        newline();
        out += bracket;
        frames_.pop_back();
      }

      void run() {
        while (!frames_.empty()) {
          Frame& frame = frames_.back();
          if (frame.array) {
            if (frame.index == frame.array->size()) {
              close(']');
              continue;
            }
            const std::size_t index = frame.index++;
            if (index) out += ',';
            newline();
            const json::value& element = (*frame.array)[index];
            if (!replacer_) {
              emit(element, nullptr);
              continue;
            }
            std::optional<json::value> replaced = (*replacer_)(std::to_string(index), element);
            if (!replaced) out += "null";
            else emitOwned(std::move(*replaced));
            continue;
          }
          const std::size_t count = frame.ordered.empty() ? frame.object->size() : frame.ordered.size();
          if (frame.index == count) {
            if (frame.any) {
              close('}');
            } else {
              indent_.resize(indent_.size() - gap_.size());
              out.resize(frame.start);
              out += "{}";
              frames_.pop_back();
            }
            continue;
          }
          const json::key_value_pair& member = frame.ordered.empty() ? frame.object->begin()[frame.index] : *frame.ordered[frame.index];
          ++frame.index;
          const std::size_t mark = out.size();
          if (frame.any) out += ',';
          newline();
          appendQuoted(out, view(member.key()));
          out += gap_.empty() ? ":" : ": ";
          if (!replacer_) {
            frame.any = true;
            emit(member.value(), nullptr);
            continue;
          }
          std::optional<json::value> replaced = (*replacer_)(view(member.key()), member.value());
          if (!replaced) {
            out.resize(mark);
            continue;
          }
          frame.any = true;
          emitOwned(std::move(*replaced));
        }
      }

      std::string gap_;
      std::string indent_;
      const JsonReplacer* replacer_;
      std::vector<Frame> frames_;
    };

    std::string gapFromString(std::string_view gap) {
      if (isAscii(gap)) return std::string(gap.substr(0, 10));
      const std::u16string units = toUtf16(gap);
      return fromUtf16(std::u16string_view(units).substr(0, 10));
    }

#if !defined(UOS_GATEWAY_SIMDJSON) && BOOST_VERSION == 109200
    constexpr std::size_t kBoostParseDepth = 32;

    // The scanner validates the grammar and decodes strings. Build the same
    // DOM type as Boost's parser while retaining raw member counts and numbers.
    class BoostJsonHandler {
    public:
      static constexpr std::size_t max_array_size = json::array::max_size();
      static constexpr std::size_t max_object_size = json::object::max_size();
      static constexpr std::size_t max_string_size = json::string::max_size();
      static constexpr std::size_t max_key_size = json::string::max_size();

      bool on_document_begin(boost::system::error_code&) noexcept {
        values_.reset();
        return true;
      }
      bool on_document_end(boost::system::error_code&) noexcept { return true; }
      bool on_array_begin(boost::system::error_code&) noexcept { return true; }
      bool on_object_begin(boost::system::error_code&) noexcept { return true; }

      bool on_array_end(std::size_t count, boost::system::error_code&) {
        values_.push_array(count);
        return true;
      }
      bool on_object_end(std::size_t count, boost::system::error_code&) {
        // Each counted member consumes at least two distinct source bytes.
        members_ += count;
        values_.push_object(count);
        return true;
      }
      bool on_string_part(json::string_view text, std::size_t, boost::system::error_code&) {
        values_.push_chars(text);
        return true;
      }
      bool on_string(json::string_view text, std::size_t, boost::system::error_code&) {
        values_.push_string(text);
        return true;
      }
      bool on_key_part(json::string_view text, std::size_t, boost::system::error_code&) {
        values_.push_chars(text);
        return true;
      }
      bool on_key(json::string_view text, std::size_t, boost::system::error_code&) {
        values_.push_key(text);
        return true;
      }
      bool on_bool(bool value, boost::system::error_code&) {
        values_.push_bool(value);
        return true;
      }
      bool on_null(boost::system::error_code&) {
        values_.push_null();
        return true;
      }

      // This adapter supplies one final buffer. Abort the attempt if a number
      // is delivered in parts so the callbacks below only see whole lexemes.
      bool on_number_part(json::string_view, boost::system::error_code& error) noexcept { return refuse(error); }

      bool on_int64(std::int64_t, json::string_view lexeme, boost::system::error_code& error) {
        const std::string_view text = view(lexeme);
        if (text.empty()) return refuse(error);
        const char* end = text.data() + text.size();
        std::int64_t integer = 0;
        const auto parsed = std::from_chars(text.data(), end, integer);
        if (parsed.ec == std::errc{} && parsed.ptr == end && integer >= -kMaxSafeInt64 && integer <= kMaxSafeInt64 &&
            (integer != 0 || text.front() != '-')) {
          values_.push_int64(integer);
          return true;
        }
        return pushNumber(lexeme, error);
      }
      bool on_uint64(std::uint64_t, json::string_view lexeme, boost::system::error_code& error) {
        return pushNumber(lexeme, error);
      }
      bool on_double(double, json::string_view lexeme, boost::system::error_code& error) {
        return pushNumber(lexeme, error);
      }
      bool on_comment_part(json::string_view, boost::system::error_code& error) noexcept { return refuse(error); }
      bool on_comment(json::string_view, boost::system::error_code& error) noexcept { return refuse(error); }

      std::size_t memberCount() const noexcept { return members_; }
      json::value release() noexcept { return values_.release(); }

    private:
      static bool refuse(boost::system::error_code& error) noexcept {
        error = json::error::syntax;
        return false;
      }
      bool pushNumber(json::string_view lexeme, boost::system::error_code& error) {
        if (lexeme.empty()) return refuse(error);
        const json::value number = makeNumber(stringToNumber(view(lexeme)));
        if (number.is_int64()) values_.push_int64(number.get_int64());
        else values_.push_double(number.get_double());
        return true;
      }

      alignas(json::value) unsigned char scratch_[1024];
      json::value_stack values_{json::storage_ptr{}, scratch_, sizeof scratch_};
      std::size_t members_ = 0;
    };

    // Recursion is bounded by kBoostParseDepth. Every stored member corresponds
    // to one source member. A missing member means a duplicate was collapsed.
    bool countBoostMembers(const json::value& value, std::size_t& remaining) noexcept {
      if (const auto* object = value.if_object()) {
        if (object->size() > remaining) return false;
        remaining -= object->size();
        for (const auto& member : *object) {
          const std::string_view key = view(member.key());
          if (key == "__proto__" || isArrayIndex(key) || !countBoostMembers(member.value(), remaining)) return false;
        }
      } else if (const auto* array = value.if_array()) {
        for (const auto& element : *array)
          if (!countBoostMembers(element, remaining)) return false;
      }
      return true;
    }

    std::optional<json::value> tryParseBoost(std::string_view text) {
      if (text.empty()) return std::nullopt;
      json::parse_options options;
      options.max_depth = kBoostParseDepth;
      options.numbers = json::number_precision::none;
      json::basic_parser<BoostJsonHandler> parser(options);
      boost::system::error_code error;
      const std::size_t consumed = parser.write_some(false, text.data(), text.size(), error);
      if (error || !parser.done() || consumed != text.size()) return std::nullopt;

      std::size_t members = parser.handler().memberCount();
      json::value value = parser.handler().release();
      if (members != 0 && (!countBoostMembers(value, members) || members != 0)) return std::nullopt;
      return std::optional<json::value>{std::move(value)};
    }
#endif

    // ---- JSON.parse: JavaScriptCore's LiteralParser in StrictJSON mode -----------------------
    enum class Token : std::uint8_t {
      LBracket,
      RBracket,
      LBrace,
      RBrace,
      Comma,
      Colon,
      String,
      Identifier,
      Number,
      True,
      False,
      Null,
      End,
      Dot,
      Error,
      LParen,
      RParen,
      Assign,
      Semi,
    };

    // TokenTypesOfLatin1Characters.
    Token latin1TokenType(std::uint32_t c) noexcept {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_') return Token::Identifier;
      if ((c >= '0' && c <= '9') || c == '-') return Token::Number;
      switch (c) {
        case '"':
        case '\'':
          return Token::String;
        case '(':
          return Token::LParen;
        case ')':
          return Token::RParen;
        case ',':
          return Token::Comma;
        case '.':
          return Token::Dot;
        case ':':
          return Token::Colon;
        case ';':
          return Token::Semi;
        case '=':
          return Token::Assign;
        case '[':
          return Token::LBracket;
        case ']':
          return Token::RBracket;
        case '{':
          return Token::LBrace;
        case '}':
          return Token::RBrace;
        default:
          return Token::Error;
      }
    }

    bool isAsciiDigit(std::uint32_t c) noexcept { return c >= '0' && c <= '9'; }
    bool isAsciiHexDigit(std::uint32_t c) noexcept { return isAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
    std::uint32_t hexValue(std::uint32_t c) noexcept { return isAsciiDigit(c) ? c - '0' : (c | 0x20u) - 'a' + 10; }

    template <class Unit>
    class Parser {
    public:
      explicit Parser(std::basic_string_view<Unit> text) : cursor_(text.data()), end_(text.data() + text.size()) {}

      json::value parse() {
        next();
        json::value result = parseValue();
        if (type_ != Token::End) fail("Unable to parse JSON string");
        return result;
      }

    private:
      struct Frame {
        json::value container;
        std::string key;
        bool hasIndexKey = false;
      };

      static std::uint32_t unit(Unit value) noexcept {
        if constexpr (std::is_same_v<Unit, char>) return static_cast<unsigned char>(value);
        else return static_cast<std::uint32_t>(value);
      }

      // The code units [start, start + count) as a JS string.
      static std::string units(const Unit* start, std::size_t count) {
        if constexpr (std::is_same_v<Unit, char>) return std::string(start, count);
        else return fromUtf16(std::u16string_view(start, count));
      }

      [[noreturn]] static void fail(std::string_view message) { throw SyntaxError("JSON Parse error: " + std::string(message)); }

      Token next() {
        while (cursor_ < end_ && (*cursor_ == ' ' || *cursor_ == '\t' || *cursor_ == '\n' || *cursor_ == '\r')) ++cursor_;
        start_ = cursor_;
        if (cursor_ == end_) return type_ = Token::End;
        const std::uint32_t c = unit(*cursor_);
        if (c < 0x100) {
          const Token type = latin1TokenType(c);
          switch (type) {
            case Token::String:
              if (c == '\'') fail("Single quotes (') are not allowed in JSON");
              return lexString();
            case Token::Identifier:
              return lexIdentifier(c);
            case Token::Number:
              return lexNumber();
            case Token::Error:
              break;
            default:
              ++cursor_;
              return type_ = type;
          }
        }
        fail("Unrecognized token '" + units(cursor_, 1) + "'");
      }

      bool matches(std::string_view word) const noexcept {
        if (static_cast<std::size_t>(end_ - cursor_) < word.size()) return false;
        for (std::size_t index = 1; index < word.size(); ++index)
          if (unit(cursor_[index]) != static_cast<unsigned char>(word[index])) return false;
        return true;
      }

      Token lexIdentifier(std::uint32_t first) {
        if (first == 't' && matches("true")) {
          cursor_ += 4;
          return type_ = Token::True;
        }
        if (first == 'f' && matches("false")) {
          cursor_ += 5;
          return type_ = Token::False;
        }
        if (first == 'n' && matches("null")) {
          cursor_ += 4;
          return type_ = Token::Null;
        }
        while (cursor_ < end_) {
          const std::uint32_t c = unit(*cursor_);
          if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isAsciiDigit(c) || c == '_' || c == '$')) break;
          ++cursor_;
        }
        return type_ = Token::Identifier;
      }

      // -?(0 | [1-9][0-9]*) ('.' [0-9]+)? ([eE][+-]?[0-9]+)?; an exponent marker that is not
      // followed by digits is left for the next token.
      Token lexNumber() {
        if (cursor_ < end_ && *cursor_ == '-') ++cursor_;
        if (cursor_ < end_ && *cursor_ == '0') {
          ++cursor_;
        } else if (cursor_ < end_ && unit(*cursor_) >= '1' && unit(*cursor_) <= '9') {
          ++cursor_;
          while (cursor_ < end_ && isAsciiDigit(unit(*cursor_))) ++cursor_;
        } else {
          fail("Invalid number");
        }
        if (cursor_ < end_ && *cursor_ == '.') {
          ++cursor_;
          if (cursor_ >= end_ || !isAsciiDigit(unit(*cursor_))) fail("Invalid digits after decimal point");
          ++cursor_;
          while (cursor_ < end_ && isAsciiDigit(unit(*cursor_))) ++cursor_;
        }
        if (cursor_ < end_ && (*cursor_ == 'e' || *cursor_ == 'E')) {
          const Unit* marker = cursor_;
          ++cursor_;
          if (cursor_ < end_ && (*cursor_ == '-' || *cursor_ == '+')) ++cursor_;
          if (cursor_ >= end_ || !isAsciiDigit(unit(*cursor_))) {
            cursor_ = marker;
          } else {
            ++cursor_;
            while (cursor_ < end_ && isAsciiDigit(unit(*cursor_))) ++cursor_;
          }
        }
        std::string literal;
        literal.reserve(static_cast<std::size_t>(cursor_ - start_));
        for (const Unit* digit = start_; digit < cursor_; ++digit) literal += static_cast<char>(unit(*digit));
        number_ = stringToNumber(literal);
        return type_ = Token::Number;
      }

      static bool isSafeStringUnit(std::uint32_t c) noexcept { return c >= 0x100 || (c >= 0x20 && c != '"' && c != '\\'); }

      void append(const Unit* from, const Unit* to) {
        if constexpr (std::is_same_v<Unit, char>) builder_.append(from, static_cast<std::size_t>(to - from));
        else wideBuilder_.append(from, static_cast<std::size_t>(to - from));
      }

      void appendUnit(std::uint32_t value) {
        if constexpr (std::is_same_v<Unit, char>) appendCodePoint(builder_, value);
        else wideBuilder_.push_back(static_cast<char16_t>(value));
      }

      bool builderEmpty() const noexcept {
        if constexpr (std::is_same_v<Unit, char>) return builder_.empty();
        else return wideBuilder_.empty();
      }

      Token lexString() {
        ++cursor_;
        const Unit* runStart = cursor_;
        while (cursor_ < end_ && isSafeStringUnit(unit(*cursor_))) ++cursor_;
        if (cursor_ < end_ && *cursor_ == '"') {
          string_ = units(runStart, static_cast<std::size_t>(cursor_ - runStart));
          ++cursor_;
          return type_ = Token::String;
        }
        builder_.clear();
        wideBuilder_.clear();
        bool first = true;
        do {
          if (!first) {
            runStart = cursor_;
            while (cursor_ < end_ && isSafeStringUnit(unit(*cursor_))) ++cursor_;
            if (!builderEmpty()) append(runStart, cursor_);
          }
          first = false;
          if (cursor_ < end_ && *cursor_ == '\\') {
            if (builderEmpty() && runStart < cursor_) append(runStart, cursor_);
            ++cursor_;
            if (cursor_ >= end_) fail("Unterminated string");
            switch (unit(*cursor_)) {
              case '"':
                appendUnit('"');
                ++cursor_;
                break;
              case '\\':
                appendUnit('\\');
                ++cursor_;
                break;
              case '/':
                appendUnit('/');
                ++cursor_;
                break;
              case 'b':
                appendUnit('\b');
                ++cursor_;
                break;
              case 'f':
                appendUnit('\f');
                ++cursor_;
                break;
              case 'n':
                appendUnit('\n');
                ++cursor_;
                break;
              case 'r':
                appendUnit('\r');
                ++cursor_;
                break;
              case 't':
                appendUnit('\t');
                ++cursor_;
                break;
              case 'u': {
                if (end_ - cursor_ < 5) fail("\\u must be followed by 4 hex digits");
                std::uint32_t value = 0;
                for (int index = 1; index < 5; ++index) {
                  if (!isAsciiHexDigit(unit(cursor_[index]))) fail("\"\\" + units(cursor_, 5) + "\" is not a valid unicode escape");
                  value = value * 16 + hexValue(unit(cursor_[index]));
                }
                appendUnit(value);
                cursor_ += 5;
                break;
              }
              default:
                fail("Invalid escape character " + units(cursor_, 1));
            }
          }
        } while (cursor_ != runStart && cursor_ < end_ && *cursor_ != '"');
        if (cursor_ >= end_ || *cursor_ != '"') fail("Unterminated string");
        if constexpr (std::is_same_v<Unit, char>) string_ = std::move(builder_);
        else string_ = fromUtf16(wideBuilder_);
        ++cursor_;
        return type_ = Token::String;
      }

      std::string identifierMessage() const {
        constexpr std::size_t kMaxLength = 200;
        const auto length = static_cast<std::size_t>(cursor_ - start_);
        std::string text;
        for (std::size_t index = 0; index < std::min(length, kMaxLength); ++index) text += static_cast<char>(unit(start_[index]));
        return "Unexpected identifier \"" + text + (length > kMaxLength ? "..." : "") + "\"";
      }

      static void enter(const std::vector<Frame>& stack) {
        if (stack.size() >= kJsonMaxDepth) throwStackOverflow();
      }

      json::value parseValue() {
        std::vector<Frame> stack;
        json::value last;
        while (true) {
          switch (type_) {
            case Token::LBracket:
              enter(stack);
              if (next() == Token::RBracket) {
                next();
                last = json::array();
                break;
              }
              stack.push_back({json::array(), std::string(), false});
              continue;
            case Token::LBrace: {
              enter(stack);
              const Token type = next();
              if (type == Token::String) {
                std::string key = std::move(string_);
                if (next() != Token::Colon) fail("Expected ':' before value in object property definition");
                next();
                stack.push_back({json::object(), std::move(key), false});
                continue;
              }
              if (type != Token::RBrace) fail("Expected '}'");
              next();
              last = json::object();
              break;
            }
            case Token::String:
              last = json::string(string_);
              next();
              break;
            case Token::Number:
              last = makeNumber(number_);
              next();
              break;
            case Token::Null:
              next();
              last = nullptr;
              break;
            case Token::True:
              next();
              last = true;
              break;
            case Token::False:
              next();
              last = false;
              break;
            case Token::RBracket:
              fail("Unexpected token ']'");
            case Token::LParen:
              fail("Unexpected token '('");
            case Token::RParen:
              fail("Unexpected token ')'");
            case Token::RBrace:
              fail("Unexpected token '}'");
            case Token::Colon:
              fail("Unexpected token ':'");
            case Token::Comma:
              fail("Unexpected token ','");
            case Token::Dot:
              fail("Unexpected token '.'");
            case Token::Assign:
              fail("Unexpected token '='");
            case Token::Semi:
              fail("Unexpected token ';'");
            case Token::End:
              fail("Unexpected EOF");
            case Token::Identifier:
              fail(identifierMessage());
            case Token::Error:
              fail("Could not parse value expression");
          }
          // A value is complete: hand it to the enclosing array or object.
          while (true) {
            if (stack.empty()) return last;
            Frame& frame = stack.back();
            if (frame.container.is_array()) {
              frame.container.get_array().push_back(std::move(last));
              if (type_ == Token::Comma) {
                if (next() == Token::RBracket) fail("Unexpected comma at the end of array expression");
                break;
              }
              if (type_ != Token::RBracket) fail("Expected ']'");
            } else {
              json::object& object = frame.container.get_object();
              if (!frame.hasIndexKey && isArrayIndex(frame.key)) frame.hasIndexKey = true;
              object.insert_or_assign(frame.key, std::move(last));
              if (type_ == Token::Comma) {
                if (next() != Token::String) fail("Property name must be a string literal");
                frame.key = std::move(string_);
                if (next() != Token::Colon) fail("Expected ':' before value in object property definition");
                next();
                break;
              }
              if (type_ != Token::RBrace) fail("Expected '}'");
              if (frame.hasIndexKey) restoreOrder(object);
            }
            next();
            last = std::move(frame.container);
            stack.pop_back();
          }
        }
      }

      const Unit* cursor_;
      const Unit* end_;
      const Unit* start_ = nullptr;
      Token type_ = Token::Error;
      std::string string_;
      double number_ = 0;
      std::string builder_;
      std::u16string wideBuilder_;
    };

    // `depth` counts the arrays and objects already entered; entering one more must stay
    // within kJsonMaxDepth.
    void checkDepth(std::size_t depth) {
      if (depth >= kJsonMaxDepth) throwStackOverflow();
    }

    void joinInto(std::string& out, const json::array& array, std::size_t depth);

    void stringInto(std::string& out, const json::value& value, std::size_t depth) {
      switch (value.kind()) {
        case json::kind::null:
          out += "null";
          return;
        case json::kind::bool_:
          out += value.get_bool() ? "true" : "false";
          return;
        case json::kind::int64:
        case json::kind::uint64:
        case json::kind::double_:
          out += numberToString(numberOf(value));
          return;
        case json::kind::string:
          append(out, view(value.get_string()));
          return;
        case json::kind::array:
          joinInto(out, value.get_array(), depth);
          return;
        case json::kind::object:
          out += "[object Object]";
          return;
      }
    }

    // Array.prototype.join(","): null and undefined elements print as "".
    void joinInto(std::string& out, const json::array& array, std::size_t depth) {
      checkDepth(depth);
      for (std::size_t index = 0; index < array.size(); ++index) {
        if (index) out += ',';
        if (!array[index].is_null()) stringInto(out, array[index], depth + 1);
      }
    }

    bool deepEqualAt(const json::value& a, const json::value& b, std::size_t depth) {
      if (isNumber(a) || isNumber(b)) return isNumber(a) && isNumber(b) && sameValue(numberOf(a), numberOf(b));
      if (a.kind() != b.kind()) return false;
      switch (a.kind()) {
        case json::kind::null:
          return true;
        case json::kind::bool_:
          return a.get_bool() == b.get_bool();
        case json::kind::string:
          return a.get_string() == b.get_string();
        case json::kind::array: {
          checkDepth(depth);
          const auto& left = a.get_array();
          const auto& right = b.get_array();
          if (left.size() != right.size()) return false;
          for (std::size_t index = 0; index < left.size(); ++index)
            if (!deepEqualAt(left[index], right[index], depth + 1)) return false;
          return true;
        }
        case json::kind::object: {
          checkDepth(depth);
          const auto& left = a.get_object();
          const auto& right = b.get_object();
          if (left.size() != right.size()) return false;
          for (const auto& member : left) {
            const auto found = right.find(member.key());
            if (found == right.end() || !deepEqualAt(member.value(), found->value(), depth + 1)) return false;
          }
          return true;
        }
        default:
          return false;
      }
    }

    bool matchesAt(const json::value& received, const json::value& expected, std::size_t depth) {
      if (expected.is_array() || expected.is_object()) checkDepth(depth);
      if (expected.is_array()) {
        if (!received.is_array() || received.get_array().size() != expected.get_array().size()) return false;
        for (std::size_t index = 0; index < expected.get_array().size(); ++index)
          if (!matchesAt(received.get_array()[index], expected.get_array()[index], depth + 1)) return false;
        return true;
      }
      if (expected.is_object()) {
        if (received.is_object()) {
          for (const auto& member : expected.get_object()) {
            const auto found = received.get_object().find(member.key());
            if (found == received.get_object().end() || !matchesAt(found->value(), member.value(), depth + 1)) return false;
          }
          return true;
        }
        if (received.is_array()) {
          const auto& elements = received.get_array();
          for (const auto& member : expected.get_object()) {
            const std::string_view key = view(member.key());
            if (key == "length") {
              if (!isNumber(member.value()) || numberOf(member.value()) != static_cast<double>(elements.size())) return false;
            } else if (!isArrayIndex(key) || indexValue(key) >= elements.size() || !matchesAt(elements[indexValue(key)], member.value(), depth + 1)) {
              return false;
            }
          }
          return true;
        }
        return false;
      }
      return deepEqualAt(received, expected, depth);
    }

    void canonicalizeAt(json::value& value, std::size_t depth) {
      if (value.is_array()) {
        checkDepth(depth);
        for (auto& element : value.get_array()) canonicalizeAt(element, depth + 1);
      } else if (value.is_object()) {
        checkDepth(depth);
        for (auto& member : value.get_object()) canonicalizeAt(member.value(), depth + 1);
      } else {
        canonicalizeTop(value);
      }
    }

    void flatInto(json::array& out, const json::array& values, double depth, std::size_t level) {
      checkDepth(level);
      for (const auto& value : values) {
        if (depth > 0 && value.is_array()) flatInto(out, value.get_array(), depth - 1, level + 1);
        else out.push_back(value);
      }
    }

    template <class Keyed>
    void sortByKey(std::vector<Keyed>& keyed) {
      detail::stableSort(keyed, [](const Keyed& a, const Keyed& b) { return compare(b.first, a.first) < 0; });
    }
  }

  json::value makeNumber(double value) {
    if (value >= -kMaxSafeInteger && value <= kMaxSafeInteger && std::trunc(value) == value && !(value == 0 && std::signbit(value)))
      return json::value(static_cast<std::int64_t>(value));
    return json::value(value);
  }

  json::value makeNumber(std::int64_t value) {
    if (value >= -kMaxSafeInt64 && value <= kMaxSafeInt64) return json::value(value);
    return json::value(static_cast<double>(value));
  }

  json::value canonicalNumbers(json::value value) {
    canonicalizeAt(value, 0);
    return value;
  }

  bool isNumber(const json::value& value) noexcept { return value.is_int64() || value.is_uint64() || value.is_double(); }

  std::string_view typeOf(const json::value& value) noexcept {
    switch (value.kind()) {
      case json::kind::bool_:
        return "boolean";
      case json::kind::int64:
      case json::kind::uint64:
      case json::kind::double_:
        return "number";
      case json::kind::string:
        return "string";
      default:
        return "object";
    }
  }

  std::string_view typeOf(const json::value* value) noexcept { return value ? typeOf(*value) : "undefined"; }

  bool isRecord(const json::value& value) noexcept { return value.is_object(); }
  bool isRecord(const json::value* value) noexcept { return value && value->is_object(); }
  bool isNullish(const json::value* value) noexcept { return !value || value->is_null(); }

  bool truthy(const json::value& value) noexcept {
    switch (value.kind()) {
      case json::kind::null:
        return false;
      case json::kind::bool_:
        return value.get_bool();
      case json::kind::int64:
      case json::kind::uint64:
      case json::kind::double_:
        return truthy(numberOf(value));
      case json::kind::string:
        return !value.get_string().empty();
      default:
        return true;
    }
  }

  bool truthy(const json::value* value) noexcept { return value && truthy(*value); }
  bool truthy(double value) noexcept { return !(value == 0 || std::isnan(value)); }
  bool truthy(std::string_view value) noexcept { return !value.empty(); }

  double toNumber(const json::value& value) {
    switch (value.kind()) {
      case json::kind::null:
        return 0;
      case json::kind::bool_:
        return value.get_bool() ? 1 : 0;
      case json::kind::int64:
      case json::kind::uint64:
      case json::kind::double_:
        return numberOf(value);
      case json::kind::string:
        return stringToNumber(view(value.get_string()));
      default:
        return stringToNumber(toString(value));
    }
  }

  double toNumber(const json::value* value) { return value ? toNumber(*value) : std::numeric_limits<double>::quiet_NaN(); }

  std::string toString(const json::value& value) {
    std::string out;
    stringInto(out, value, 0);
    return out;
  }

  std::string toString(const json::value* value) { return value ? toString(*value) : "undefined"; }

  namespace Number {
    bool isFinite(const json::value& value) noexcept { return isNumber(value) && std::isfinite(numberOf(value)); }
    bool isNaN(const json::value& value) noexcept { return isNumber(value) && std::isnan(numberOf(value)); }
    bool isInteger(const json::value& value) noexcept { return isNumber(value) && isInteger(numberOf(value)); }
    bool isSafeInteger(const json::value& value) noexcept { return isNumber(value) && isSafeInteger(numberOf(value)); }
  }

  bool isNaN(const json::value& value) { return std::isnan(toNumber(value)); }
  bool isFinite(const json::value& value) { return std::isfinite(toNumber(value)); }

  bool sameValue(double a, double b) noexcept {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    return a == b && std::signbit(a) == std::signbit(b);
  }

  bool sameValueZero(double a, double b) noexcept { return a == b || (std::isnan(a) && std::isnan(b)); }

  bool strictEquals(const json::value& a, const json::value& b) noexcept {
    if (isNumber(a) || isNumber(b)) return isNumber(a) && isNumber(b) && numberOf(a) == numberOf(b);
    if (a.kind() != b.kind()) return false;
    switch (a.kind()) {
      case json::kind::null:
        return true;
      case json::kind::bool_:
        return a.get_bool() == b.get_bool();
      case json::kind::string:
        return a.get_string() == b.get_string();
      default:
        return &a == &b;
    }
  }

  bool deepEqual(const json::value& a, const json::value& b) { return deepEqualAt(a, b, 0); }
  bool matchesObject(const json::value& received, const json::value& expected) {
    if (!(received.is_object() || received.is_array()) || !(expected.is_object() || expected.is_array())) return false;
    return matchesAt(received, expected, 0);
  }
  json::value structuredClone(const json::value& value) { return value; }

  bool isArrayIndex(std::string_view key) noexcept {
    if (key.empty() || key.size() > 10) return false;
    if (key[0] == '0') return key.size() == 1;
    std::uint64_t value = 0;
    for (const char c : key) {
      if (c < '0' || c > '9') return false;
      value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value <= 4294967294u;
  }

  const json::value* get(const json::object& object, std::string_view key) noexcept {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->value();
  }

  json::value* get(json::object& object, std::string_view key) noexcept {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->value();
  }

  const json::value* get(const json::value& value, std::string_view key) noexcept { return value.is_object() ? get(value.get_object(), key) : nullptr; }
  const json::value* get(const json::value* value, std::string_view key) noexcept { return value ? get(*value, key) : nullptr; }
  const json::value* get(const json::array& array, std::size_t index) noexcept { return index < array.size() ? &array[index] : nullptr; }
  bool hasOwn(const json::object& object, std::string_view key) noexcept { return object.find(key) != object.end(); }

  std::optional<std::string_view> getString(const json::value& value) noexcept {
    if (!value.is_string()) return std::nullopt;
    return view(value.get_string());
  }
  std::optional<double> getNumber(const json::value& value) noexcept {
    if (!isNumber(value)) return std::nullopt;
    return numberOf(value);
  }
  std::optional<bool> getBool(const json::value& value) noexcept {
    if (!value.is_bool()) return std::nullopt;
    return value.get_bool();
  }
  const json::object* getObject(const json::value& value) noexcept { return value.if_object(); }
  const json::array* getArray(const json::value& value) noexcept { return value.if_array(); }

  std::optional<std::string_view> getString(const json::value* value) noexcept { return value ? getString(*value) : std::nullopt; }
  std::optional<double> getNumber(const json::value* value) noexcept { return value ? getNumber(*value) : std::nullopt; }
  std::optional<bool> getBool(const json::value* value) noexcept { return value ? getBool(*value) : std::nullopt; }
  const json::object* getObject(const json::value* value) noexcept { return value ? value->if_object() : nullptr; }
  const json::array* getArray(const json::value* value) noexcept { return value ? value->if_array() : nullptr; }

  std::optional<std::string_view> getString(const json::object& object, std::string_view key) noexcept { return getString(get(object, key)); }
  std::optional<double> getNumber(const json::object& object, std::string_view key) noexcept { return getNumber(get(object, key)); }
  std::optional<bool> getBool(const json::object& object, std::string_view key) noexcept { return getBool(get(object, key)); }
  const json::object* getObject(const json::object& object, std::string_view key) noexcept { return getObject(get(object, key)); }
  const json::array* getArray(const json::object& object, std::string_view key) noexcept { return getArray(get(object, key)); }
  json::object* getObject(json::object& object, std::string_view key) noexcept {
    json::value* member = get(object, key);
    return member ? member->if_object() : nullptr;
  }
  json::array* getArray(json::object& object, std::string_view key) noexcept {
    json::value* member = get(object, key);
    return member ? member->if_array() : nullptr;
  }

  std::optional<std::string_view> getString(const json::value& value, std::string_view key) noexcept { return getString(get(value, key)); }
  std::optional<double> getNumber(const json::value& value, std::string_view key) noexcept { return getNumber(get(value, key)); }
  std::optional<bool> getBool(const json::value& value, std::string_view key) noexcept { return getBool(get(value, key)); }
  const json::object* getObject(const json::value& value, std::string_view key) noexcept { return getObject(get(value, key)); }
  const json::array* getArray(const json::value& value, std::string_view key) noexcept { return getArray(get(value, key)); }

  std::vector<std::string> objectKeys(const json::object& object) {
    std::vector<std::string> keys;
    keys.reserve(object.size());
    for (const auto* member : orderedMembers<const json::key_value_pair>(object)) keys.emplace_back(view(member->key()));
    return keys;
  }

  std::vector<const json::value*> objectValues(const json::object& object) {
    std::vector<const json::value*> values;
    values.reserve(object.size());
    for (const auto* member : orderedMembers<const json::key_value_pair>(object)) values.push_back(&member->value());
    return values;
  }

  std::vector<const json::key_value_pair*> objectEntries(const json::object& object) { return orderedMembers<const json::key_value_pair>(object); }
  std::vector<json::key_value_pair*> objectEntries(json::object& object) { return orderedMembers<json::key_value_pair>(object); }

  void objectSet(json::object& object, std::string_view key, json::value value) {
    if (key == "__proto__" && object.find(key) == object.end()) return;
    defineMember(object, key, std::move(value));
  }

  void objectDefine(json::object& object, std::string_view key, json::value value) { defineMember(object, key, std::move(value)); }

  bool objectDelete(json::object& object, std::string_view key) noexcept { return object.stable_erase(key) != 0; }

  void objectAssign(json::object& target, const json::object& source) {
    if (&target == &source) return;
    // The source may live inside the target, which objectSet can rebuild: copy it first.
    std::vector<std::pair<std::string, json::value>> members;
    members.reserve(source.size());
    for (const auto* member : orderedMembers<const json::key_value_pair>(source)) members.emplace_back(view(member->key()), member->value());
    for (auto& [key, value] : members) objectSet(target, key, std::move(value));
  }

  json::object objectSpread(std::initializer_list<const json::object*> sources) {
    json::object out;
    for (const json::object* source : sources)
      for (const auto* member : orderedMembers<const json::key_value_pair>(*source)) defineMember(out, view(member->key()), member->value());
    return out;
  }

  json::object objectFromEntries(const std::vector<std::pair<std::string, json::value>>& entries) {
    json::object out;
    for (const auto& [key, value] : entries) defineMember(out, key, value);
    return out;
  }

  std::string jsonStringify(const json::value& value) {
    Serializer serializer({}, nullptr);
    serializer.root(value);
    return std::move(serializer.out);
  }

  std::string jsonStringify(const json::value& value, int space) {
    Serializer serializer(std::string(static_cast<std::size_t>(std::clamp(space, 0, 10)), ' '), nullptr);
    serializer.root(value);
    return std::move(serializer.out);
  }

  std::string jsonStringify(const json::value& value, std::string_view gap) {
    Serializer serializer(gapFromString(gap), nullptr);
    serializer.root(value);
    return std::move(serializer.out);
  }

  std::optional<std::string> jsonStringify(const json::value& value, const JsonReplacer& replacer, std::string_view gap) {
    Serializer serializer(gapFromString(gap), replacer ? &replacer : nullptr);
    if (!serializer.root(value)) return std::nullopt;
    return std::move(serializer.out);
  }

  std::string jsonQuote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    appendQuoted(out, text);
    return out;
  }

  json::value jsonParse(std::string_view text) {
#if defined(UOS_GATEWAY_SIMDJSON)
    if (auto value = detail::tryParseSimdJson(text)) return std::move(*value);
#elif BOOST_VERSION == 109200
    if (auto value = tryParseBoost(text)) return std::move(*value);
#endif
    if (isAscii(text)) return Parser<char>(text).parse();
    const std::u16string units = toUtf16(text);
    return Parser<char16_t>(units).parse();
  }

  json::value jsonParseBody(std::string_view text, bool emptyBody) {
    if (emptyBody) throw SyntaxError("Unexpected end of JSON input");
    try {
      return jsonParse(text);
    } catch (const SyntaxError&) {
      throw SyntaxError("Failed to parse JSON");
    }
  }

  std::vector<std::string>& sort(std::vector<std::string>& values) {
    detail::stableSort(values, [](const std::string& a, const std::string& b) { return compare(b, a) < 0; });
    return values;
  }

  std::vector<double>& sort(std::vector<double>& values) {
    std::vector<std::pair<std::string, double>> keyed;
    keyed.reserve(values.size());
    for (const double value : values) keyed.emplace_back(numberToString(value), value);
    sortByKey(keyed);
    for (std::size_t index = 0; index < values.size(); ++index) values[index] = keyed[index].second;
    return values;
  }

  json::array& sort(json::array& values) {
    std::vector<std::pair<std::string, json::value>> keyed;
    keyed.reserve(values.size());
    for (auto& value : values) {
      std::string key = toString(value);
      keyed.emplace_back(std::move(key), std::move(value));
    }
    sortByKey(keyed);
    for (std::size_t index = 0; index < values.size(); ++index) values[index] = std::move(keyed[index].second);
    return values;
  }

  std::vector<std::string> toSorted(std::vector<std::string> values) {
    sort(values);
    return values;
  }

  std::vector<double> toSorted(std::vector<double> values) {
    sort(values);
    return values;
  }

  json::array toSorted(json::array values) {
    sort(values);
    return values;
  }

  bool includes(const json::array& values, const json::value& search) noexcept {
    for (const auto& value : values) {
      if (isNumber(value) && isNumber(search) ? sameValueZero(numberOf(value), numberOf(search)) : strictEquals(value, search)) return true;
    }
    return false;
  }

  std::int64_t indexOf(const json::array& values, const json::value& search) noexcept {
    for (std::size_t index = 0; index < values.size(); ++index)
      if (strictEquals(values[index], search)) return static_cast<std::int64_t>(index);
    return -1;
  }

  const json::value* at(const json::array& values, NumberArg index) noexcept {
    double relative = std::isnan(index.value) ? 0.0 : std::trunc(index.value);
    const double length = static_cast<double>(values.size());
    if (relative < 0) relative += length;
    if (relative < 0 || relative >= length) return nullptr;
    return &values[static_cast<std::size_t>(relative)];
  }

  json::array flat(const json::array& values, double depth) {
    json::array out;
    flatInto(out, values, std::isnan(depth) ? 0.0 : std::trunc(depth), 0);
    return out;
  }
}
