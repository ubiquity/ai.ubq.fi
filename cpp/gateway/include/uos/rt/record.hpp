#pragma once
// Member-wise JSON conversion of record structs: the bodies of a record's tag_invoke
// value_from / value_to overloads. Usage and supported member types: docs/rt/record.md.
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <boost/system/system_error.hpp>

#include <concepts>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uos::rt::record {
  template <class T> struct IsVector : std::false_type {};
  template <class T, class A> struct IsVector<std::vector<T, A>> : std::true_type {};

  // Integral types other than bool and the character types. They hold JS numbers that are
  // integers in the type's range.
  template <class T>
  inline constexpr bool IsInteger = std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> && !std::is_same_v<T, wchar_t> &&
                                    !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t>;
  template <class T>
  inline constexpr bool IsJson = std::is_same_v<T, json::value> || std::is_same_v<T, json::object> || std::is_same_v<T, json::array>;
  template <class T>
  inline constexpr bool IsScalar = std::is_same_v<T, double> || std::is_same_v<T, bool> || std::is_same_v<T, std::string> || IsInteger<T>;

  // The tag_invoke overloads declared next to an enum, record or variant. Boost's own
  // std::optional and std::variant readers take try_value_to_tag, so HasValueTo sees only a
  // pair the porter declared; Boost's generic writers for those types do satisfy HasValueFrom.
  template <class T>
  concept HasValueTo = requires(const json::value& value) {
    { tag_invoke(json::value_to_tag<T>(), value) } -> std::same_as<T>;
  };
  template <class T>
  concept HasValueFrom = requires(json::value& out, const T& value) { tag_invoke(json::value_from_tag(), out, value); };

  // The member types Reader / Writer convert. Anything else fails a static_assert.
  template <class T> struct Readable : std::bool_constant<IsScalar<T> || IsJson<T> || ((std::is_enum_v<T> || std::is_class_v<T>) && HasValueTo<T>)> {};
  template <class T, class A> struct Readable<std::vector<T, A>> : Readable<T> {};
  template <class T> struct Writable : std::bool_constant<IsScalar<T> || IsJson<T> || ((std::is_enum_v<T> || std::is_class_v<T>) && HasValueFrom<T>)> {};
  template <class T, class A> struct Writable<std::vector<T, A>> : Writable<T> {};
  template <class T> struct Writable<std::optional<T>> : std::false_type {};
  template <> struct Writable<std::nullopt_t> : std::false_type {};
  // Boost writes any variant; one is a member only with its own pair, which its value_to proves.
  template <class... Ts> struct Writable<std::variant<Ts...>> : std::bool_constant<HasValueTo<std::variant<Ts...>>> {};

  // The member's JS type for messages; values are never printed (records hold tokens).
  inline std::string describe(const json::value* value) {
    if (!value) return "undefined";
    if (value->is_null()) return "null";
    if (value->is_array()) return "array";
    return std::string(rt::typeOf(*value));
  }

  template <class T> std::string expected() {
    if constexpr (std::is_same_v<T, double> || IsInteger<T>) return "number";
    else if constexpr (std::is_same_v<T, bool>) return "boolean";
    else if constexpr (std::is_same_v<T, std::string>) return "string";
    else if constexpr (std::is_same_v<T, json::value>) return "JSON value";
    else if constexpr (std::is_same_v<T, json::array>) return "array";
    else if constexpr (IsVector<T>::value) return expected<typename T::value_type>() + "[]";
    else if constexpr (std::is_enum_v<T>) return "string";
    else return "object";
  }

  // What an integer member wants when it holds a number that is not an integer in its range.
  template <class T> std::string integerRange() {
    return "integer from " + std::to_string(std::numeric_limits<T>::min()) + " to " + std::to_string(std::numeric_limits<T>::max());
  }

  template <class T> json::value toJson(const T& value) {
    static_assert(Writable<T>::value,
                  "uos::rt::record cannot write this member type. Supported: double, bool, std::string, integer types, json::value, "
                  "json::object, json::array, std::vector of these, and enums, records and variants with their own tag_invoke pair. "
                  "T | null and key?: T members go through orNull, orUndefined or nullable (docs/rt/record.md).");
    if constexpr (std::is_same_v<T, double>) return rt::makeNumber(value);
    else if constexpr (std::is_same_v<T, bool>) return json::value(value);
    else if constexpr (std::is_same_v<T, std::string>) return json::value(json::string(value));
    else if constexpr (IsInteger<T>) return rt::toJsonValue(value);
    else if constexpr (IsJson<T>) return rt::canonicalNumbers(json::value(value));
    else if constexpr (IsVector<T>::value) {
      json::array out;
      out.reserve(value.size());
      for (const auto& item : value) out.push_back(toJson(item));
      return out;
    } else return json::value_from(value);
  }

  // Builds the object in member declaration order.
  class Writer {
  public:
    // A member the type requires.
    template <class T> Writer& required(std::string_view key, const T& value) {
      object_[key] = toJson(value);
      return *this;
    }
    // `T | null`: std::nullopt is written as null.
    template <class T> Writer& orNull(std::string_view key, const std::optional<T>& value) {
      object_[key] = value ? toJson(*value) : json::value(nullptr);
      return *this;
    }
    // `key?: T`: std::nullopt (undefined) omits the member.
    template <class T> Writer& orUndefined(std::string_view key, const std::optional<T>& value) {
      if (value) object_[key] = toJson(*value);
      return *this;
    }
    // `key?: T | null`
    template <class T> Writer& nullable(std::string_view key, const rt::Nullable<T>& value) {
      if (value.isNull()) object_[key] = nullptr;
      else if (value.hasValue()) object_[key] = toJson(*value);
      return *this;
    }
    json::value done() { return json::value(std::move(object_)); }

  private:
    json::object object_;
  };

  // Reads the members of a JSON object; the first member the C++ type cannot hold
  // throws rt::TypeError("<type>.<member> is <JS type>, not <wanted>").
  class Reader {
  public:
    Reader(const json::value& value, std::string_view type) : type_(type), object_(value.if_object()) {
      if (!object_) throw rt::TypeError(std::string(type) + " is " + describe(&value) + ", not object");
    }
    template <class T> T required(std::string_view key) const {
      const json::value* member = rt::get(*object_, key);
      if (!member) fail(key, member, expected<T>());
      return convert<T>(key, *member, "");
    }
    template <class T> std::optional<T> orNull(std::string_view key) const {
      const json::value* member = rt::get(*object_, key);
      if (!member) fail(key, member, expected<T>() + " or null");
      if (member->is_null()) return std::nullopt;
      return convert<T>(key, *member, " or null");
    }
    template <class T> std::optional<T> orUndefined(std::string_view key) const {
      const json::value* member = rt::get(*object_, key);
      if (!member) return std::nullopt;
      return convert<T>(key, *member, " or undefined");
    }
    template <class T> rt::Nullable<T> nullable(std::string_view key) const {
      const json::value* member = rt::get(*object_, key);
      if (!member) return rt::undefined;
      if (member->is_null()) return nullptr;
      return convert<T>(key, *member, ", null or undefined");
    }
    [[noreturn]] void fail(std::string_view key, const json::value* member, const std::string& wanted) const {
      throw rt::TypeError(std::string(type_) + "." + std::string(key) + " is " + describe(member) + ", not " + wanted);
    }

  private:
    template <class T> T convert(std::string_view key, const json::value& member, std::string_view suffix) const {
      static_assert(Readable<T>::value,
                    "uos::rt::record cannot read this member type. Supported: double, bool, std::string, integer types, json::value, "
                    "json::object, json::array, std::vector of these, and enums, records and variants with their own tag_invoke pair. "
                    "T | null and key?: T members go through orNull, orUndefined or nullable (docs/rt/record.md).");
      if constexpr (std::is_same_v<T, double>) {
        if (const std::optional<double> number = rt::getNumber(member)) return *number;
      } else if constexpr (std::is_same_v<T, bool>) {
        if (member.is_bool()) return member.get_bool();
      } else if constexpr (std::is_same_v<T, std::string>) {
        if (member.is_string()) return std::string(member.get_string());
      } else if constexpr (IsInteger<T>) {
        if (rt::isNumber(member)) {
          if (const std::optional<T> integer = rt::fromJsonValue<T>(member)) return *integer;
          fail(key, &member, integerRange<T>() + std::string(suffix));
        }
      } else if constexpr (std::is_same_v<T, json::value>) {
        return member;
      } else if constexpr (std::is_same_v<T, json::object>) {
        if (member.is_object()) return member.get_object();
      } else if constexpr (std::is_same_v<T, json::array>) {
        if (member.is_array()) return member.get_array();
      } else if constexpr (IsVector<T>::value) {
        if (const json::array* items = member.if_array()) {
          const std::string label = std::string(key) + "[]";
          T out;
          out.reserve(items->size());
          for (const json::value& item : *items) out.push_back(convert<typename T::value_type>(label, item, ""));
          return out;
        }
      } else if constexpr (std::is_enum_v<T>) {
        return nested<T>(key, member);
      } else {
        if (member.is_object()) return nested<T>(key, member);
      }
      fail(key, &member, expected<T>() + std::string(suffix));
    }

    // The member type's own value_to. rt errors pass through; any other exception (Boost's
    // system_error from a json accessor, say) becomes "<type>.<member>: <reason>", so value_to
    // on a record throws only rt errors (and std::bad_alloc).
    template <class T> T nested(std::string_view key, const json::value& member) const {
      try {
        return json::value_to<T>(member);
      } catch (const rt::JsError&) {
        throw;
      } catch (const std::bad_alloc&) {
        throw;
      } catch (const boost::system::system_error& error) {
        throw rt::TypeError(std::string(type_) + "." + std::string(key) + ": " + error.code().message());
      } catch (const std::exception& error) {
        throw rt::TypeError(std::string(type_) + "." + std::string(key) + ": " + error.what());
      }
    }

    std::string_view type_;
    const json::object* object_;
  };
}
