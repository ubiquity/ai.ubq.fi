#include "uos/rt/date.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

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

    int readDigits(std::string_view& text, std::size_t width) {
      if (text.size() < width || text.front() < '0' || text.front() > '9') return -1;
      int value = 0;
      const auto end = text.data() + width;
      const auto parsed = std::from_chars(text.data(), end, value);
      if (parsed.ec != std::errc{} || parsed.ptr != end) return -1;
      text.remove_prefix(width);
      return value;
    }

    bool consume(std::string_view& text, char byte) {
      if (!text.starts_with(byte)) return false;
      text.remove_prefix(1);
      return true;
    }

    double utcMilliseconds(int y, int m, int d, int h = 0, int min = 0, int s = 0, int ms = 0, int offset = 0) {
      using namespace std::chrono;
      if (m < 1 || m > 12 || d < 1 || d > 31 || h < 0 || h > 24 ||
          min < 0 || min > 59 || s < 0 || s > 59 || (h == 24 && (min != 0 || s != 0 || ms != 0))) return NaN;
      // The Gregorian calendar repeats every 400 years. Keep expanded ISO years out
      // of chrono::year's narrower range; add the whole cycles to the epoch days.
      const year_month_day date{year{y % 400}, month{static_cast<unsigned>(m)}, day{static_cast<unsigned>(d)}};
      if (!date.ok()) return NaN;
      const days epochDays = sys_days{date}.time_since_epoch() + days{(y / 400) * 146097};
      const auto epoch = epochDays + hours{h} + minutes{min - offset} + seconds{s} + milliseconds{ms};
      return timeClip(static_cast<double>(epoch.count()));
    }

    double parseIso(std::string_view text) {
      const bool negative = consume(text, '-');
      const bool expanded = negative || consume(text, '+');
      const int y = readDigits(text, expanded ? 6 : 4);
      if (y < 0 || (negative && y == 0)) return NaN;
      const int year = negative ? -y : y;

      int month = 1;
      int day = 1;
      bool completeDate = false;
      if (consume(text, '-')) {
        month = readDigits(text, 2);
        if (month < 0) return NaN;
        if (consume(text, '-')) {
          day = readDigits(text, 2);
          completeDate = true;
        }
      }
      if (text.empty()) return utcMilliseconds(year, month, day);
      if (!consume(text, 'T') && !consume(text, 't') && !(completeDate && consume(text, ' '))) return NaN;

      const int hour = readDigits(text, 2);
      if (!consume(text, ':')) return NaN;
      const int minute = readDigits(text, 2);
      int second = 0;
      int ms = 0;
      if (consume(text, ':')) {
        second = readDigits(text, 2);
        if (consume(text, '.')) {
          const auto end = text.find_first_not_of("0123456789");
          auto fraction = text.substr(0, end);
          if (fraction.empty() || (hour == 24 && fraction.find_first_not_of('0') != std::string_view::npos)) return NaN;
          text.remove_prefix(fraction.size());
          const auto width = std::min(fraction.size(), std::size_t{3});
          constexpr int scale[] = {0, 100, 10, 1};
          ms = readDigits(fraction, width) * scale[width];
        }
      }
      int offset = 0;
      if (!text.empty() && text != "Z" && text != "z") {
        const bool west = consume(text, '-');
        if (!west && !consume(text, '+')) return NaN;
        const int hours = readDigits(text, 2);
        consume(text, ':');
        const int minutes = readDigits(text, 2);
        if (!text.empty() || hours < 0 || hours > 23 || minutes < 0 || minutes > 59) return NaN;
        offset = (west ? -1 : 1) * (hours * 60 + minutes);
      }
      return utcMilliseconds(year, month, day, hour, minute, second, ms, offset);
    }

    double parseHttpDate(std::string_view text) {
      if (text.size() != 29) return NaN;
      std::istringstream input{std::string(text)};
      input.imbue(std::locale::classic());
      std::tm fields{};
      input >> std::get_time(&fields, "%a, %d %b %Y %H:%M:%S GMT");
      if (input.fail() || input.peek() != std::char_traits<char>::eof()) return NaN;
      const double time = utcMilliseconds(fields.tm_year + 1900, fields.tm_mon + 1, fields.tm_mday,
                                          fields.tm_hour, fields.tm_min, fields.tm_sec);
      // Require the exact IMF-fixdate spelling, including the corresponding weekday.
      return Date(time).toUTCString() == text ? time : NaN;
    }
  }

  Date::Date(double timeValue) noexcept : time_(timeClip(timeValue)) {}

  Date::Date(std::string_view text) : time_(parse(text)) {}

  double Date::parse(std::string_view text) {
    if (text.starts_with('+') || text.starts_with('-') || (!text.empty() && text.front() >= '0' && text.front() <= '9')) return parseIso(text);
    return parseHttpDate(text);
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
