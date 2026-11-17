#pragma once
// JavaScript Date with Bun 1.4's results: a time value in epoch milliseconds (NaN is an
// Invalid Date), Date.parse, Date.UTC, the UTC getters and setters and the string forms.
// Local time is UTC: the service runs with TZ=UTC (docs/rt/text.md), so every local-time
// reading (date-time strings without an offset, toString) uses offset 0.
#include "uos/rt/error.hpp"
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

    // `date.getTime()` / `date.valueOf()` / `+date`.
    double getTime() const noexcept { return time_; }
    double valueOf() const noexcept { return time_; }

    // UTC getters (NaN for an Invalid Date). getUTCMonth is 0-based, getUTCDay 0 = Sunday.
    double getUTCFullYear() const noexcept;
    double getUTCMonth() const noexcept;
    double getUTCDate() const noexcept;
    double getUTCDay() const noexcept;
    double getUTCHours() const noexcept;
    double getUTCMinutes() const noexcept;
    double getUTCSeconds() const noexcept;
    double getUTCMilliseconds() const noexcept;
    // `date.getTimezoneOffset()`: 0 under TZ=UTC (NaN for an Invalid Date).
    double getTimezoneOffset() const noexcept;

    // UTC setters: each returns the new time value. An omitted optional argument keeps
    // the current field, as in JS. On an Invalid Date they return NaN, except
    // setUTCFullYear, which starts from time value 0.
    double setTime(double time) noexcept;
    double setUTCFullYear(double year, std::optional<double> month = std::nullopt, std::optional<double> date = std::nullopt) noexcept;
    double setUTCMonth(double month, std::optional<double> date = std::nullopt) noexcept;
    double setUTCDate(double date) noexcept;
    double setUTCHours(double hours, std::optional<double> minutes = std::nullopt, std::optional<double> seconds = std::nullopt, std::optional<double> ms = std::nullopt) noexcept;
    double setUTCMinutes(double minutes, std::optional<double> seconds = std::nullopt, std::optional<double> ms = std::nullopt) noexcept;
    double setUTCSeconds(double seconds, std::optional<double> ms = std::nullopt) noexcept;
    double setUTCMilliseconds(double ms) noexcept;

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
