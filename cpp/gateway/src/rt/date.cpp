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
    using namespace std::chrono;
    using WideDays = duration<std::int64_t, days::period>;
    using WideMilliseconds = duration<std::int64_t, milliseconds::period>;
    using DoubleDays = duration<double, days::period>;
    using DoubleMilliseconds = duration<double, milliseconds::period>;
    constexpr int DAYS_PER_CYCLE = 146097;
    constexpr int YEARS_PER_CYCLE = 400;
    constexpr auto FIRST_CALENDAR_DAY = sys_days{year::min() / January / 1}.time_since_epoch().count();
    constexpr auto LAST_CALENDAR_DAY = sys_days{year::max() / December / 31}.time_since_epoch().count();
    constexpr double MAX_TIME = 8.64e15;
    constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

    double timeClip(double time) {
      if (!std::isfinite(time) || std::fabs(time) > MAX_TIME) return NaN;
      return std::trunc(time) + 0.0;
    }

    DoubleMilliseconds makeTime(double hour, double minute, double second, double ms) {
      const DoubleMilliseconds hoursMs{duration<double, hours::period>{std::trunc(hour)}};
      const DoubleMilliseconds minutesMs{duration<double, minutes::period>{std::trunc(minute)}};
      const DoubleMilliseconds secondsMs{duration<double, seconds::period>{std::trunc(second)}};
      return hoursMs + minutesMs + secondsMs + DoubleMilliseconds{std::trunc(ms)};
    }

    DoubleDays makeDay(double fullYear, double monthIndex, double dayOfMonth) {
      const double wholeMonth = std::trunc(monthIndex);
      // Combine the year and month before dividing so large opposing inputs can cancel exactly.
      const double normalizedYear = std::floor(std::fma(std::trunc(fullYear), 12, wholeMonth) / 12);
      if (!(std::fabs(normalizedYear) < 1e13)) return DoubleDays{NaN};
      const double remainder = std::fmod(wholeMonth, 12);
      const month normalizedMonth{static_cast<unsigned>(remainder < 0 ? remainder + 12 : remainder) + 1};
      const bool extended = normalizedYear < static_cast<int>(year::min()) || normalizedYear > static_cast<int>(year::max());
      const double cycles = extended ? std::trunc(normalizedYear / YEARS_PER_CYCLE) : 0;
      const auto reducedYear = static_cast<int>(normalizedYear - cycles * YEARS_PER_CYCLE);
      const auto start = sys_days{year{reducedYear} / normalizedMonth / 1};
      // Keep the day adjustment wide through TimeClip, including offsets outside chrono's calendar.
      return DoubleDays{static_cast<double>(start.time_since_epoch().count()) + cycles * DAYS_PER_CYCLE + std::trunc(dayOfMonth) - 1};
    }

    // The callers supply a valid, clipped time value.
    Date::UtcFields fieldsOf(double time) {
      const sys_time<WideMilliseconds> timestamp{WideMilliseconds{static_cast<std::int64_t>(time)}};
      const auto midnight = floor<WideDays>(timestamp);
      const auto dayCount = midnight.time_since_epoch().count();
      // Each Gregorian cycle preserves the weekday as well as the month and day.
      const auto cycles = dayCount < FIRST_CALENDAR_DAY || dayCount > LAST_CALENDAR_DAY ? dayCount / DAYS_PER_CYCLE : 0;
      const sys_days reducedDay{days{static_cast<days::rep>(dayCount - cycles * DAYS_PER_CYCLE)}};
      const year_month_day date{reducedDay};
      return {static_cast<int>(date.year()) + static_cast<int>(cycles) * YEARS_PER_CYCLE,
              date.month(), date.day(), weekday{reducedDay}, hh_mm_ss{duration_cast<milliseconds>(timestamp - midnight)}};
    }

    constexpr std::string_view WEEK_DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    constexpr std::string_view MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    void appendPadded(std::string& out, std::int64_t value, std::size_t width) {
      const auto digits = std::to_string(value);
      if (digits.size() < width) out.append(width - digits.size(), '0');
      out += digits;
    }

    void appendYear(std::string& out, int year, bool expanded = false) {
      if (year < 0 || expanded) out.push_back(year < 0 ? '-' : '+');
      appendPadded(out, std::abs(year), expanded ? 6 : 4);
    }

    void appendClock(std::string& out, const hh_mm_ss<milliseconds>& clock) {
      appendPadded(out, clock.hours().count(), 2);
      out.push_back(':');
      appendPadded(out, clock.minutes().count(), 2);
      out.push_back(':');
      appendPadded(out, clock.seconds().count(), 2);
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
    const double wholeYear = std::trunc(year);
    const double fullYear = wholeYear >= 0 && wholeYear <= 99 ? 1900 + wholeYear : wholeYear;
    return fromUtc(fullYear, month, date, hours, minutes, seconds, ms).getTime();
  }

  Date Date::fromUtc(double year, double month, double day, double hour, double minute, double second, double ms) noexcept {
    return Date((makeDay(year, month, day) + makeTime(hour, minute, second, ms)).count());
  }

  std::optional<Date::UtcFields> Date::utcFields() const noexcept {
    if (std::isnan(time_)) return std::nullopt;
    return fieldsOf(time_);
  }

  double Date::getUTCFullYear() const noexcept { return std::isnan(time_) ? NaN : fieldsOf(time_).year; }
  double Date::getUTCMonth() const noexcept { return std::isnan(time_) ? NaN : static_cast<unsigned>(fieldsOf(time_).month) - 1; }
  double Date::getUTCDate() const noexcept { return std::isnan(time_) ? NaN : static_cast<unsigned>(fieldsOf(time_).day); }

  std::string Date::toISOString() const {
    if (std::isnan(time_)) throw RangeError("Invalid Date");
    const UtcFields fields = fieldsOf(time_);
    std::string out;
    out.reserve(27);
    appendYear(out, fields.year, fields.year < 0 || fields.year > 9999);
    out.push_back('-');
    appendPadded(out, static_cast<unsigned>(fields.month), 2);
    out.push_back('-');
    appendPadded(out, static_cast<unsigned>(fields.day), 2);
    out.push_back('T');
    appendClock(out, fields.clock);
    out.push_back('.');
    appendPadded(out, fields.clock.subseconds().count(), 3);
    out.push_back('Z');
    return out;
  }

  std::optional<std::string> Date::toJSON() const {
    if (std::isnan(time_)) return std::nullopt;
    return toISOString();
  }

  std::string Date::toUTCString() const {
    if (std::isnan(time_)) return "Invalid Date";
    const UtcFields fields = fieldsOf(time_);
    std::string out{WEEK_DAYS[fields.weekday.c_encoding()]};
    out += ", ";
    appendPadded(out, static_cast<unsigned>(fields.day), 2);
    out.push_back(' ');
    out += MONTHS[static_cast<unsigned>(fields.month) - 1];
    out.push_back(' ');
    appendYear(out, fields.year);
    out.push_back(' ');
    appendClock(out, fields.clock);
    out += " GMT";
    return out;
  }

  std::string Date::toString() const {
    if (std::isnan(time_)) return "Invalid Date";
    const UtcFields fields = fieldsOf(time_);
    std::string out{WEEK_DAYS[fields.weekday.c_encoding()]};
    out.push_back(' ');
    out += MONTHS[static_cast<unsigned>(fields.month) - 1];
    out.push_back(' ');
    appendPadded(out, static_cast<unsigned>(fields.day), 2);
    out.push_back(' ');
    appendYear(out, fields.year);
    out.push_back(' ');
    appendClock(out, fields.clock);
    out += " GMT+0000 (Coordinated Universal Time)";
    return out;
  }
}
