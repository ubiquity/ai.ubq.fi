// Tests for uos/rt/nullable.hpp. JSON texts and the ?? / === / == truth tables were
// produced by Bun 1.4 evaluating the equivalent JavaScript.
#include "uos/rt/nullable.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;

TEST("states, predicates and ?? (valueOr)") {
  const rt::Nullable<int> missing;
  const rt::Nullable<int> null = nullptr;
  const rt::Nullable<int> zero = 0;
  const rt::Nullable<int> explicitUndefined = rt::undefined;
  EXPECT(missing.isUndefined() && missing.isNullish() && !missing.hasValue());
  EXPECT(explicitUndefined.isUndefined());
  EXPECT(null.isNull() && null.isNullish() && !null.isUndefined());
  EXPECT(zero.hasValue() && !zero.isNullish());
  // [undefined ?? 5, null ?? 5, 0 ?? 5] is [5, 5, 0]
  EXPECT_EQ(missing.valueOr(5), 5);
  EXPECT_EQ(null.valueOr(5), 5);
  EXPECT_EQ(zero.valueOr(5), 0);
  const rt::Nullable<std::string> empty = "";
  EXPECT_EQ(empty.valueOr("5"), "");
  const rt::Nullable<bool> no = false;
  EXPECT_EQ(no.valueOr(true), false);
  EXPECT_EQ(*zero, 0);
  const rt::Nullable<std::string> text = std::string("abc");
  EXPECT_EQ(text->size(), 3u);
  EXPECT_THROWS(null.value(), std::bad_optional_access);
  EXPECT(null.state() == rt::Nullable<int>::State::Null);
}

TEST("=== and == null follow the JS truth tables") {
  const rt::Nullable<int> missing;
  const rt::Nullable<int> null = nullptr;
  const rt::Nullable<int> one = 1;
  // [u === null, u == null, u === undefined, n === null, n == null, n === undefined, n == undefined]
  // is [false, true, true, true, true, false, true]
  EXPECT(!(missing == nullptr));
  EXPECT(missing.isNullish());
  EXPECT(missing == rt::undefined);
  EXPECT(null == nullptr);
  EXPECT(null.isNullish());
  EXPECT(!(null == rt::undefined));
  EXPECT(one == 1);
  EXPECT(1 == one);
  EXPECT(one != 2);
  EXPECT(!(null == 0));
  EXPECT(missing == rt::Nullable<int>());
  EXPECT(missing != null);
  EXPECT(one == rt::Nullable<int>(1));
  EXPECT(one != rt::Nullable<int>(nullptr));
  EXPECT(rt::Nullable<std::string>("a") == "a");
}

TEST("JSON.stringify drops undefined members and prints null; arrays print both as null") {
  json::object object;
  rt::objectSet(object, "a", rt::Nullable<int>());
  rt::objectSet(object, "b", rt::Nullable<int>(nullptr));
  rt::objectSet(object, "c", rt::Nullable<int>(1));
  // JSON.stringify({ a: undefined, b: null, c: 1 })
  EXPECT_EQ(rt::jsonStringify(json::value(object)), R"({"b":null,"c":1})");
  json::array array;
  for (const auto& element : {rt::Nullable<int>(), rt::Nullable<int>(nullptr), rt::Nullable<int>(1)}) array.push_back(element.toJson().value_or(nullptr));
  // JSON.stringify([undefined, null, 1])
  EXPECT_EQ(rt::jsonStringify(json::value(array)), "[null,null,1]");
  // const o = { a: 1, b: 2 }; o.a = undefined; JSON.stringify(o) / o.a = null
  json::object assigned = rt::jsonParse(R"({"a":1,"b":2})").get_object();
  rt::objectSet(assigned, "a", rt::Nullable<int>());
  EXPECT_EQ(rt::jsonStringify(json::value(assigned)), R"({"b":2})");
  json::object nulled = rt::jsonParse(R"({"a":1,"b":2})").get_object();
  rt::objectSet(nulled, "a", rt::Nullable<int>(nullptr));
  EXPECT_EQ(rt::jsonStringify(json::value(nulled)), R"({"a":null,"b":2})");
  // { ...{ a: 1, b: 2 }, a: undefined }
  json::object spread = rt::objectSpread(rt::jsonParse(R"({"a":1,"b":2})").get_object());
  rt::objectDefine(spread, "a", rt::Nullable<int>());
  EXPECT_EQ(rt::jsonStringify(json::value(spread)), R"({"b":2})");
  json::object optional;
  rt::objectSet(optional, "x", std::optional<std::string>());
  rt::objectSet(optional, "y", std::optional<std::string>("v"));
  rt::objectDefine(optional, "z", std::optional<double>(2.0));
  rt::objectDefine(optional, "w", std::optional<double>());
  EXPECT_EQ(rt::jsonStringify(json::value(optional)), R"({"y":"v","z":2})");
  EXPECT(optional.at("z").is_int64());
}

TEST("reading T | null | undefined members from JSON") {
  const json::object object = rt::jsonParse(R"({"s":"x","n":null,"i":3,"f":1.5,"neg":-1,"big":9007199254740992,"o":{"k":1},"a":[1],"b":true})").get_object();
  EXPECT(rt::getNullable<std::string>(object, "missing")->isUndefined());
  EXPECT(rt::getNullable<std::string>(object, "n")->isNull());
  EXPECT_EQ(rt::getNullable<std::string>(object, "s")->value(), "x");
  EXPECT(!rt::getNullable<std::string>(object, "i").has_value());
  EXPECT_EQ(rt::getNullable<int>(object, "i")->value(), 3);
  EXPECT(!rt::getNullable<int>(object, "f").has_value());
  EXPECT_EQ(rt::getNullable<double>(object, "f")->value(), 1.5);
  EXPECT(!rt::getNullable<unsigned>(object, "neg").has_value());
  EXPECT_EQ(rt::getNullable<std::int64_t>(object, "big")->value(), std::int64_t{9007199254740992});
  EXPECT(!rt::getNullable<std::int32_t>(object, "big").has_value());
  EXPECT_EQ(rt::getNullable<json::object>(object, "o")->value().size(), 1u);
  EXPECT(!rt::getNullable<json::object>(object, "a").has_value());
  EXPECT_EQ(rt::getNullable<json::array>(object, "a")->value().size(), 1u);
  EXPECT_EQ(rt::getNullable<bool>(object, "b")->value(), true);
  EXPECT(rt::getNullable<json::value>(object, "n")->isNull());
  EXPECT(rt::getNullable<json::value>(object, "o")->hasValue());
  EXPECT(rt::Nullable<int>::fromJson(nullptr)->isUndefined());
}

TEST("std::optional conversions and JSON values of held types") {
  EXPECT(rt::orUndefined(std::optional<int>()).isUndefined());
  EXPECT(rt::orNull(std::optional<int>()).isNull());
  EXPECT_EQ(rt::orNull(std::optional<int>(4)).value(), 4);
  EXPECT_EQ(rt::Nullable<int>(4).toOptional(), std::optional<int>(4));
  EXPECT_EQ(rt::Nullable<int>(nullptr).toOptional(), std::nullopt);
  EXPECT_EQ(rt::Nullable<int>().toOptional(), std::nullopt);
  EXPECT(!rt::Nullable<int>().toJson().has_value());
  EXPECT_EQ(*rt::Nullable<int>(nullptr).toJson(), json::value(nullptr));
  EXPECT_EQ(*rt::Nullable<double>(2.0).toJson(), json::value(2));
  EXPECT(rt::Nullable<double>(-0.0).toJson()->is_double());
  EXPECT_EQ(*rt::Nullable<std::string>("q").toJson(), json::value("q"));
  EXPECT_EQ(*rt::Nullable<bool>(true).toJson(), json::value(true));
  EXPECT_EQ(rt::toJsonValue(std::uint64_t{7}), json::value(7));
  EXPECT_EQ(rt::toJsonValue(std::vector<int>{1, 2}), rt::jsonParse("[1,2]"));
  EXPECT_EQ(rt::fromJsonValue<std::vector<int>>(rt::jsonParse("[1,2]")), std::optional<std::vector<int>>(std::vector<int>{1, 2}));
  EXPECT(!rt::fromJsonValue<std::vector<int>>(rt::jsonParse("{}")).has_value());
}

TEST("a range-for over std::vector<Nullable<std::string>> compiles and === keeps its meaning") {
  // Iterator comparisons over a container of Nullables used to find Nullable's `== U` friend and
  // fail with "satisfaction of atomic constraint ... depends on itself".
  const std::vector<rt::Nullable<std::string>> values{rt::Nullable<std::string>("a"), nullptr, rt::undefined, rt::Nullable<std::string>("")};
  std::vector<std::string> seen;
  for (const rt::Nullable<std::string>& value : values) seen.push_back(value.isUndefined() ? "undefined" : value.isNull() ? "null" : "=" + *value);
  EXPECT_EQ(seen, (std::vector<std::string>{"=a", "null", "undefined", "="}));
  EXPECT(std::find(values.begin(), values.end(), rt::Nullable<std::string>(nullptr)) == values.begin() + 1);
  EXPECT(values == values);
  // x === "a" / "a" === x / x !== "b": a T-comparable value matches only a held value.
  EXPECT(values[0] == "a");
  EXPECT("a" == values[0]);
  EXPECT(values[0] == std::string("a"));
  EXPECT(values[0] == std::string_view("a"));
  EXPECT(values[0] != "b");
  EXPECT(!(values[1] == "a") && !(values[2] == "a"));
  EXPECT(values[3] == "" && !(values[2] == ""));
  EXPECT(values[1] == nullptr && values[2] == rt::undefined && !(values[1] == rt::undefined));
  const rt::Nullable<double> one = 1.0;
  EXPECT(one == 1 && 1 == one && !(one == 2));
  const rt::Nullable<double> missing;
  EXPECT(!(missing == 0));
  std::vector<rt::Nullable<double>> numbers{one, missing, nullptr};
  int held = 0;
  for (const auto& number : numbers) held += number.hasValue() ? 1 : 0;
  EXPECT_EQ(held, 1);
}
