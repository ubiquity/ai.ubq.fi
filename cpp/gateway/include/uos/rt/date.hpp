#pragma once
// A UTC time value in epoch milliseconds, with NaN representing an invalid date.
// Date-time strings without an offset and the local string form use UTC.
#include "uos/rt/error.hpp"
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace uos::rt {
  class Date {
  public:
    // `new Date(value)` for a number: TimeClip (NaN, +-Infinity and |value| > 8.64e15 give
    // an Invalid Date; otherwise truncation toward zero). `new Date()` is
    // `Date(rt::dateNow())` (uos/rt/clock.hpp); `new Date(null)` is `Date(0)`.
    explicit Date(double timeValue) noexcept;
    // `new Date(string)`: Date.parse(text).
    explicit Date(std::string_view text);

    // `Date.parse(text)`: the ECMAScript date-time string format first, then Bun's
    // fallback parser for other forms (RFC 7231/2822 dates, `Jan 2 2020 10:00 PM`, ...).
    // NaN when unparseable or out of range.
    static double parse(std::string_view text);
    // `Date.UTC(year, month, date, hours, minutes, seconds, ms)`; a year 0..99 means
    // 1900..1999. Every argument is truncated toward zero first.
    static double UTC(double year, double month = 0, double date = 1, double hours = 0, double minutes = 0, double seconds = 0, double ms = 0) noexcept;

    double getTime() const noexcept { return time_; }
    // Calendar getters return NaN for an invalid date. getUTCMonth is zero-based.
    double getUTCFullYear() const noexcept;
    double getUTCMonth() const noexcept;
    double getUTCDate() const noexcept;

    struct UtcFields {
      int year;  // TimeClip spans more than chrono::year.
      std::chrono::month month;
      std::chrono::day day;
      std::chrono::weekday weekday;
      std::chrono::hh_mm_ss<std::chrono::milliseconds> clock;
    };
    std::optional<UtcFields> utcFields() const noexcept;

    // Truncate and normalize the UTC components, using a zero-based month and a literal year.
    static Date fromUtc(double year, double month, double day, double hour = 0, double minute = 0, double second = 0, double ms = 0) noexcept;

    // `date.toISOString()`: YYYY-MM-DDTHH:mm:ss.sssZ, with a signed six-digit year outside
    // 0..9999. RangeError "Invalid Date" for an Invalid Date.
    std::string toISOString() const;
    // `date.toJSON()`: toISOString, or null (nullopt) for an Invalid Date.
    std::optional<std::string> toJSON() const;
    // `date.toUTCString()`: "Thu, 01 Jan 1970 00:00:00 GMT"; "Invalid Date" when invalid.
    std::string toUTCString() const;
    // `date.toString()` / `String(date)` / `${date}` under TZ=UTC:
    // "Thu Jan 01 1970 00:00:00 GMT+0000 (Coordinated Universal Time)"; "Invalid Date".
    std::string toString() const;

  private:
    double time_;
  };
}
