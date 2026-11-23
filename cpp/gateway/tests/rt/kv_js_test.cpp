// Tests for uos/rt/kv_js.hpp: typeof, the record guards, the JSON view, JSON.stringify,
// String(), instanceof Uint8Array, === and the key-part conversions on stored values. Every JS
// fact was recorded with bun 1.4.0 on values read back from the store (node:v8
// deserialize(serialize(v)); a top-level KvU64 or Uint8Array stored raw), TZ=UTC: typeof /
// Array.isArray / isRecord / Object.keys / member reads per kind, `Unsupported keyPart: ...`
// errors and encodings from @deno/kv 0.14's packKeyPart (with the repository's number patch),
// flatMap skipping holes, String() and JSON.stringify
// output (texts quoted next to each case).
// Links: tests/rt/kv_js_test.cpp (every src/rt/*.cpp links automatically).
#include "uos/rt/kv_js.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;

namespace {
  using rt::KvValue;
  using Options = rt::StoredJsonViewOptions;

  // A typed array of `kind` over `values` (little-endian, as on the host), viewing `count`
  // elements from element `offset` (count -1: to the end).
  template <class T> KvValue typed(rt::KvViewKind kind, std::vector<T> values, std::size_t offset = 0, std::size_t count = static_cast<std::size_t>(-1)) {
    rt::KvArrayBufferView view;
    view.kind = kind;
    view.buffer.resize(values.size() * sizeof(T));
    if (!values.empty()) std::memcpy(view.buffer.data(), values.data(), view.buffer.size());
    if (count == static_cast<std::size_t>(-1)) count = values.size() - offset;
    view.byteOffset = offset * sizeof(T);
    view.byteLength = count * sizeof(T);
    return KvValue(std::move(view));
  }

  KvValue dataView(std::size_t bytes) {
    rt::KvArrayBufferView view;
    view.kind = rt::KvViewKind::DataView;
    view.buffer.assign(bytes, 0);
    view.byteLength = bytes;
    return KvValue(std::move(view));
  }

  KvValue mapOf(KvValue key, KvValue value) {
    rt::KvMap map;
    map.set(std::move(key), std::move(value));
    return KvValue(std::move(map));
  }

  KvValue setOf(KvValue item) {
    rt::KvSet set;
    set.add(std::move(item));
    return KvValue(std::move(set));
  }

  KvValue error(std::string name, std::optional<std::string> message) {
    rt::KvError value;
    value.name = std::move(name);
    value.message = std::move(message);
    return KvValue(std::move(value));
  }

  KvValue regexp(std::string source, std::string flags) { return KvValue(rt::KvRegExp{std::move(source), std::move(flags)}); }

  const double NaN = std::numeric_limits<double>::quiet_NaN();
  const double Infinity = std::numeric_limits<double>::infinity();

  std::string thrown(const std::function<void()>& run) {
    try {
      run();
    } catch (const rt::JsError& error) {
      return error.toString();
    }
    return "<no throw>";
  }

  // Object.keys of a JSON object view.
  std::vector<std::string> keysOf(const json::value& value) { return rt::objectKeys(value.as_object()); }
}

TEST("kvTypeof, kvIsObject and kvIsRecord follow typeof on stored values") {
  struct Row {
    KvValue value;
    std::string_view type;
    bool object;
    bool record;
  };
  const std::vector<Row> rows = {
    {KvValue(), "undefined", false, false},
    {KvValue(rt::KvHole{}), "undefined", false, false},
    {KvValue(nullptr), "object", false, false},
    {KvValue(true), "boolean", false, false},
    {KvValue(NaN), "number", false, false},
    {KvValue("s"), "string", false, false},
    {KvValue(rt::BigInt(0)), "bigint", false, false},
    {KvValue(rt::KvU64(5)), "object", true, true},
    {KvValue(rt::KvBytes{1, 2}), "object", true, true},
    {typed<std::int16_t>(rt::KvViewKind::Int16Array, {1, -2}), "object", true, true},
    {dataView(2), "object", true, true},
    {KvValue(rt::KvArrayBuffer{{0, 0}}), "object", true, true},
    {KvValue(rt::KvDate{0}), "object", true, true},
    {regexp("a+", "gi"), "object", true, true},
    {error("Error", "boom"), "object", true, true},
    {mapOf(KvValue("a"), KvValue(1)), "object", true, true},
    {setOf(KvValue(1)), "object", true, true},
    {KvValue::object({{"a", KvValue(1)}}), "object", true, true},
    {KvValue::array({KvValue(1)}), "object", true, false},
  };
  for (const Row& row : rows) {
    EXPECT_EQ(rt::kvTypeof(row.value), row.type);
    EXPECT_EQ(rt::kvIsObject(row.value), row.object);
    EXPECT_EQ(rt::kvIsRecord(row.value), row.record);
  }
}

TEST("storedJsonView: the default view is what JS guards observe") {
  // { a: undefined, b: [undefined, <hole>, 1], c: 5n, d: new Date(0), e: new Map([["a", 1]]),
  //   f: new Set([1]), g: /x/g, h: new TypeError("t"), i: new Uint8Array([1, 2]),
  //   j: new Int16Array([9, 1, -2]).subarray(1), k: new BigInt64Array([3n]), l: new DataView(...),
  //   m: new ArrayBuffer(2), n: new KvU64(7n), o: NaN, p: -0, q: Infinity, r: { 2: "two", x: null } }
  rt::KvObject stored;
  stored.set("a", KvValue());
  stored.set("b", KvValue::array({KvValue(), KvValue(rt::KvHole{}), KvValue(1)}));
  stored.set("c", KvValue(rt::BigInt(5)));
  stored.set("d", KvValue(rt::KvDate{0}));
  stored.set("e", mapOf(KvValue("a"), KvValue(1)));
  stored.set("f", setOf(KvValue(1)));
  stored.set("g", regexp("x", "g"));
  stored.set("h", error("TypeError", "t"));
  stored.set("i", KvValue(rt::KvBytes{1, 2}));
  stored.set("j", typed<std::int16_t>(rt::KvViewKind::Int16Array, {9, 1, -2}, 1));
  stored.set("k", typed<std::int64_t>(rt::KvViewKind::BigInt64Array, {3}));
  stored.set("l", dataView(2));
  stored.set("m", KvValue(rt::KvArrayBuffer{{0, 0}}));
  stored.set("n", KvValue(rt::KvU64(7)));
  stored.set("o", KvValue(NaN));
  stored.set("p", KvValue(-0.0));
  stored.set("q", KvValue(Infinity));
  stored.set("r", KvValue::object({{"x", KvValue(nullptr)}, {"2", KvValue("two")}}));
  const json::value view = rt::storedJsonView(KvValue(stored));
  const json::value expected = rt::jsonParse(
    R"({"b":[null,null,1],"c":{},"d":{},"e":{},"f":{},"g":{},"h":{},"i":{"0":1,"1":2},"j":{"0":1,"1":-2},"k":{"0":{}},"l":{},"m":{},"n":{"value":{}},"o":0,"p":0,"q":0,"r":{"2":"two","x":null}})");
  // Members in JS order; `a` (undefined) is absent.
  EXPECT_EQ(keysOf(view), keysOf(expected));
  for (const std::string& key : keysOf(expected)) {
    if (key == "o" || key == "p" || key == "q") continue;
    EXPECT(rt::deepEqual(*rt::get(view, key), *rt::get(expected, key)));
  }
  // NaN, -0 and Infinity stay numbers: typeof "number", Number.isFinite false/true/false.
  EXPECT(std::isnan(*rt::getNumber(view, "o")));
  EXPECT(rt::sameValue(*rt::getNumber(view, "p"), -0.0));
  EXPECT_EQ(*rt::getNumber(view, "q"), Infinity);
  EXPECT(!rt::Number::isFinite(*rt::get(view, "o")) && rt::Number::isFinite(*rt::get(view, "p")));
  // The guards' observations (bun): stored.a === undefined; typeof stored.c !== "number" and
  // isRecord(5n) is false only in JS (a {} view is a record: the documented bigint default);
  // isRecord(date) / map / error / u8 true; Object.keys(u8) ["0","1"]; Object.keys(date) [];
  // Object.keys(kvU64) ["value"]; Number.isFinite(stored.c) false.
  EXPECT(rt::get(view, "a") == nullptr);
  EXPECT(!rt::isNumber(*rt::get(view, "c")) && !rt::get(view, "c")->is_string() && !rt::get(view, "c")->is_null());
  for (const char* key : {"d", "e", "f", "g", "h", "i", "j", "l", "m", "n"}) EXPECT(rt::isRecord(rt::get(view, key)));
  EXPECT_EQ(keysOf(*rt::get(view, "i")), (std::vector<std::string>{"0", "1"}));
  EXPECT(keysOf(*rt::get(view, "d")).empty());
  EXPECT_EQ(keysOf(*rt::get(view, "n")), (std::vector<std::string>{"value"}));
  // A stored undefined, an array hole at the top and a top-level bigint.
  EXPECT(rt::storedJsonView(KvValue()).is_null());
  EXPECT(rt::storedJsonView(KvValue(rt::KvHole{})).is_null());
  EXPECT(rt::deepEqual(rt::storedJsonView(KvValue(rt::BigInt(5))), json::object()));
  // Strings and nested containers are kept; the view is canonical (int64 for 7).
  EXPECT(rt::deepEqual(rt::storedJsonView(KvValue::array({KvValue("s"), KvValue::array({KvValue(7)})})), rt::jsonParse(R"(["s",[7]])")));
  EXPECT(rt::storedJsonView(KvValue(7)).is_int64());
}

TEST("storedJsonView options reproduce the earlier local helpers") {
  // { a: undefined, b: [undefined, <hole>], c: 5n, n: new KvU64(7n), i: new Uint8Array([1]) }
  rt::KvObject stored;
  stored.set("a", KvValue());
  stored.set("b", KvValue::array({KvValue(), KvValue(rt::KvHole{})}));
  stored.set("c", KvValue(rt::BigInt(5)));
  stored.set("n", KvValue(rt::KvU64(7)));
  stored.set("i", KvValue(rt::KvBytes{1}));
  const KvValue value(stored);
  using U = Options::UndefinedView;
  // quota_v2::kernelQuotaValue and the b1 storedView copies (top-level undefined aside).
  const Options dropping{.undefinedMember = U::omit, .undefinedElement = U::null, .hole = U::null, .topLevelUndefined = U::emptyObject, .kvU64Members = false, .typedArrayMembers = false};
  EXPECT(rt::deepEqual(rt::storedJsonView(value, dropping), rt::jsonParse(R"({"b":[null,null],"c":{},"n":{},"i":{}})")));
  EXPECT(rt::deepEqual(rt::storedJsonView(KvValue(), dropping), json::object()));
  // runtime_config.cpp / rollups.cpp storedView.
  const Options keeping{.undefinedMember = U::emptyObject, .undefinedElement = U::emptyObject, .hole = U::emptyObject, .topLevelUndefined = U::emptyObject, .kvU64Members = false, .typedArrayMembers = false};
  EXPECT(rt::deepEqual(rt::storedJsonView(value, keeping), rt::jsonParse(R"({"a":{},"b":[{},{}],"c":{},"n":{},"i":{}})")));
  // metered_quota.cpp storedView.
  const Options metered{.undefinedMember = U::omit, .undefinedElement = U::null, .hole = U::emptyObject, .topLevelUndefined = U::null, .kvU64Members = false, .typedArrayMembers = false};
  EXPECT(rt::deepEqual(rt::storedJsonView(value, metered), rt::jsonParse(R"({"b":[null,{}],"c":{},"n":{},"i":{}})")));
  EXPECT(rt::storedJsonView(KvValue(), metered).is_null());
  // undefined members as null; bigint as text, as a number, as null.
  const Options nulls{.undefinedMember = U::null};
  EXPECT(rt::deepEqual(rt::storedJsonView(value, nulls), rt::jsonParse(R"({"a":null,"b":[null,null],"c":{},"n":{"value":{}},"i":{"0":1}})")));
  const json::value text = rt::storedJsonView(KvValue::array({KvValue(rt::BigInt(-12)), KvValue(rt::KvU64(18446744073709551615ULL))}), Options{.bigint = Options::BigIntView::decimalString});
  EXPECT(rt::deepEqual(text, rt::jsonParse(R"(["-12",{"value":"18446744073709551615"}])")));
  const json::value number = rt::storedJsonView(KvValue(rt::BigInt(5)), Options{.bigint = Options::BigIntView::number});
  EXPECT(number.is_int64() && number.get_int64() == 5);
  EXPECT(rt::storedJsonView(KvValue(rt::BigInt(5)), Options{.bigint = Options::BigIntView::null}).is_null());
  EXPECT(rt::deepEqual(rt::storedJsonView(typed<std::uint64_t>(rt::KvViewKind::BigUint64Array, {1}), Options{.bigint = Options::BigIntView::decimalString}), rt::jsonParse(R"({"0":"1"})")));
}

TEST("kvJsonStringify is JSON.stringify of the stored value") {
  // JSON.stringify({ a: [undefined, , new Date(0), new Map([[1, 2]])], b: undefined, c: -0, d: NaN,
  //   e: new Date(NaN), f: /x/, g: new Error("m"), h: new ArrayBuffer(2), i: new DataView(new ArrayBuffer(2)) })
  rt::KvObject nested;
  nested.set("a", KvValue::array({KvValue(), KvValue(rt::KvHole{}), KvValue(rt::KvDate{0}), mapOf(KvValue(1), KvValue(2))}));
  nested.set("b", KvValue());
  nested.set("c", KvValue(-0.0));
  nested.set("d", KvValue(NaN));
  nested.set("e", KvValue(rt::KvDate{NaN}));
  nested.set("f", regexp("x", ""));
  nested.set("g", error("Error", "m"));
  nested.set("h", KvValue(rt::KvArrayBuffer{{0, 0}}));
  nested.set("i", dataView(2));
  EXPECT_EQ(rt::kvJsonStringify(KvValue(nested)), std::optional<std::string>(R"({"a":[null,null,"1970-01-01T00:00:00.000Z",{}],"c":0,"d":null,"e":null,"f":{},"g":{},"h":{},"i":{}})"));
  // new Uint8Array([9, 0, 1, 9]).subarray(1, 3) -> {"0":0,"1":1}
  EXPECT_EQ(rt::kvJsonStringify(typed<std::uint8_t>(rt::KvViewKind::Uint8Array, {9, 0, 1, 9}, 1, 2)), std::optional<std::string>(R"({"0":0,"1":1})"));
  // new Float32Array([1.1, NaN, -0]) -> {"0":1.100000023841858,"1":null,"2":0}
  EXPECT_EQ(rt::kvJsonStringify(typed<float>(rt::KvViewKind::Float32Array, {1.1f, std::numeric_limits<float>::quiet_NaN(), -0.0f})),
            std::optional<std::string>(R"({"0":1.100000023841858,"1":null,"2":0})"));
  EXPECT_EQ(rt::kvJsonStringify(typed<std::uint16_t>(rt::KvViewKind::Uint16Array, {65535})), std::optional<std::string>(R"({"0":65535})"));
  EXPECT_EQ(rt::kvJsonStringify(typed<std::int32_t>(rt::KvViewKind::Int32Array, {1, 2, 3}, 1)), std::optional<std::string>(R"({"0":2,"1":3})"));
  EXPECT_EQ(rt::kvJsonStringify(typed<std::int64_t>(rt::KvViewKind::BigInt64Array, {})), std::optional<std::string>("{}"));
  EXPECT_EQ(rt::kvJsonStringify(KvValue(rt::KvBytes{7, 8})), std::optional<std::string>(R"({"0":7,"1":8})"));
  // BigInt anywhere throws, a KvU64 too (its value is a bigint).
  EXPECT_EQ(thrown([] { (void)rt::kvJsonStringify(KvValue(rt::BigInt(1))); }), "TypeError: JSON.stringify cannot serialize BigInt.");
  EXPECT_EQ(thrown([] { (void)rt::kvJsonStringify(KvValue(rt::KvU64(1))); }), "TypeError: JSON.stringify cannot serialize BigInt.");
  EXPECT_EQ(thrown([] { (void)rt::kvJsonStringify(KvValue::object({{"u", KvValue::object({{"value", KvValue(rt::BigInt(1))}})}})); }), "TypeError: JSON.stringify cannot serialize BigInt.");
  EXPECT_EQ(thrown([] { (void)rt::kvJsonStringify(typed<std::int64_t>(rt::KvViewKind::BigInt64Array, {1})); }), "TypeError: JSON.stringify cannot serialize BigInt.");
  // undefined -> undefined; a stored `toJSON` string member is an ordinary member.
  EXPECT_EQ(rt::kvJsonStringify(KvValue()), std::nullopt);
  EXPECT_EQ(rt::kvJsonStringify(KvValue::object({{"toJSON", KvValue("x")}, {"a", KvValue(1)}})), std::optional<std::string>(R"({"toJSON":"x","a":1})"));
  // { b: 1, 2: "two", a: 2, 1: "one" } -> {"1":"one","2":"two","b":1,"a":2}
  rt::KvObject ordered;
  ordered.set("b", KvValue(1));
  ordered.set("2", KvValue("two"));
  ordered.set("a", KvValue(2));
  ordered.set("1", KvValue("one"));
  EXPECT_EQ(rt::kvJsonStringify(KvValue(ordered)), std::optional<std::string>(R"({"1":"one","2":"two","b":1,"a":2})"));
  // JSON.stringify({ a: [1, { b: undefined, c: new Uint8Array([7]) }], d: "x" }, null, 2)
  const KvValue spaced = KvValue::object({{"a", KvValue::array({KvValue(1), KvValue::object({{"b", KvValue()}, {"c", KvValue(rt::KvBytes{7})}})})}, {"d", KvValue("x")}});
  EXPECT_EQ(rt::kvJsonStringify(spaced, 2), std::optional<std::string>("{\n  \"a\": [\n    1,\n    {\n      \"c\": {\n        \"0\": 7\n      }\n    }\n  ],\n  \"d\": \"x\"\n}"));
  // JSON.stringify([new Date(86400000), []], null, "\t")
  EXPECT_EQ(rt::kvJsonStringify(KvValue::array({KvValue(rt::KvDate{86400000}), KvValue::array({})}), std::string_view("\t")),
            std::optional<std::string>("[\n\t\"1970-01-02T00:00:00.000Z\",\n\t[]\n]"));
}

TEST("kvToString is String() of the stored value") {
  EXPECT_EQ(rt::kvToString(KvValue()), "undefined");
  EXPECT_EQ(rt::kvToString(KvValue(nullptr)), "null");
  EXPECT_EQ(rt::kvToString(KvValue(false)), "false");
  EXPECT_EQ(rt::kvToString(KvValue(NaN)), "NaN");
  EXPECT_EQ(rt::kvToString(KvValue(-0.0)), "0");
  EXPECT_EQ(rt::kvToString(KvValue(1e21)), "1e+21");
  EXPECT_EQ(rt::kvToString(KvValue(rt::BigInt(-12))), "-12");
  EXPECT_EQ(rt::kvToString(KvValue(rt::KvDate{0})), "Thu Jan 01 1970 00:00:00 GMT+0000 (Coordinated Universal Time)");
  EXPECT_EQ(rt::kvToString(KvValue(rt::KvDate{NaN})), "Invalid Date");
  EXPECT_EQ(rt::kvToString(mapOf(KvValue(1), KvValue(2))), "[object Map]");
  EXPECT_EQ(rt::kvToString(setOf(KvValue(1))), "[object Set]");
  EXPECT_EQ(rt::kvToString(regexp("a\\/b", "gi")), "/a\\/b/gi");
  EXPECT_EQ(rt::kvToString(error("TypeError", "bad")), "TypeError: bad");
  EXPECT_EQ(rt::kvToString(error("Error", std::nullopt)), "Error");
  EXPECT_EQ(rt::kvToString(KvValue(rt::KvU64(7))), "[object Object]");
  EXPECT_EQ(rt::kvToString(typed<std::int8_t>(rt::KvViewKind::Int8Array, {1, -2})), "1,-2");
  EXPECT_EQ(rt::kvToString(typed<std::uint8_t>(rt::KvViewKind::Uint8ClampedArray, {3})), "3");
  EXPECT_EQ(rt::kvToString(typed<float>(rt::KvViewKind::Float32Array, {1.1f, std::numeric_limits<float>::quiet_NaN()})), "1.100000023841858,NaN");
  EXPECT_EQ(rt::kvToString(typed<std::int64_t>(rt::KvViewKind::BigInt64Array, {-5})), "-5");
  EXPECT_EQ(rt::kvToString(dataView(1)), "[object DataView]");
  EXPECT_EQ(rt::kvToString(KvValue(rt::KvArrayBuffer{{0}})), "[object ArrayBuffer]");
  EXPECT_EQ(rt::kvToString(KvValue(rt::KvBytes{1, 2})), "1,2");
  // String([1, [2, null], undefined]) and String([null, undefined, , [null], [[]], 1.5, -0, 5n, "s", { a: 1 }])
  EXPECT_EQ(rt::kvToString(KvValue::array({KvValue(1), KvValue::array({KvValue(2), KvValue(nullptr)}), KvValue()})), "1,2,,");
  EXPECT_EQ(rt::kvToString(KvValue::array({KvValue(nullptr), KvValue(), KvValue(rt::KvHole{}), KvValue::array({KvValue(nullptr)}), KvValue::array({KvValue::array({})}), KvValue(1.5),
                                           KvValue(-0.0), KvValue(rt::BigInt(5)), KvValue("s"), KvValue::object({{"a", KvValue(1)}})})),
            ",,,,,1.5,0,5,s,[object Object]");
  EXPECT_EQ(rt::kvToString(KvValue::object({{"a", KvValue(1)}})), "[object Object]");
  // String({ toString: "x" }) and String([1, { toString: "x" }]): TypeError "No default value".
  EXPECT_EQ(thrown([] { (void)rt::kvToString(KvValue::object({{"toString", KvValue("x")}})); }), "TypeError: No default value");
  EXPECT_EQ(thrown([] { (void)rt::kvToString(KvValue::array({KvValue(1), KvValue::object({{"toString", KvValue("x")}})})); }), "TypeError: No default value");
  // String({ valueOf: 1 }) is "[object Object]".
  EXPECT_EQ(rt::kvToString(KvValue::object({{"valueOf", KvValue(1)}})), "[object Object]");
}

TEST("instanceof Uint8Array and the bytes of a view") {
  EXPECT(rt::kvIsUint8Array(KvValue(rt::KvBytes{1})));
  const KvValue view = typed<std::uint8_t>(rt::KvViewKind::Uint8Array, {9, 0, 1, 9}, 1, 2);
  EXPECT(rt::kvIsUint8Array(view));
  ASSERT(rt::kvUint8ArrayBytes(view).has_value());
  const std::span<const std::uint8_t> bytes = *rt::kvUint8ArrayBytes(view);
  EXPECT_EQ(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), (std::vector<std::uint8_t>{0, 1}));
  EXPECT(!rt::kvIsUint8Array(typed<std::uint8_t>(rt::KvViewKind::Uint8ClampedArray, {1})));
  EXPECT(!rt::kvIsUint8Array(typed<std::int8_t>(rt::KvViewKind::Int8Array, {1})));
  EXPECT(!rt::kvIsUint8Array(dataView(1)));
  EXPECT(!rt::kvIsUint8Array(KvValue(rt::KvArrayBuffer{{1}})));
  EXPECT(!rt::kvIsUint8Array(KvValue::array({KvValue(1)})));
  EXPECT_EQ(rt::kvUint8ArrayBytes(KvValue("x")), std::nullopt);
}

TEST("key parts from stored values encode as the SDK encodes them, and fail with its error") {
  const auto keyPartError = [](const KvValue& value) { return thrown([&] { (void)rt::toKvKeyPart(value); }); };
  EXPECT_EQ(keyPartError(KvValue()), "Error: Unsupported keyPart: undefined undefined");
  EXPECT_EQ(keyPartError(KvValue(nullptr)), "Error: Unsupported keyPart: object null");
  EXPECT_EQ(keyPartError(KvValue::object({{"a", KvValue(1)}})), "Error: Unsupported keyPart: object [object Object]");
  EXPECT_EQ(keyPartError(KvValue::array({KvValue(1), KvValue::array({KvValue(2), KvValue(nullptr)}), KvValue()})), "Error: Unsupported keyPart: object 1,2,,");
  EXPECT_EQ(keyPartError(KvValue(rt::KvDate{0})), "Error: Unsupported keyPart: object Thu Jan 01 1970 00:00:00 GMT+0000 (Coordinated Universal Time)");
  EXPECT_EQ(keyPartError(KvValue(rt::KvDate{NaN})), "Error: Unsupported keyPart: object Invalid Date");
  EXPECT_EQ(keyPartError(KvValue(rt::KvMap{})), "Error: Unsupported keyPart: object [object Map]");
  EXPECT_EQ(keyPartError(KvValue(rt::KvSet{})), "Error: Unsupported keyPart: object [object Set]");
  EXPECT_EQ(keyPartError(regexp("a\\/b", "gi")), "Error: Unsupported keyPart: object /a\\/b/gi");
  EXPECT_EQ(keyPartError(error("TypeError", "bad")), "Error: Unsupported keyPart: object TypeError: bad");
  EXPECT_EQ(keyPartError(error("Error", std::nullopt)), "Error: Unsupported keyPart: object Error");
  EXPECT_EQ(keyPartError(KvValue(rt::KvU64(7))), "Error: Unsupported keyPart: object [object Object]");
  EXPECT_EQ(keyPartError(typed<std::int8_t>(rt::KvViewKind::Int8Array, {1, -2})), "Error: Unsupported keyPart: object 1,-2");
  EXPECT_EQ(keyPartError(typed<std::uint8_t>(rt::KvViewKind::Uint8ClampedArray, {3})), "Error: Unsupported keyPart: object 3");
  EXPECT_EQ(keyPartError(typed<float>(rt::KvViewKind::Float32Array, {1.1f, std::numeric_limits<float>::quiet_NaN()})), "Error: Unsupported keyPart: object 1.100000023841858,NaN");
  EXPECT_EQ(keyPartError(typed<std::int64_t>(rt::KvViewKind::BigInt64Array, {-5})), "Error: Unsupported keyPart: object -5");
  EXPECT_EQ(keyPartError(dataView(1)), "Error: Unsupported keyPart: object [object DataView]");
  EXPECT_EQ(keyPartError(KvValue(rt::KvArrayBuffer{{0}})), "Error: Unsupported keyPart: object [object ArrayBuffer]");
  EXPECT_EQ(rt::kvKeyPartFromValue(KvValue(nullptr)), std::nullopt);
  // packKeyPart of the parts (the SDK with the repository's number patch,
  // src/storage/patches/deno-kv-0.14.0.patch): a Uint8Array view packs the bytes it covers.
  EXPECT_EQ(rt::packKeyPart(rt::toKvKeyPart(typed<std::uint8_t>(rt::KvViewKind::Uint8Array, {9, 0, 1, 9}, 1, 2))), (rt::KvBytes{1, 0, 255, 1, 0}));
  EXPECT_EQ(rt::packKeyPart(rt::toKvKeyPart(KvValue(NaN))), (rt::KvBytes{33, 255, 248, 0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(rt::packKeyPart(rt::toKvKeyPart(KvValue(-0.0))), (rt::KvBytes{33, 127, 255, 255, 255, 255, 255, 255, 255}));
  EXPECT_EQ(rt::packKeyPart(rt::toKvKeyPart(KvValue(0))), (rt::KvBytes{33, 128, 0, 0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(rt::packKeyPart(rt::toKvKeyPart(KvValue(rt::BigInt(-12)))), (rt::KvBytes{19, 243}));
  EXPECT_EQ(rt::packKeyPart(rt::toKvKeyPart(KvValue(1e21))), (rt::KvBytes{33, 196, 75, 26, 228, 214, 226, 239, 80}));
  EXPECT_EQ(rt::toKvKeyPart(KvValue("s")), rt::KvKeyPart("s"));
  EXPECT_EQ(rt::toKvKeyPart(KvValue(true)), rt::KvKeyPart(true));
  // packKey([, "a"]) skips the hole (flatMap); packKey([undefined, "a"]) throws.
  EXPECT_EQ(rt::packKey(rt::kvKeyFromValue(KvValue::array({KvValue(rt::KvHole{}), KvValue("a")}))), (rt::KvBytes{2, 97, 0}));
  EXPECT_EQ(thrown([] { (void)rt::kvKeyFromValue(KvValue::array({KvValue(), KvValue("a")})); }), "Error: Unsupported keyPart: undefined undefined");
  EXPECT_EQ(thrown([] { (void)rt::kvKeyFromValue(KvValue("a")); }), "TypeError: KV value is string, not Array");
  // Keys round-trip through their stored arrays.
  const rt::KvKey key{"a", 1, rt::BigInt(2), true, rt::KvBytes{3}};
  const KvValue stored = rt::kvValueFromKey(key);
  EXPECT_EQ(stored, KvValue::array({KvValue("a"), KvValue(1), KvValue(rt::BigInt(2)), KvValue(true), KvValue(rt::KvBytes{3})}));
  EXPECT_EQ(rt::kvKeyFromValue(stored), key);
  EXPECT_EQ(rt::kvValueFromKeyPart(rt::KvKeyPart(-0.0)), KvValue(-0.0));
}

TEST("=== on key parts and keys") {
  const rt::KvKeyPart bytes(rt::KvBytes{1});
  const rt::KvKeyPart sameBytes(rt::KvBytes{1});
  EXPECT(rt::kvKeyPartStrictEquals(rt::KvKeyPart("a"), rt::KvKeyPart("a")));
  EXPECT(!rt::kvKeyPartStrictEquals(rt::KvKeyPart("1"), rt::KvKeyPart(1)));
  EXPECT(rt::kvKeyPartStrictEquals(rt::KvKeyPart(1), rt::KvKeyPart(1.0)));
  EXPECT(rt::kvKeyPartStrictEquals(rt::KvKeyPart(0.0), rt::KvKeyPart(-0.0)));
  EXPECT(!rt::kvKeyPartStrictEquals(rt::KvKeyPart(NaN), rt::KvKeyPart(NaN)));
  EXPECT(rt::kvKeyPartStrictEquals(rt::KvKeyPart(rt::BigInt(1)), rt::KvKeyPart(rt::BigInt(1))));
  EXPECT(!rt::kvKeyPartStrictEquals(rt::KvKeyPart(rt::BigInt(1)), rt::KvKeyPart(1)));
  EXPECT(rt::kvKeyPartStrictEquals(rt::KvKeyPart(true), rt::KvKeyPart(true)));
  EXPECT(rt::kvKeyPartStrictEquals(bytes, bytes));
  EXPECT(!rt::kvKeyPartStrictEquals(bytes, sameBytes));
  EXPECT(rt::kvKeyStrictEquals(rt::KvKey{"a", 1}, rt::KvKey{"a", 1.0}));
  EXPECT(!rt::kvKeyStrictEquals(rt::KvKey{"a", 1}, rt::KvKey{"a"}));
  EXPECT(!rt::kvKeyStrictEquals(rt::KvKey{"a", NaN}, rt::KvKey{"a", NaN}));
}
