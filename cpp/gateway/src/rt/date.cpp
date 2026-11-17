#include "uos/rt/date.hpp"

#include "uos/rt/string.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace uos::rt {
  namespace {
    constexpr double MS_PER_DAY = 86400000.0;
    constexpr double MAX_TIME = 8.64e15;
    constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

    // ---- Time value arithmetic (ECMA-262 21.4.1) -----------------------------------------
    double toIntegerOrInfinity(double value) {
      if (std::isnan(value)) return 0;
      return std::trunc(value) + 0.0;
    }

    double timeClip(double time) {
      if (!std::isfinite(time) || std::fabs(time) > MAX_TIME) return NaN;
      return std::trunc(time) + 0.0;
    }

    // Days from 1970-01-01 to January 1 of `year`; exact for |year| below 1e13.
    double dayFromYear(double year) { return 365.0 * (year - 1970) + std::floor((year - 1969) / 4) - std::floor((year - 1901) / 100) + std::floor((year - 1601) / 400); }
    bool inLeapYear(double year) { return std::fmod(year, 4) == 0 && (std::fmod(year, 100) != 0 || std::fmod(year, 400) == 0); }
    constexpr int DAYS_BEFORE_MONTH[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

    double makeTime(double hour, double minute, double second, double ms) {
      if (!std::isfinite(hour) || !std::isfinite(minute) || !std::isfinite(second) || !std::isfinite(ms)) return NaN;
      return toIntegerOrInfinity(hour) * 3600000.0 + toIntegerOrInfinity(minute) * 60000.0 + toIntegerOrInfinity(second) * 1000.0 + toIntegerOrInfinity(ms);
    }

    double makeDay(double year, double month, double date) {
      if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date)) return NaN;
      const double y = toIntegerOrInfinity(year);
      const double m = toIntegerOrInfinity(month);
      const double dt = toIntegerOrInfinity(date);
      const double ym = y + std::floor(m / 12);
      // Beyond this the year start is not exactly representable; every such date is far
      // outside the time value range.
      if (!(std::fabs(ym) < 1e13)) return NaN;
      const int mn = static_cast<int>(m - std::floor(m / 12) * 12);
      const double start = dayFromYear(ym) + DAYS_BEFORE_MONTH[mn] + (mn >= 2 && inLeapYear(ym) ? 1 : 0);
      return start + dt - 1;
    }

    double makeDate(double day, double time) {
      if (!std::isfinite(day) || !std::isfinite(time)) return NaN;
      const double value = day * MS_PER_DAY + time;
      return std::isfinite(value) ? value : NaN;
    }

    // The calendar fields of a valid time value.
    struct Fields {
      std::int64_t year;
      int month;  // 0-based
      int day;    // 1-based
      int weekDay;
      int hour;
      int minute;
      int second;
      int ms;
      std::int64_t dayNumber;  // Day(t)
      double timeInDay;        // TimeWithinDay(t)
    };
    Fields fieldsOf(double time) {
      const auto total = static_cast<std::int64_t>(time);
      std::int64_t days = total / 86400000;
      std::int64_t rest = total % 86400000;
      if (rest < 0) {
        rest += 86400000;
        --days;
      }
      // Civil date from a day count (proleptic Gregorian).
      const std::int64_t shifted = days + 719468;
      const std::int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
      const std::int64_t dayOfEra = shifted - era * 146097;
      const std::int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
      const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
      const std::int64_t shiftedMonth = (5 * dayOfYear + 2) / 153;
      Fields fields{};
      fields.day = static_cast<int>(dayOfYear - (153 * shiftedMonth + 2) / 5 + 1);
      fields.month = static_cast<int>(shiftedMonth < 10 ? shiftedMonth + 2 : shiftedMonth - 10);
      fields.year = yearOfEra + era * 400 + (fields.month <= 1 ? 1 : 0);
      fields.weekDay = static_cast<int>(((days % 7) + 11) % 7);
      fields.hour = static_cast<int>(rest / 3600000);
      fields.minute = static_cast<int>(rest / 60000 % 60);
      fields.second = static_cast<int>(rest / 1000 % 60);
      fields.ms = static_cast<int>(rest % 1000);
      fields.dayNumber = days;
      fields.timeInDay = static_cast<double>(rest);
      return fields;
    }

    // ---- Formatting -------------------------------------------------------------------------
    constexpr const char* WEEK_DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    constexpr const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    void appendPadded(std::string& out, std::int64_t value, int width) {
      const std::string digits = std::to_string(value);
      for (int index = static_cast<int>(digits.size()); index < width; ++index) out.push_back('0');
      out += digits;
    }
    // Year as toUTCString/toString print it: a sign for negative years, four digits at least.
    void appendDisplayYear(std::string& out, std::int64_t year) {
      if (year < 0) out.push_back('-');
      appendPadded(out, year < 0 ? -year : year, 4);
    }
    void appendClock(std::string& out, const Fields& fields) {
      appendPadded(out, fields.hour, 2);
      out.push_back(':');
      appendPadded(out, fields.minute, 2);
      out.push_back(':');
      appendPadded(out, fields.second, 2);
    }

    // ---- Date.parse ---------------------------------------------------------------------------
    // Bun parses date strings the way V8's DateParser does: the ES date-time format first,
    // then a legacy token loop that accepts loose forms. It reads UTF-16 code units, stops
    // at the first U+0000, and treats only the characters below as white space.
    constexpr int NONE = std::numeric_limits<int>::max();
    constexpr int MAX_SIGNIFICANT_DIGITS = 9;
    constexpr std::uint32_t MAX_OFFSET_SECONDS = 2147483647u;

    // VT (U+000B) is white space only in a string JSC stores as UTF-16, which the port
    // decides by content: one that has a code unit above U+00FF (`wide`).
    bool isDateWhiteSpace(char16_t unit, bool wide) {
      return unit == 0x09 || unit == 0x0A || unit == 0x0C || unit == 0x0D || unit == 0x20 || unit == 0xA0 || unit == 0x1680 || (unit >= 0x2000 && unit <= 0x200A) || unit == 0x202F || unit == 0x205F || unit == 0x3000 || unit == 0xFEFF || (wide && unit == 0x0B);
    }
    bool between(int value, int low, int high) { return value >= low && value <= high; }
    bool isMonth(int value) { return between(value, 1, 12); }
    bool isDay(int value) { return between(value, 1, 31); }
    bool isHour(int value) { return between(value, 0, 23); }
    bool isMinute(int value) { return between(value, 0, 59); }
    bool isSecond(int value) { return between(value, 0, 59); }
    bool isHour12(int value) { return between(value, 0, 12); }
    bool isMillisecond(int value) { return between(value, 0, 999); }

    enum class KeywordType { Invalid, MonthName, TimeZoneName, TimeSeparator, AmPm };
    struct Keyword {
      char prefix[3];
      KeywordType type;
      int value;
    };
    constexpr Keyword KEYWORDS[] = {
      {{'j', 'a', 'n'}, KeywordType::MonthName, 1}, {{'f', 'e', 'b'}, KeywordType::MonthName, 2}, {{'m', 'a', 'r'}, KeywordType::MonthName, 3},
      {{'a', 'p', 'r'}, KeywordType::MonthName, 4}, {{'m', 'a', 'y'}, KeywordType::MonthName, 5}, {{'j', 'u', 'n'}, KeywordType::MonthName, 6},
      {{'j', 'u', 'l'}, KeywordType::MonthName, 7}, {{'a', 'u', 'g'}, KeywordType::MonthName, 8}, {{'s', 'e', 'p'}, KeywordType::MonthName, 9},
      {{'o', 'c', 't'}, KeywordType::MonthName, 10}, {{'n', 'o', 'v'}, KeywordType::MonthName, 11}, {{'d', 'e', 'c'}, KeywordType::MonthName, 12},
      {{'a', 'm', '\0'}, KeywordType::AmPm, 0}, {{'p', 'm', '\0'}, KeywordType::AmPm, 12},
      {{'u', 't', '\0'}, KeywordType::TimeZoneName, 0}, {{'u', 't', 'c'}, KeywordType::TimeZoneName, 0}, {{'z', '\0', '\0'}, KeywordType::TimeZoneName, 0},
      {{'g', 'm', 't'}, KeywordType::TimeZoneName, 0}, {{'c', 'd', 't'}, KeywordType::TimeZoneName, -5}, {{'c', 's', 't'}, KeywordType::TimeZoneName, -6},
      {{'e', 'd', 't'}, KeywordType::TimeZoneName, -4}, {{'e', 's', 't'}, KeywordType::TimeZoneName, -5}, {{'m', 'd', 't'}, KeywordType::TimeZoneName, -6},
      {{'m', 's', 't'}, KeywordType::TimeZoneName, -7}, {{'p', 'd', 't'}, KeywordType::TimeZoneName, -7}, {{'p', 's', 't'}, KeywordType::TimeZoneName, -8},
      {{'t', '\0', '\0'}, KeywordType::TimeSeparator, 0},
    };
    // A word longer than three characters only matches a month name.
    const Keyword* lookupKeyword(const std::uint32_t (&prefix)[3], int length) {
      for (const Keyword& keyword : KEYWORDS) {
        int matched = 0;
        while (matched < 3 && prefix[matched] == static_cast<std::uint32_t>(static_cast<unsigned char>(keyword.prefix[matched]))) ++matched;
        if (matched == 3 && (length <= 3 || keyword.type == KeywordType::MonthName)) return &keyword;
      }
      return nullptr;
    }

    class Reader {
    public:
      explicit Reader(std::u16string_view text) : text_(text) {
        for (const char16_t unit : text) {
          if (unit > 0xFF) {
            wide_ = true;
            break;
          }
        }
        next();
      }
      void next() {
        ch_ = index_ < text_.size() ? text_[index_] : 0;
        ++index_;
      }
      std::size_t position() const { return index_; }
      bool isEnd() const { return ch_ == 0; }
      bool isAsciiDigit() const { return ch_ >= '0' && ch_ <= '9'; }
      bool isAsciiAlphaOrAbove() const { return ch_ >= 'A'; }
      bool isWhiteSpaceChar() const { return isDateWhiteSpace(ch_, wide_); }
      bool skip(char16_t unit) {
        if (ch_ != unit) return false;
        next();
        return true;
      }
      // Digits as an unsigned number: leading zeros skipped, at most nine significant
      // digits kept, the rest consumed.
      int readUnsignedNumeral() {
        int value = 0;
        int count = 0;
        while (ch_ == '0') next();
        while (isAsciiDigit()) {
          if (count < MAX_SIGNIFICANT_DIGITS) value = value * 10 + (ch_ - '0');
          ++count;
          next();
        }
        return value;
      }
      // A word (characters >= 'A' that are not white space): its first three characters
      // with bit 0x20 set, zero padded; returns the word length.
      int readWord(std::uint32_t (&prefix)[3]) {
        int length = 0;
        for (; isAsciiAlphaOrAbove() && !isWhiteSpaceChar(); next(), ++length) {
          if (length < 3) prefix[length] = static_cast<std::uint32_t>(ch_) | 0x20;
        }
        for (int index = length; index < 3; ++index) prefix[index] = 0;
        return length;
      }
      bool skipWhiteSpace() {
        if (!isDateWhiteSpace(ch_, wide_)) return false;
        next();
        return true;
      }
      bool skipParentheses() {
        if (ch_ != '(') return false;
        int balance = 0;
        do {
          if (ch_ == ')') --balance;
          else if (ch_ == '(') ++balance;
          next();
        } while (balance > 0 && ch_);
        return true;
      }

    private:
      std::u16string_view text_;
      std::size_t index_ = 0;
      char16_t ch_ = 0;
      bool wide_ = false;
    };

    struct Token {
      enum class Tag { Invalid, Unknown, WhiteSpace, Number, Symbol, EndOfInput, Keyword };
      Tag tag = Tag::Invalid;
      int length = 0;
      int value = 0;
      KeywordType keyword = KeywordType::Invalid;

      bool isInvalid() const { return tag == Tag::Invalid; }
      bool isNumber() const { return tag == Tag::Number; }
      bool isWhiteSpace() const { return tag == Tag::WhiteSpace; }
      bool isEndOfInput() const { return tag == Tag::EndOfInput; }
      bool isKeyword() const { return tag == Tag::Keyword; }
      bool isSymbol(char symbol) const { return tag == Tag::Symbol && value == symbol; }
      bool isAsciiSign() const { return tag == Tag::Symbol && (value == '-' || value == '+'); }
      int asciiSign() const { return 44 - value; }
      bool isKeywordType(KeywordType type) const { return tag == Tag::Keyword && keyword == type; }
      bool isKeywordZ() const { return tag == Tag::Keyword && keyword == KeywordType::TimeZoneName && length == 1 && value == 0; }
      bool isFixedLengthNumber(int digits) const { return tag == Tag::Number && length == digits; }

      static Token make(Tag tag, int length, int value) { return {tag, length, value, KeywordType::Invalid}; }
    };

    class Tokenizer {
    public:
      explicit Tokenizer(Reader& reader) : reader_(reader), next_(scan()) {}
      Token next() {
        const Token result = next_;
        next_ = scan();
        return result;
      }
      const Token& peek() const { return next_; }
      bool skipSymbol(char symbol) {
        if (!next_.isSymbol(symbol)) return false;
        next_ = scan();
        return true;
      }

    private:
      Token scan() {
        const std::size_t start = reader_.position();
        if (reader_.isEnd()) return Token::make(Token::Tag::EndOfInput, 0, 0);
        if (reader_.isAsciiDigit()) {
          const int value = reader_.readUnsignedNumeral();
          return Token::make(Token::Tag::Number, static_cast<int>(reader_.position() - start), value);
        }
        for (const char symbol : {':', '-', '+', '.', ')'}) {
          if (reader_.skip(static_cast<char16_t>(symbol))) return Token::make(Token::Tag::Symbol, 1, symbol);
        }
        if (reader_.isAsciiAlphaOrAbove() && !reader_.isWhiteSpaceChar()) {
          std::uint32_t prefix[3];
          const int length = reader_.readWord(prefix);
          const Keyword* keyword = lookupKeyword(prefix, length);
          return {Token::Tag::Keyword, length, keyword ? keyword->value : 0, keyword ? keyword->type : KeywordType::Invalid};
        }
        if (reader_.skipWhiteSpace()) return Token::make(Token::Tag::WhiteSpace, static_cast<int>(reader_.position() - start), 0);
        if (reader_.skipParentheses()) return Token::make(Token::Tag::Unknown, 1, -1);
        reader_.next();
        return Token::make(Token::Tag::Unknown, 1, -1);
      }

      Reader& reader_;
      Token next_;
    };

    struct Output {
      double year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, ms = 0;
      double utcOffsetSeconds = NaN;  // NaN: local time
    };

    class DayComposer {
    public:
      bool isEmpty() const { return index_ == 0; }
      bool add(int value) {
        if (index_ >= 3) return false;
        components_[index_++] = value;
        return true;
      }
      void setNamedMonth(int value) { namedMonth_ = value; }
      void setIsoDate() { isoDate_ = true; }
      bool write(Output& out) {
        if (index_ < 1) return false;
        while (index_ < 3) components_[index_++] = 1;
        int year = 0;
        int month = NONE;
        int day = NONE;
        if (namedMonth_ == NONE) {
          if (isoDate_ || !isDay(components_[0])) {
            year = components_[0];
            month = components_[1];
            day = components_[2];
          } else {
            month = components_[0];
            day = components_[1];
            year = components_[2];
          }
        } else {
          month = namedMonth_;
          if (!isDay(components_[0])) {
            year = components_[0];
            day = components_[1];
          } else {
            day = components_[0];
            year = components_[1];
          }
        }
        if (!isoDate_) {
          if (between(year, 0, 49)) year += 2000;
          else if (between(year, 50, 99)) year += 1900;
        }
        if (!isMonth(month) || !isDay(day)) return false;
        out.year = year;
        out.month = month - 1;
        out.day = day;
        return true;
      }

    private:
      int components_[3] = {0, 0, 0};
      int index_ = 0;
      int namedMonth_ = NONE;
      bool isoDate_ = false;
    };

    class TimeComposer {
    public:
      bool isEmpty() const { return index_ == 0; }
      bool isExpecting(int value) const { return (index_ == 1 && isMinute(value)) || (index_ == 2 && isSecond(value)) || (index_ == 3 && isMillisecond(value)); }
      bool add(int value) {
        if (index_ >= 4) return false;
        components_[index_++] = value;
        return true;
      }
      bool addFinal(int value) {
        if (!add(value)) return false;
        while (index_ < 4) components_[index_++] = 0;
        return true;
      }
      void setHourOffset(int value) { hourOffset_ = value; }
      bool write(Output& out) {
        while (index_ < 4) components_[index_++] = 0;
        int hour = components_[0];
        const int minute = components_[1];
        const int second = components_[2];
        const int ms = components_[3];
        if (hourOffset_ != NONE) {
          if (!isHour12(hour)) return false;
          hour %= 12;
          hour += hourOffset_;
        }
        if (!isHour(hour) || !isMinute(minute) || !isSecond(second) || !isMillisecond(ms)) {
          // Hour 24 is allowed when everything after it is zero.
          if (hour != 24 || minute != 0 || second != 0 || ms != 0) return false;
        }
        out.hour = hour;
        out.minute = minute;
        out.second = second;
        out.ms = ms;
        return true;
      }

    private:
      int components_[4] = {0, 0, 0, 0};
      int index_ = 0;
      int hourOffset_ = NONE;
    };

    class TimeZoneComposer {
    public:
      void set(int offsetInHours) {
        sign_ = offsetInHours < 0 ? -1 : 1;
        hour_ = offsetInHours * sign_;
        minute_ = 0;
      }
      void setSign(int sign) { sign_ = sign < 0 ? -1 : 1; }
      void setAbsoluteHour(int hour) { hour_ = hour; }
      void setAbsoluteMinute(int minute) { minute_ = minute; }
      bool isExpecting(int value) const { return hour_ != NONE && minute_ == NONE && isMinute(value); }
      bool isUtc() const { return hour_ == 0 && minute_ == 0; }
      bool isEmpty() const { return hour_ == NONE; }
      bool write(Output& out) {
        if (sign_ == NONE) {
          out.utcOffsetSeconds = NaN;
          return true;
        }
        if (hour_ == NONE) hour_ = 0;
        if (minute_ == NONE) minute_ = 0;
        // Unsigned 32-bit arithmetic and a 2^31 - 1 bound, as Bun.
        const std::uint32_t total = static_cast<std::uint32_t>(hour_) * 3600u + static_cast<std::uint32_t>(minute_) * 60u;
        if (total > MAX_OFFSET_SECONDS) return false;
        const int seconds = static_cast<int>(total);
        out.utcOffsetSeconds = sign_ < 0 ? -seconds : seconds;
        return true;
      }

    private:
      int sign_ = NONE;
      int hour_ = NONE;
      int minute_ = NONE;
    };

    // Fraction digits as milliseconds: the first three significant digits of the numeral
    // as read (so at most nine digits count, leading zeros included).
    int readMilliseconds(const Token& token) {
      int number = token.value;
      int length = token.length;
      if (length < 3) {
        if (length == 1) number *= 100;
        else if (length == 2) number *= 10;
      } else if (length > 3) {
        if (length > MAX_SIGNIFICANT_DIGITS) length = MAX_SIGNIFICANT_DIGITS;
        int factor = 1;
        do {
          factor *= 10;
          --length;
        } while (length > 3);
        number /= factor;
      }
      return number;
    }

    // [('-'|'+')yy]yyyy[-MM[-DD]][THH:mm[:ss[.sss]][Z|(+|-)hh:mm]] with V8's extensions;
    // returns the first token it did not consume (for the legacy loop), EndOfInput on a
    // complete ES date-time string, or Invalid.
    Token parseEsDateTime(Tokenizer& scanner, DayComposer& day, TimeComposer& time, TimeZoneComposer& zone) {
      if (scanner.peek().isAsciiSign()) {
        const Token sign = scanner.next();
        if (!scanner.peek().isFixedLengthNumber(6)) return sign;
        const int signValue = sign.asciiSign();
        const int year = scanner.next().value;
        if (signValue < 0 && year == 0) return sign;
        day.add(signValue * year);
      } else if (scanner.peek().isFixedLengthNumber(4)) {
        day.add(scanner.next().value);
      } else {
        return scanner.next();
      }
      if (scanner.skipSymbol('-')) {
        if (!scanner.peek().isFixedLengthNumber(2) || !isMonth(scanner.peek().value)) return scanner.next();
        day.add(scanner.next().value);
        if (scanner.skipSymbol('-')) {
          if (!scanner.peek().isFixedLengthNumber(2) || !isDay(scanner.peek().value)) return scanner.next();
          day.add(scanner.next().value);
        }
      }
      if (!scanner.peek().isKeywordType(KeywordType::TimeSeparator)) {
        if (!scanner.peek().isEndOfInput()) return scanner.next();
      } else {
        scanner.next();
        if (!scanner.peek().isFixedLengthNumber(2) || !between(scanner.peek().value, 0, 24)) return Token{};
        const bool hourIs24 = scanner.peek().value == 24;
        time.add(scanner.next().value);
        if (!scanner.skipSymbol(':')) return Token{};
        if (!scanner.peek().isFixedLengthNumber(2) || !isMinute(scanner.peek().value) || (hourIs24 && scanner.peek().value > 0)) return Token{};
        time.add(scanner.next().value);
        if (scanner.skipSymbol(':')) {
          if (!scanner.peek().isFixedLengthNumber(2) || !isSecond(scanner.peek().value) || (hourIs24 && scanner.peek().value > 0)) return Token{};
          time.add(scanner.next().value);
          if (scanner.skipSymbol('.')) {
            if (!scanner.peek().isNumber() || (hourIs24 && scanner.peek().value > 0)) return Token{};
            time.add(readMilliseconds(scanner.next()));
          }
        }
        if (scanner.peek().isKeywordZ()) {
          scanner.next();
          zone.set(0);
        } else if (scanner.peek().isSymbol('+') || scanner.peek().isSymbol('-')) {
          zone.setSign(scanner.next().value == '+' ? 1 : -1);
          if (scanner.peek().isFixedLengthNumber(4)) {
            const int hourMinute = scanner.next().value;
            const int hour = hourMinute / 100;
            const int minute = hourMinute % 100;
            if (!isHour(hour) || !isMinute(minute)) return Token{};
            zone.setAbsoluteHour(hour);
            zone.setAbsoluteMinute(minute);
          } else {
            if (!scanner.peek().isFixedLengthNumber(2) || !isHour(scanner.peek().value)) return Token{};
            zone.setAbsoluteHour(scanner.next().value);
            if (!scanner.skipSymbol(':')) return Token{};
            if (!scanner.peek().isFixedLengthNumber(2) || !isMinute(scanner.peek().value)) return Token{};
            zone.setAbsoluteMinute(scanner.next().value);
          }
        }
        if (!scanner.peek().isEndOfInput()) return Token{};
      }
      // Date-only forms without an offset are UTC; date-time forms are local time.
      if (zone.isEmpty() && time.isEmpty()) zone.set(0);
      day.setIsoDate();
      return Token::make(Token::Tag::EndOfInput, 0, 0);
    }

    bool parseFields(std::u16string_view text, Output& out) {
      Reader reader(text);
      Tokenizer scanner(reader);
      DayComposer day;
      TimeComposer time;
      TimeZoneComposer zone;
      const Token first = parseEsDateTime(scanner, day, time, zone);
      if (first.isInvalid()) return false;
      bool hasReadNumber = !day.isEmpty();
      for (Token token = first; !token.isEndOfInput(); token = scanner.next()) {
        if (token.isNumber()) {
          hasReadNumber = true;
          const int value = token.value;
          if (scanner.skipSymbol(':')) {
            if (scanner.skipSymbol(':')) {
              // value + "::"
              if (!time.isEmpty()) return false;
              time.add(value);
              time.add(0);
            } else {
              // value + ":"
              if (!time.add(value)) return false;
              if (scanner.peek().isSymbol('.')) scanner.next();
            }
          } else if (scanner.skipSymbol('.') && time.isExpecting(value)) {
            time.add(value);
            if (!scanner.peek().isNumber()) return false;
            const int ms = readMilliseconds(scanner.next());
            if (ms < 0) return false;
            time.addFinal(ms);
          } else if (zone.isExpecting(value)) {
            zone.setAbsoluteMinute(value);
          } else if (time.isExpecting(value)) {
            time.addFinal(value);
            // End, white space, "Z", "+" or "-" must follow a completed time.
            const Token& peek = scanner.peek();
            if (!peek.isEndOfInput() && !peek.isWhiteSpace() && !peek.isKeywordZ() && !peek.isAsciiSign()) return false;
          } else {
            if (!day.add(value)) return false;
            scanner.skipSymbol('-');
          }
        } else if (token.isKeyword()) {
          if (token.keyword == KeywordType::AmPm && !time.isEmpty()) {
            time.setHourOffset(token.value);
          } else if (token.keyword == KeywordType::MonthName) {
            day.setNamedMonth(token.value);
            scanner.skipSymbol('-');
          } else if (token.keyword == KeywordType::TimeZoneName && hasReadNumber) {
            zone.set(token.value);
          } else {
            // Words are ignored only before the first number, and must not touch it.
            if (hasReadNumber) return false;
            if (scanner.peek().isNumber()) return false;
          }
        } else if (token.isAsciiSign() && (zone.isUtc() || !time.isEmpty())) {
          // A UTC offset, only after a time or UTC.
          zone.setSign(token.asciiSign());
          int value = 0;
          int length = 0;
          if (scanner.peek().isNumber()) {
            const Token number = scanner.next();
            length = number.length;
            value = number.value;
          }
          hasReadNumber = true;
          if (scanner.peek().isSymbol(':')) {
            zone.setAbsoluteHour(value);
            zone.setAbsoluteMinute(NONE);
          } else if (length == 1 || length == 2) {
            zone.setAbsoluteHour(value);
            zone.setAbsoluteMinute(0);
          } else if (length == 3 || length == 4) {
            zone.setAbsoluteHour(value / 100);
            zone.setAbsoluteMinute(value % 100);
          } else {
            return false;
          }
        } else if ((token.isAsciiSign() || token.isSymbol(')')) && hasReadNumber) {
          return false;
        }
      }
      return day.write(out) && time.write(out) && zone.write(out);
    }
  }

  Date::Date(double timeValue) noexcept : time_(timeClip(timeValue)) {}

  Date::Date(std::string_view text) : time_(parse(text)) {}

  double Date::parse(std::string_view text) {
    Output out;
    if (!parseFields(toUtf16(text), out)) return NaN;
    const double date = makeDate(makeDay(out.year, out.month, out.day), makeTime(out.hour, out.minute, out.second, out.ms));
    // Local time is UTC (TZ=UTC), so both forms reduce to the offset subtraction.
    return timeClip(std::isnan(out.utcOffsetSeconds) ? date : date - out.utcOffsetSeconds * 1000.0);
  }

  double Date::UTC(double year, double month, double date, double hours, double minutes, double seconds, double ms) noexcept {
    double fullYear = year;
    if (!std::isnan(year)) {
      const double integer = toIntegerOrInfinity(year);
      if (integer >= 0 && integer <= 99) fullYear = 1900 + integer;
    }
    return timeClip(makeDate(makeDay(fullYear, month, date), makeTime(hours, minutes, seconds, ms)));
  }

  double Date::getUTCFullYear() const noexcept { return std::isnan(time_) ? NaN : static_cast<double>(fieldsOf(time_).year); }
  double Date::getUTCMonth() const noexcept { return std::isnan(time_) ? NaN : fieldsOf(time_).month; }
  double Date::getUTCDate() const noexcept { return std::isnan(time_) ? NaN : fieldsOf(time_).day; }
  double Date::getUTCDay() const noexcept { return std::isnan(time_) ? NaN : fieldsOf(time_).weekDay; }
  double Date::getUTCHours() const noexcept { return std::isnan(time_) ? NaN : fieldsOf(time_).hour; }
  double Date::getUTCMinutes() const noexcept { return std::isnan(time_) ? NaN : fieldsOf(time_).minute; }
  double Date::getUTCSeconds() const noexcept { return std::isnan(time_) ? NaN : fieldsOf(time_).second; }
  double Date::getUTCMilliseconds() const noexcept { return std::isnan(time_) ? NaN : fieldsOf(time_).ms; }
  double Date::getTimezoneOffset() const noexcept { return std::isnan(time_) ? NaN : 0; }

  double Date::setTime(double time) noexcept { return time_ = timeClip(time); }

  double Date::setUTCFullYear(double year, std::optional<double> month, std::optional<double> date) noexcept {
    const Fields fields = fieldsOf(std::isnan(time_) ? 0 : time_);
    const double newDate = makeDate(makeDay(year, month.value_or(fields.month), date.value_or(fields.day)), fields.timeInDay);
    return time_ = timeClip(newDate);
  }

  double Date::setUTCMonth(double month, std::optional<double> date) noexcept {
    if (std::isnan(time_)) return NaN;
    const Fields fields = fieldsOf(time_);
    return time_ = timeClip(makeDate(makeDay(static_cast<double>(fields.year), month, date.value_or(fields.day)), fields.timeInDay));
  }

  double Date::setUTCDate(double date) noexcept {
    if (std::isnan(time_)) return NaN;
    const Fields fields = fieldsOf(time_);
    return time_ = timeClip(makeDate(makeDay(static_cast<double>(fields.year), fields.month, date), fields.timeInDay));
  }

  double Date::setUTCHours(double hours, std::optional<double> minutes, std::optional<double> seconds, std::optional<double> ms) noexcept {
    if (std::isnan(time_)) return NaN;
    const Fields fields = fieldsOf(time_);
    const double time = makeTime(hours, minutes.value_or(fields.minute), seconds.value_or(fields.second), ms.value_or(fields.ms));
    return time_ = timeClip(makeDate(static_cast<double>(fields.dayNumber), time));
  }

  double Date::setUTCMinutes(double minutes, std::optional<double> seconds, std::optional<double> ms) noexcept {
    if (std::isnan(time_)) return NaN;
    const Fields fields = fieldsOf(time_);
    const double time = makeTime(fields.hour, minutes, seconds.value_or(fields.second), ms.value_or(fields.ms));
    return time_ = timeClip(makeDate(static_cast<double>(fields.dayNumber), time));
  }

  double Date::setUTCSeconds(double seconds, std::optional<double> ms) noexcept {
    if (std::isnan(time_)) return NaN;
    const Fields fields = fieldsOf(time_);
    const double time = makeTime(fields.hour, fields.minute, seconds, ms.value_or(fields.ms));
    return time_ = timeClip(makeDate(static_cast<double>(fields.dayNumber), time));
  }

  double Date::setUTCMilliseconds(double ms) noexcept {
    if (std::isnan(time_)) return NaN;
    const Fields fields = fieldsOf(time_);
    const double time = makeTime(fields.hour, fields.minute, fields.second, ms);
    return time_ = timeClip(makeDate(static_cast<double>(fields.dayNumber), time));
  }

  std::string Date::toISOString() const {
    if (std::isnan(time_)) throw RangeError("Invalid Date");
    const Fields fields = fieldsOf(time_);
    std::string out;
    if (fields.year >= 0 && fields.year <= 9999) {
      appendPadded(out, fields.year, 4);
    } else {
      out.push_back(fields.year < 0 ? '-' : '+');
      appendPadded(out, fields.year < 0 ? -fields.year : fields.year, 6);
    }
    out.push_back('-');
    appendPadded(out, fields.month + 1, 2);
    out.push_back('-');
    appendPadded(out, fields.day, 2);
    out.push_back('T');
    appendClock(out, fields);
    out.push_back('.');
    appendPadded(out, fields.ms, 3);
    out.push_back('Z');
    return out;
  }

  std::optional<std::string> Date::toJSON() const {
    if (std::isnan(time_)) return std::nullopt;
    return toISOString();
  }

  std::string Date::toUTCString() const {
    if (std::isnan(time_)) return "Invalid Date";
    const Fields fields = fieldsOf(time_);
    std::string out = WEEK_DAYS[fields.weekDay];
    out += ", ";
    appendPadded(out, fields.day, 2);
    out.push_back(' ');
    out += MONTHS[fields.month];
    out.push_back(' ');
    appendDisplayYear(out, fields.year);
    out.push_back(' ');
    appendClock(out, fields);
    out += " GMT";
    return out;
  }

  std::string Date::toString() const {
    if (std::isnan(time_)) return "Invalid Date";
    const Fields fields = fieldsOf(time_);
    std::string out = WEEK_DAYS[fields.weekDay];
    out.push_back(' ');
    out += MONTHS[fields.month];
    out.push_back(' ');
    appendPadded(out, fields.day, 2);
    out.push_back(' ');
    appendDisplayYear(out, fields.year);
    out.push_back(' ');
    appendClock(out, fields);
    out += " GMT+0000 (Coordinated Universal Time)";
    return out;
  }
}
