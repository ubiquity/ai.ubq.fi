#pragma once
// `T | null | undefined` where the TypeScript tells null and undefined apart
// (docs/rt/values.md). Use std::optional<T> when only one of the two can occur.
#include "uos/rt/json.hpp"
#include "uos/rt/prelude.hpp"

#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace uos::rt {
  struct Undefined {
    friend constexpr bool operator==(Undefined, Undefined) noexcept { return true; }
  };
  // The JS value `undefined`, for Nullable<T> initialisation and comparison.
  inline constexpr Undefined undefined{};

  // JSON conversions of the value types a Nullable usually holds: bool, arithmetic types
  // (canonical numbers; integral targets accept only integral JS numbers in range),
  // std::string, json::value, json::object, json::array; other types go through
  // boost::json::value_from / value_to.
  template <class T>
  json::value toJsonValue(const T& value) {
    if constexpr (std::is_same_v<T, bool>) return json::value(value);
    else if constexpr (std::is_floating_point_v<T>) return makeNumber(static_cast<double>(value));
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) return makeNumber(static_cast<std::int64_t>(value));
    else if constexpr (std::is_integral_v<T>) return makeNumber(static_cast<double>(value));
    else if constexpr (std::is_same_v<T, std::string>) return json::value(json::string(value));
    else if constexpr (std::is_same_v<T, json::value>) return value;
    else if constexpr (std::is_same_v<T, json::object> || std::is_same_v<T, json::array>) return json::value(value);
    else return json::value_from(value);
  }

  template <class T>
  std::optional<T> fromJsonValue(const json::value& value) {
    if constexpr (std::is_same_v<T, bool>) {
      if (value.is_bool()) return value.get_bool();
      return std::nullopt;
    } else if constexpr (std::is_arithmetic_v<T>) {
      const std::optional<double> number = getNumber(value);
      if (!number) return std::nullopt;
      if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(*number);
      } else {
        if (!Number::isInteger(*number) || *number < static_cast<double>(std::numeric_limits<T>::min()) || *number >= std::ldexp(1.0, std::numeric_limits<T>::digits)) return std::nullopt;
        return static_cast<T>(*number);
      }
    } else if constexpr (std::is_same_v<T, std::string>) {
      if (value.is_string()) return std::string(value.get_string());
      return std::nullopt;
    } else if constexpr (std::is_same_v<T, json::value>) {
      return value;
    } else if constexpr (std::is_same_v<T, json::object>) {
      if (value.is_object()) return value.get_object();
      return std::nullopt;
    } else if constexpr (std::is_same_v<T, json::array>) {
      if (value.is_array()) return value.get_array();
      return std::nullopt;
    } else {
      try {
        return json::value_to<T>(value);
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }
  }

  // Declared outside Nullable so that comparing states never considers Nullable's operators.
  enum class NullableState : std::uint8_t { Undefined, Null, Value };

  template <class T>
  class Nullable {
  public:
    using State = NullableState;

    constexpr Nullable() noexcept = default;  // undefined
    constexpr Nullable(Undefined) noexcept {}
    constexpr Nullable(std::nullptr_t) noexcept : state_(State::Null) {}
    template <class U = T>
      requires(std::is_constructible_v<T, U &&> && !std::is_same_v<std::remove_cvref_t<U>, Nullable> && !std::is_same_v<std::remove_cvref_t<U>, Undefined> &&
               !std::is_same_v<std::remove_cvref_t<U>, std::nullptr_t>)
    constexpr explicit(!std::is_convertible_v<U&&, T>) Nullable(U&& value) : state_(State::Value), value_(std::forward<U>(value)) {}

    State state() const noexcept { return state_; }
    bool isUndefined() const noexcept { return state_ == State::Undefined; }
    bool isNull() const noexcept { return state_ == State::Null; }
    // x == null: undefined or null.
    bool isNullish() const noexcept { return state_ != State::Value; }
    bool hasValue() const noexcept { return state_ == State::Value; }

    T& operator*() & noexcept { return *value_; }
    const T& operator*() const& noexcept { return *value_; }
    T* operator->() noexcept { return &*value_; }
    const T* operator->() const noexcept { return &*value_; }
    // std::bad_optional_access when null or undefined.
    T& value() & { return value_.value(); }
    const T& value() const& { return value_.value(); }
    // x ?? fallback
    template <class U>
    T valueOr(U&& fallback) const& {
      return hasValue() ? *value_ : static_cast<T>(std::forward<U>(fallback));
    }
    // null and undefined both become std::nullopt.
    std::optional<T> toOptional() const { return hasValue() ? value_ : std::nullopt; }

    // The member value JSON.stringify writes: nullopt for undefined (the member is omitted;
    // an array element prints null, so use `.toJson().value_or(nullptr)` there).
    std::optional<json::value> toJson() const {
      if (isUndefined()) return std::nullopt;
      if (isNull()) return json::value(nullptr);
      return toJsonValue(*value_);
    }
    // A member read from JSON: nullptr (absent) is undefined, JSON null is null; nullopt when
    // the member holds a value of another type.
    static std::optional<Nullable> fromJson(const json::value* member) {
      if (!member) return Nullable();
      if (member->is_null()) return Nullable(nullptr);
      std::optional<T> converted = fromJsonValue<T>(*member);
      if (!converted) return std::nullopt;
      return Nullable(std::move(*converted));
    }

    // === between two Nullables: undefined only equals undefined, null only null.
    friend bool operator==(const Nullable& a, const Nullable& b) {
      if (a.state_ != b.state_) return false;
      return a.state_ != State::Value || *a.value_ == *b.value_;
    }
    friend bool operator==(const Nullable& a, std::nullptr_t) noexcept { return a.isNull(); }    // x === null
    friend bool operator==(const Nullable& a, Undefined) noexcept { return a.isUndefined(); }  // x === undefined
    // x === value for any value a T compares with (false while x is null or undefined). The
    // Nullable operand is deduced (N), never converted, and the type test comes first: so
    // `iterator == iterator` over a container of Nullables, which finds this friend through
    // argument-dependent lookup, rejects it before evaluating `T == U` (whose evaluation would
    // otherwise depend on itself).
    template <class N, class U>
      requires(std::is_same_v<N, Nullable> && !std::is_same_v<std::remove_cvref_t<U>, Nullable> && !std::is_same_v<std::remove_cvref_t<U>, Undefined> &&
               !std::is_same_v<std::remove_cvref_t<U>, std::nullptr_t> &&
               requires(const T& t, const U& u) {
                 { t == u } -> std::convertible_to<bool>;
               })
    friend bool operator==(const N& a, const U& value) {
      return a.hasValue() && *a.value_ == value;
    }

  private:
    State state_ = State::Undefined;
    std::optional<T> value_;
  };

  // std::optional<T> as T | undefined, or as T | null.
  template <class T>
  Nullable<T> orUndefined(std::optional<T> value) {
    if (!value) return Nullable<T>();
    return Nullable<T>(std::move(*value));
  }
  template <class T>
  Nullable<T> orNull(std::optional<T> value) {
    if (!value) return Nullable<T>(nullptr);
    return Nullable<T>(std::move(*value));
  }

  // obj[key] = value for a possibly-undefined value: undefined removes the member (JSON
  // keeps no undefined members), null stores null.
  template <class T>
  void objectSet(json::object& object, std::string_view key, const Nullable<T>& value) {
    std::optional<json::value> member = value.toJson();
    if (!member) objectDelete(object, key);
    else objectSet(object, key, std::move(*member));
  }
  template <class T>
  void objectSet(json::object& object, std::string_view key, const std::optional<T>& value) {
    if (!value) objectDelete(object, key);
    else objectSet(object, key, toJsonValue(*value));
  }
  template <class T>
  void objectDefine(json::object& object, std::string_view key, const Nullable<T>& value) {
    std::optional<json::value> member = value.toJson();
    if (!member) objectDelete(object, key);
    else objectDefine(object, key, std::move(*member));
  }
  template <class T>
  void objectDefine(json::object& object, std::string_view key, const std::optional<T>& value) {
    if (!value) objectDelete(object, key);
    else objectDefine(object, key, toJsonValue(*value));
  }
  // const raw = obj[key] read as T | null | undefined; nullopt when it holds another type.
  template <class T>
  std::optional<Nullable<T>> getNullable(const json::object& object, std::string_view key) {
    return Nullable<T>::fromJson(get(object, key));
  }
}
