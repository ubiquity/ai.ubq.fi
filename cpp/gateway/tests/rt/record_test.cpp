// Tests for uos/rt/record.hpp: every supported member kind round-trips, value_to rejects what a
// member cannot hold with the documented rt::TypeError text, and the static_asserts below pin
// which member types compile at all.
#include "uos/rt/record.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/test.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;

namespace uos::record_test {
  enum class Color { Red, Blue };
  struct Inner {
    std::string name;
  };
  struct Hold {
    static constexpr std::string_view type = "hold";
    double until_ms;
  };
  struct Done {
    static constexpr std::string_view type = "done";
  };
  using Step = std::variant<Hold, Done>;
  // One member per scalar and JSON kind.
  struct Scalars {
    double d;
    bool b;
    std::string s;
    std::int64_t i64;
    std::int32_t i32;
    std::uint8_t u8;
    std::uint64_t u64;
    json::value v;
    json::object o;
    json::array a;
  };
  // Member shapes, enum, nested record, variant and vector members.
  struct Shapes {
    std::optional<std::int64_t> count;  // count: number | null
    std::optional<std::string> note{};  // note?: string
    rt::Nullable<json::value> extra{};  // extra?: unknown | null
    Color color;
    Inner inner;
    std::vector<Step> steps;
    std::vector<std::vector<std::int32_t>> grid;
    std::vector<json::value> items;
  };
  // Legacy's value_to (only) uses Boost's accessors, which throw boost::system::system_error.
  struct Legacy {
    std::string code;
  };
  struct Wrapper {
    Legacy legacy;
    std::vector<Legacy> legacies;
  };
  struct NoPair {
    double x;
  };

  void tag_invoke(const json::value_from_tag&, json::value& out, Color value);
  Color tag_invoke(const json::value_to_tag<Color>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const Inner& value);
  Inner tag_invoke(const json::value_to_tag<Inner>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const Step& value);
  Step tag_invoke(const json::value_to_tag<Step>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const Scalars& value);
  Scalars tag_invoke(const json::value_to_tag<Scalars>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const Shapes& value);
  Shapes tag_invoke(const json::value_to_tag<Shapes>&, const json::value& value);
  Legacy tag_invoke(const json::value_to_tag<Legacy>&, const json::value& value);
  Wrapper tag_invoke(const json::value_to_tag<Wrapper>&, const json::value& value);

  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, Color value) { out = json::string(value == Color::Red ? "red" : "blue"); }
  Color tag_invoke(const json::value_to_tag<Color>&, const json::value& value) {
    if (const json::string* text = value.if_string()) {
      if (*text == "red") return Color::Red;
      if (*text == "blue") return Color::Blue;
    }
    throw rt::TypeError("Color is " + rt::record::describe(&value) + ", not \"red\" | \"blue\"");
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const Inner& value) { out = Writer().required("name", value.name).done(); }
  Inner tag_invoke(const json::value_to_tag<Inner>&, const json::value& value) {
    const Reader in(value, "Inner");
    return {.name = in.required<std::string>("name")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const Step& value) {
    if (const Hold* hold = std::get_if<Hold>(&value))
      out = Writer().required("type", std::string(Hold::type)).required("until_ms", hold->until_ms).done();
    else out = Writer().required("type", std::string(Done::type)).done();
  }
  Step tag_invoke(const json::value_to_tag<Step>&, const json::value& value) {
    const Reader in(value, "Step");
    const std::string type = in.required<std::string>("type");
    if (type == Hold::type) return Hold{.until_ms = in.required<double>("until_ms")};
    if (type == Done::type) return Done{};
    in.fail("type", rt::get(value, "type"), "\"hold\" | \"done\"");
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const Scalars& value) {
    out = Writer()
      .required("d", value.d)
      .required("b", value.b)
      .required("s", value.s)
      .required("i64", value.i64)
      .required("i32", value.i32)
      .required("u8", value.u8)
      .required("u64", value.u64)
      .required("v", value.v)
      .required("o", value.o)
      .required("a", value.a)
      .done();
  }
  Scalars tag_invoke(const json::value_to_tag<Scalars>&, const json::value& value) {
    const Reader in(value, "Scalars");
    return {
      .d = in.required<double>("d"),
      .b = in.required<bool>("b"),
      .s = in.required<std::string>("s"),
      .i64 = in.required<std::int64_t>("i64"),
      .i32 = in.required<std::int32_t>("i32"),
      .u8 = in.required<std::uint8_t>("u8"),
      .u64 = in.required<std::uint64_t>("u64"),
      .v = in.required<json::value>("v"),
      .o = in.required<json::object>("o"),
      .a = in.required<json::array>("a"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const Shapes& value) {
    out = Writer()
      .orNull("count", value.count)
      .orUndefined("note", value.note)
      .nullable("extra", value.extra)
      .required("color", value.color)
      .required("inner", value.inner)
      .required("steps", value.steps)
      .required("grid", value.grid)
      .required("items", value.items)
      .done();
  }
  Shapes tag_invoke(const json::value_to_tag<Shapes>&, const json::value& value) {
    const Reader in(value, "Shapes");
    return {
      .count = in.orNull<std::int64_t>("count"),
      .note = in.orUndefined<std::string>("note"),
      .extra = in.nullable<json::value>("extra"),
      .color = in.required<Color>("color"),
      .inner = in.required<Inner>("inner"),
      .steps = in.required<std::vector<Step>>("steps"),
      .grid = in.required<std::vector<std::vector<std::int32_t>>>("grid"),
      .items = in.required<std::vector<json::value>>("items"),
    };
  }

  Legacy tag_invoke(const json::value_to_tag<Legacy>&, const json::value& value) {
    const std::string code = json::value_to<std::string>(value.as_object().at("code"));
    if (code == "boom") throw std::runtime_error("boom");
    if (code == "deep") throw rt::RangeError("Maximum call stack size exceeded.");
    return {.code = code};
  }
  Wrapper tag_invoke(const json::value_to_tag<Wrapper>&, const json::value& value) {
    const Reader in(value, "Wrapper");
    return {.legacy = in.required<Legacy>("legacy"), .legacies = in.required<std::vector<Legacy>>("legacies")};
  }
}

namespace {
  namespace record = uos::rt::record;
  namespace t = uos::record_test;
  using t::Scalars;
  using t::Shapes;
  using t::Wrapper;

  // Which member types compile: everything else fails the static_assert in Reader / Writer.
  template <class T> constexpr bool both = record::Readable<T>::value && record::Writable<T>::value;
  template <class T> constexpr bool neither = !record::Readable<T>::value && !record::Writable<T>::value;
  static_assert(both<double> && both<bool> && both<std::string> && both<std::int64_t> && both<int> && both<std::uint8_t> && both<std::uint64_t>);
  static_assert(both<json::value> && both<json::object> && both<json::array>);
  static_assert(both<std::vector<std::vector<std::int32_t>>> && both<std::vector<json::value>> && both<std::vector<t::Step>>);
  static_assert(both<t::Color> && both<t::Inner> && both<t::Step>);
  static_assert(record::Readable<t::Legacy>::value && !record::Writable<t::Legacy>::value);
  static_assert(neither<float> && neither<char> && neither<std::string_view> && neither<const char*> && neither<json::string>);
  static_assert(neither<std::optional<double>> && neither<rt::Nullable<double>> && neither<std::vector<std::optional<double>>>);
  static_assert(neither<std::map<std::string, double>> && neither<std::array<int, 2>> && neither<t::NoPair> && !record::Readable<t::Hold>::value);
  static_assert(neither<std::variant<double, std::string>> && neither<std::variant<t::Inner, t::Hold>>);

  constexpr std::string_view SCALARS =
    R"({"d":1.5,"b":true,"s":"x","i64":-9007199254740991,"i32":-2147483648,"u8":255,"u64":1152921504606847000,"v":{"k":[1,null,"t"]},"o":{"n":2},"a":[1,{"m":3}]})";
  constexpr std::string_view SHAPES =
    R"({"count":7,"note":"n","extra":null,"color":"blue","inner":{"name":"i"},"steps":[{"type":"hold","until_ms":5},{"type":"done"}],"grid":[[1,2],[]],"items":[null,"t",{"k":1}]})";

  // The rt::TypeError message value_to<T> throws, "<accepted>", or "<other: ...>" for any other exception.
  template <class T> std::string rejected(const json::value& value) {
    try {
      (void)json::value_to<T>(value);
    } catch (const rt::TypeError& error) {
      return error.message;
    } catch (const std::exception& error) {
      return std::string("<other: ") + error.what() + ">";
    }
    return "<accepted>";
  }
  template <class T> std::string rejection(std::string_view text) { return rejected<T>(rt::jsonParse(text)); }
  // The JSON text of value_from(value_to<T>(text)).
  template <class T> std::string roundTrip(std::string_view text) { return rt::jsonStringify(json::value_from(json::value_to<T>(rt::jsonParse(text)))); }
  // `base` with member `key` replaced, or removed when `member` is std::nullopt.
  json::value with(std::string_view base, std::string_view key, const std::optional<json::value>& member) {
    json::value value = rt::jsonParse(base);
    if (member) value.as_object()[key] = *member;
    else rt::objectDelete(value.as_object(), key);
    return value;
  }
  std::string scalar(std::string_view key, const std::optional<json::value>& member) { return rejected<Scalars>(with(SCALARS, key, member)); }
  std::string shape(std::string_view key, const std::optional<json::value>& member) { return rejected<Shapes>(with(SHAPES, key, member)); }
}

TEST("scalar, integer and JSON members round-trip") {
  EXPECT_EQ(roundTrip<Scalars>(SCALARS), std::string(SCALARS));
  const Scalars read = json::value_to<Scalars>(rt::jsonParse(SCALARS));
  EXPECT_EQ(read.d, 1.5);
  EXPECT_EQ(read.b, true);
  EXPECT_EQ(read.s, "x");
  EXPECT_EQ(read.i64, std::int64_t{-9007199254740991});
  EXPECT_EQ(read.i32, std::numeric_limits<std::int32_t>::min());
  EXPECT_EQ(read.u8, std::uint8_t{255});
  EXPECT_EQ(read.u64, std::uint64_t{1} << 60);
  EXPECT_EQ(rt::jsonStringify(read.v), R"({"k":[1,null,"t"]})");
  EXPECT_EQ(rt::jsonStringify(json::value(read.o)), R"({"n":2})");
  EXPECT_EQ(rt::jsonStringify(json::value(read.a)), R"([1,{"m":3}])");
  for (const std::string_view v : {"null", "true", "-0.5", R"("t")", "[]", "{}"}) {
    EXPECT_EQ(scalar("v", rt::jsonParse(v)), "<accepted>");
    EXPECT_EQ(rt::jsonStringify(json::value_to<Scalars>(with(SCALARS, "v", rt::jsonParse(v))).v), std::string(v));
  }
}

TEST("member shapes, enum, nested record, variant and vector members round-trip") {
  EXPECT_EQ(roundTrip<Shapes>(SHAPES), std::string(SHAPES));
  const Shapes read = json::value_to<Shapes>(rt::jsonParse(SHAPES));
  EXPECT_EQ(read.count, std::optional<std::int64_t>(7));
  EXPECT(read.extra.isNull());
  EXPECT(read.color == t::Color::Blue);
  EXPECT_EQ(read.inner.name, "i");
  EXPECT_EQ(read.steps.size(), std::size_t{2});
  EXPECT_EQ(std::get<t::Hold>(read.steps[0]).until_ms, 5.0);
  EXPECT_EQ(read.grid, (std::vector<std::vector<std::int32_t>>{{1, 2}, {}}));
  const std::string sparse = R"({"count":null,"color":"red","inner":{"name":""},"steps":[],"grid":[],"items":[]})";
  EXPECT_EQ(roundTrip<Shapes>(sparse), sparse);
  const Shapes empty = json::value_to<Shapes>(rt::jsonParse(sparse));
  EXPECT(!empty.count && !empty.note && empty.extra.isUndefined());
  EXPECT_EQ(rt::jsonStringify(json::value_from(json::value_to<Shapes>(with(SHAPES, "extra", json::value(5))))), std::string(R"({"count":7,"note":"n","extra":5,"color":"blue","inner":{"name":"i"},"steps":[{"type":"hold","until_ms":5},{"type":"done"}],"grid":[[1,2],[]],"items":[null,"t",{"k":1}]})"));
}

TEST("numbers are written canonically; JSON members are read as they are") {
  const json::value written = record::Writer()
                                .required("big", std::int64_t{1} << 60)
                                .required("small", std::uint64_t{5})
                                .required("negative", std::int8_t{-3})
                                .required("double", 6.0)
                                .required("v", json::value(2.0))
                                .required("o", json::object{{"n", 3.0}})
                                .required("a", json::array{4.0, json::object{{"m", 5.0}}})
                                .done();
  EXPECT(written.at("big").is_double());
  EXPECT_EQ(written.at("big").get_double(), 1152921504606846976.0);
  EXPECT(written.at("small").is_int64() && written.at("negative").is_int64() && written.at("double").is_int64());
  EXPECT(written.at("v").is_int64());
  EXPECT(written.at("o").at("n").is_int64());
  EXPECT(written.at("a").at(0).is_int64() && written.at("a").at(1).at("m").is_int64());
  EXPECT_EQ(rt::jsonStringify(written), R"({"big":1152921504606847000,"small":5,"negative":-3,"double":6,"v":2,"o":{"n":3},"a":[4,{"m":5}]})");

  json::value input = rt::jsonParse(SCALARS);
  input.as_object()["v"] = json::value(7.0);
  input.as_object()["o"] = json::object{{"n", 2.0}};
  input.as_object()["a"] = json::array{1.0};
  const Scalars read = json::value_to<Scalars>(input);
  EXPECT(read.v.is_double() && read.o.at("n").is_double() && read.a.at(0).is_double());
  const json::value rewritten = json::value_from(read);
  EXPECT(rewritten.at("v").is_int64() && rewritten.at("o").at("n").is_int64() && rewritten.at("a").at(0).is_int64());
}

TEST("integer members accept JS integers in their range and reject the rest") {
  const std::string i32Range = "Scalars.i32 is number, not integer from -2147483648 to 2147483647";
  EXPECT_EQ(scalar("i32", json::value(2147483647)), "<accepted>");
  EXPECT_EQ(scalar("i32", rt::makeNumber(2147483648.0)), i32Range);
  EXPECT_EQ(scalar("i32", rt::makeNumber(-2147483649.0)), i32Range);
  EXPECT_EQ(scalar("i32", rt::makeNumber(1.5)), i32Range);
  EXPECT_EQ(scalar("i32", rt::makeNumber(std::nan(""))), i32Range);
  EXPECT_EQ(scalar("i32", rt::makeNumber(std::numeric_limits<double>::infinity())), i32Range);
  EXPECT_EQ(json::value_to<Scalars>(with(SCALARS, "i32", rt::makeNumber(-0.0))).i32, 0);
  EXPECT_EQ(scalar("i32", json::value(json::string("1"))), "Scalars.i32 is string, not number");
  EXPECT_EQ(scalar("i32", json::value(nullptr)), "Scalars.i32 is null, not number");
  EXPECT_EQ(scalar("i32", json::value(true)), "Scalars.i32 is boolean, not number");
  EXPECT_EQ(scalar("i32", std::nullopt), "Scalars.i32 is undefined, not number");
  EXPECT_EQ(scalar("u8", json::value(-1)), "Scalars.u8 is number, not integer from 0 to 255");
  EXPECT_EQ(scalar("u8", json::value(256)), "Scalars.u8 is number, not integer from 0 to 255");
  const std::string i64Range = "Scalars.i64 is number, not integer from -9223372036854775808 to 9223372036854775807";
  EXPECT_EQ(scalar("i64", rt::jsonParse("9223372036854775807")), i64Range);
  EXPECT_EQ(json::value_to<Scalars>(with(SCALARS, "i64", rt::jsonParse("-9223372036854775808"))).i64, std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(json::value_to<Scalars>(with(SCALARS, "i64", rt::jsonParse("9007199254740993"))).i64, std::int64_t{9007199254740992});
  EXPECT_EQ(scalar("u64", rt::jsonParse("18446744073709551615")), "Scalars.u64 is number, not integer from 0 to 18446744073709551615");
  EXPECT_EQ(json::value_to<Scalars>(with(SCALARS, "u64", rt::jsonParse("18446744073709550000"))).u64, std::uint64_t{18446744073709549568u});
  EXPECT_EQ(shape("count", rt::makeNumber(1.5)), "Shapes.count is number, not integer from -9223372036854775808 to 9223372036854775807 or null");
  EXPECT_EQ(shape("count", std::nullopt), "Shapes.count is undefined, not number or null");
  EXPECT_EQ(shape("grid", rt::jsonParse("[[1],[2.5]]")), "Shapes.grid[][] is number, not integer from -2147483648 to 2147483647");
  EXPECT_EQ(shape("grid", rt::jsonParse(R"([[1],"x"])")), "Shapes.grid[] is string, not number[]");
  EXPECT_EQ(shape("grid", json::value(json::string("x"))), "Shapes.grid is string, not number[][]");
}

TEST("wrong-type and missing-member texts") {
  EXPECT_EQ(scalar("d", json::value(json::string("x"))), "Scalars.d is string, not number");
  EXPECT_EQ(scalar("d", std::nullopt), "Scalars.d is undefined, not number");
  EXPECT_EQ(scalar("b", json::value(1)), "Scalars.b is number, not boolean");
  EXPECT_EQ(scalar("b", std::nullopt), "Scalars.b is undefined, not boolean");
  EXPECT_EQ(scalar("s", json::value(1)), "Scalars.s is number, not string");
  EXPECT_EQ(scalar("s", std::nullopt), "Scalars.s is undefined, not string");
  EXPECT_EQ(scalar("v", std::nullopt), "Scalars.v is undefined, not JSON value");
  EXPECT_EQ(scalar("o", json::array()), "Scalars.o is array, not object");
  EXPECT_EQ(scalar("o", json::value(nullptr)), "Scalars.o is null, not object");
  EXPECT_EQ(scalar("o", std::nullopt), "Scalars.o is undefined, not object");
  EXPECT_EQ(scalar("a", json::object()), "Scalars.a is object, not array");
  EXPECT_EQ(scalar("a", json::value(json::string("x"))), "Scalars.a is string, not array");
  EXPECT_EQ(scalar("a", std::nullopt), "Scalars.a is undefined, not array");
  EXPECT_EQ(rejection<Scalars>("[]"), "Scalars is array, not object");
  EXPECT_EQ(rejection<Scalars>("null"), "Scalars is null, not object");
  EXPECT_EQ(shape("note", json::value(nullptr)), "Shapes.note is null, not string or undefined");
  EXPECT_EQ(shape("note", std::nullopt), "<accepted>");
  EXPECT_EQ(shape("extra", std::nullopt), "<accepted>");
  EXPECT_EQ(shape("items", json::object()), "Shapes.items is object, not JSON value[]");
  EXPECT_EQ(shape("color", json::value(json::string("green"))), R"(Color is string, not "red" | "blue")");
  EXPECT_EQ(shape("color", std::nullopt), "Shapes.color is undefined, not string");
  EXPECT_EQ(shape("inner", json::value(json::string("x"))), "Shapes.inner is string, not object");
  EXPECT_EQ(shape("inner", json::object()), "Inner.name is undefined, not string");
  EXPECT_EQ(shape("steps", rt::jsonParse(R"([{"type":"wait"}])")), R"(Step.type is string, not "hold" | "done")");
  EXPECT_EQ(shape("steps", rt::jsonParse("[7]")), "Shapes.steps[] is number, not object");
}

TEST("a member's own value_to throws only rt errors through value_to") {
  EXPECT_EQ(rejection<Wrapper>(R"({"legacy":{"code":"ok"},"legacies":[]})"), "<accepted>");
  // The reason after the member path is Boost's error_code message (Boost 1.81).
  EXPECT_EQ(rejection<Wrapper>(R"({"legacy":{"code":1},"legacies":[]})"), "Wrapper.legacy: value is not a string");
  EXPECT_EQ(rejection<Wrapper>(R"({"legacy":{"code":"ok"},"legacies":[{"code":"boom"}]})"), "Wrapper.legacies[]: boom");
  EXPECT_THROWS_MESSAGE(json::value_to<Wrapper>(rt::jsonParse(R"({"legacy":{"code":"deep"},"legacies":[]})")), rt::RangeError, "Maximum call stack size exceeded.");
}
