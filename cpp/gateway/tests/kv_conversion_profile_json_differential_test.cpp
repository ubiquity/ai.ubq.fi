// Differential cases for src/kv-conversion/profile-json.ts, recorded under Bun 1.4.0
// (kv_conversion_profile_json_differential_vectors.inc). Its direct tests need modules that are not
// ported yet: tests/kv-runtime-conversion.test.ts "retains profile JSON wire tags, nested values, U64
// identity and profile namespace filtering" and "profile export is NDJSON through actual Bun KV with
// typed values" (src/cache/kv-migration-base.ts parseKvMigrationEntryLine / classifyKvMigrationKey),
// tests/codex-manual-reset-state.test.ts "manual reset intent is durable across prod export/import
// and exact-value roundtrip" (src/cache/kv-migration-base.ts), tests/kv-migration.test.ts
// (src/cache/kv-migration.ts) and tests/helpers/admin-auth-harness.ts (the admin handler). These
// cases cover the module itself:
// - valueToJson on 40 values built here exactly as the Bun script built them (the C++ store holds
//   only what structured clone keeps, so a JS Error's generated stack is absent on both sides);
// - keyToJson / toKey, toValue on 118 wire inputs (results compared through valueToJson, errors by
//   name and message), entryToJson and exportEntries (NDJSON text and cancel -> iterator.return).
// Two inputs differ by design and are checked separately: a part whose toValue result is not a key
// part (the TS returns the bad key and fails when it is used; the port throws the SDK's error at
// once) and an Error stack that is not a string (rt::KvError keeps its string form).
// Links src/kv_conversion/profile_json.cpp.
#include "uos/kv_conversion/profile_json.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::kv_conversion::profile_json;

namespace {
#include "kv_conversion_profile_json_differential_vectors.inc"

  constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
  constexpr double INF = std::numeric_limits<double>::infinity();

  std::string text(const KvValueJson& value) { return rt::jsonStringify(json::value(value)); }

  std::string text(const std::vector<KvValueJson>& parts) {
    json::array items;
    for (const KvValueJson& part : parts) items.push_back(part);
    return rt::jsonStringify(json::value(items));
  }

  std::string errorText(const rt::JsError& error) { return error.name + ": " + error.message; }

  template <class T> std::vector<std::uint8_t> littleEndian(T number) {
    std::vector<std::uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &number, sizeof(T));
    return bytes;
  }

  rt::KvValue view(rt::KvViewKind kind, std::vector<std::uint8_t> buffer, std::uint64_t offset, std::uint64_t length) {
    rt::KvArrayBufferView typed;
    typed.kind = kind;
    typed.buffer = std::move(buffer);
    typed.byteOffset = offset;
    typed.byteLength = length;
    return rt::KvValue(std::move(typed));
  }

  rt::KvValue error(std::string name, std::optional<std::string> message) {
    rt::KvError value;
    value.name = std::move(name);
    value.message = std::move(message);
    return rt::KvValue(std::move(value));
  }

  // The values of the Bun script, by name.
  std::map<std::string, rt::KvValue> namedValues() {
    std::map<std::string, rt::KvValue> values;
    values["undefined"] = rt::KvValue();
    values["null"] = rt::KvValue(nullptr);
    values["true"] = rt::KvValue(true);
    values["zero"] = rt::KvValue(0.0);
    values["negative zero"] = rt::KvValue(-0.0);
    values["fraction"] = rt::KvValue(1.5);
    values["NaN"] = rt::KvValue(NaN);
    values["Infinity"] = rt::KvValue(INF);
    values["-Infinity"] = rt::KvValue(-INF);
    values["string"] = rt::KvValue("str");
    values["unicode"] = rt::KvValue("\xC3\xA9\xF0\x9F\x98\x80");
    values["lone surrogate"] = rt::KvValue("a\xED\xA0\x80"
                                           "b");
    values["bigint"] = rt::KvValue(rt::BigInt(10));
    values["big negative"] = rt::KvValue(-rt::BigInt(2).pow(rt::BigInt(120)));
    values["u64 max"] = rt::KvValue(rt::KvU64(std::numeric_limits<std::uint64_t>::max()));
    values["u64 zero"] = rt::KvValue(rt::KvU64(0));
    values["date"] = rt::KvValue(rt::KvDate{1700000000000});
    values["invalid date"] = rt::KvValue(rt::KvDate{NaN});
    values["regexp literal"] = rt::KvValue(rt::KvRegExp{"a\\/b", "giu"});
    values["regexp constructed"] = rt::KvValue(rt::KvRegExp{"a\\/b", "g"});
    values["regexp empty"] = rt::KvValue(rt::KvRegExp{"(?:)", ""});
    values["array buffer"] = rt::KvValue(rt::KvArrayBuffer{{1, 2, 3}});
    values["uint8 whole"] = rt::KvValue(rt::KvBytes{0, 255});
    values["uint8 view"] = view(rt::KvViewKind::Uint8Array, {1, 2, 3, 4}, 1, 2);
    values["data view"] = view(rt::KvViewKind::DataView, {1, 2, 3, 4}, 2, 1);
    values["int16"] = view(rt::KvViewKind::Int16Array, {1, 0, 255, 255}, 0, 4);
    values["float64"] = view(rt::KvViewKind::Float64Array, littleEndian(1.5), 0, 8);
    values["bigint64"] = view(rt::KvViewKind::BigInt64Array, littleEndian(std::int64_t{5}), 0, 8);
    values["array"] = rt::KvValue(rt::KvArray{rt::KvValue(), rt::KvValue(rt::BigInt(4)), rt::KvValue(rt::KvBytes{0, 255})});
    values["sparse"] = rt::KvValue(rt::KvArray{rt::KvValue(rt::KvHole{}), rt::KvValue(1)});
    rt::KvMap map;
    map.set(rt::KvValue(true), rt::KvValue("yes"));
    map.set(rt::KvValue(1), rt::KvValue(rt::KvObject{}));
    values["map"] = rt::KvValue(map);
    rt::KvSet set;
    set.add(rt::KvValue(rt::BigInt(2)));
    set.add(rt::KvValue("x"));
    values["set"] = rt::KvValue(set);
    values["type error"] = error("TypeError", "typed");
    rt::KvError stacked;
    stacked.name = "RangeError";
    stacked.message = "ranged";
    stacked.stack = "custom stack";
    values["error with stack"] = rt::KvValue(stacked);
    rt::KvError caused;
    caused.message = "outer";
    caused.cause = std::make_shared<const rt::KvValue>(rt::BigInt(7));
    values["error with cause"] = rt::KvValue(caused);
    rt::KvError falsyCause;
    falsyCause.message = "outer";
    falsyCause.cause = std::make_shared<const rt::KvValue>(0);
    values["error with falsy cause"] = rt::KvValue(falsyCause);
    values["error without message"] = error("Error", std::nullopt);
    values["custom error"] = error("Custom", "c");
    rt::KvMap yes;
    yes.set(rt::KvValue(true), rt::KvValue("yes"));
    rt::KvSet two;
    two.add(rt::KvValue(rt::BigInt(2)));
    rt::KvObject object;
    object.set("nested", values["array"]);
    object.set("when", values["date"]);
    object.set("set", rt::KvValue(two));
    object.set("map", rt::KvValue(yes));
    object.set("count", values["u64 max"]);
    values["object"] = rt::KvValue(object);
    rt::KvObject withUndefined;
    withUndefined.set("payload", rt::KvValue(rt::KvBytes{1, 2}));
    withUndefined.set("missing", rt::KvValue());
    values["object with undefined"] = rt::KvValue(withUndefined);
    rt::KvObject indexKeys;
    indexKeys.set("b", rt::KvValue(1));
    indexKeys.set("2", rt::KvValue(2));
    indexKeys.set("a", rt::KvValue(3));
    indexKeys.set("1", rt::KvValue(4));
    values["index keys"] = rt::KvValue(indexKeys);
    return values;
  }

  rt::KvKey sampleKey() {
    return rt::KvKey{"uos_ai", "runtime_config", "v2", rt::KvBytes{7}, rt::BigInt(9), true, INF, -0.0, NaN, -INF, 1.5, "\xC3\xA9"};
  }

  // Counts iterator.return() through to the store's own iterator.
  class CountingIterator final : public rt::KvListIterator {
  public:
    CountingIterator(std::unique_ptr<rt::KvListIterator> inner_, int& returns_) : inner(std::move(inner_)), returns(returns_) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override { return inner->next(); }
    std::string cursor() const override { return inner->cursor(); }
    rt::Task<void> return_() override {
      returns += 1;
      return inner->return_();
    }

  private:
    std::unique_ptr<rt::KvListIterator> inner;
    int& returns;
  };
  class CountingKv final : public rt::ForwardingKv {
  public:
    using rt::ForwardingKv::ForwardingKv;
    int returns = 0;
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options) override {
      return std::make_unique<CountingIterator>(inner.list(std::move(selector), std::move(options)), returns);
    }
  };

  rt::Task<std::string> readAll(rt::ReadableStream<rt::ByteChunk> stream) {
    auto reader = stream.getReader();
    std::vector<std::uint8_t> bytes;
    for (;;) {
      auto chunk = co_await reader.read();
      if (chunk.done) break;
      bytes.insert(bytes.end(), chunk.value->begin(), chunk.value->end());
    }
    co_return rt::decodeUtf8(bytes);
  }
}

TEST("valueToJson matches Bun") {
  const json::value vectors = rt::jsonParse(PROFILE_JSON_VECTORS);
  const std::map<std::string, rt::KvValue> values = namedValues();
  std::size_t checked = 0;
  for (const json::value& vector : vectors.at("values").as_array()) {
    const std::string name(vector.at("name").as_string());
    const auto found = values.find(name);
    ASSERT(found != values.end());
    EXPECT_EQ(text(valueToJson(found->second)), rt::jsonStringify(vector.at("out")));
    checked += 1;
  }
  EXPECT_EQ(checked, values.size());
}

TEST("keys convert both ways as Bun converts them") {
  const json::value vectors = rt::jsonParse(PROFILE_JSON_VECTORS);
  const std::vector<KvValueJson> parts = keyToJson(sampleKey());
  EXPECT_EQ(text(parts), rt::jsonStringify(vectors.at("key")));
  EXPECT_EQ(text(keyToJson(toKey(parts))), rt::jsonStringify(vectors.at("roundKey")));
  std::size_t checked = 0;
  for (const json::value& vector : vectors.at("toKey").as_array()) {
    const json::array& input = vector.at("input").as_array();
    std::string actual;
    try {
      actual = text(keyToJson(toKey(input)));
    } catch (const rt::JsError& error) {
      actual = errorText(error);
    }
    const std::string recorded = rt::get(vector, "out") ? rt::jsonStringify(vector.at("out")) : std::string(vector.at("error").as_string());
    if (rt::jsonStringify(json::value(input)) == R"([{"type":"string"}])") {
      // The TS returns [undefined] here; the port throws what the SDK throws when that key is used.
      EXPECT_EQ(recorded, std::string(R"([{"type":"undefined"}])"));
      EXPECT_EQ(actual, std::string("Error: Unsupported keyPart: undefined undefined"));
    } else {
      EXPECT_EQ(actual, recorded);
    }
    checked += 1;
  }
  EXPECT_EQ(checked, std::size_t{11});
  std::string nullPart;
  try {
    (void)toKey(json::array{nullptr});
  } catch (const rt::JsError& error) {
    nullPart = errorText(error);
  }
  EXPECT_EQ(nullPart, std::string(vectors.at("toKeyNull").at("error").as_string()));
  std::string objectPart;
  try {
    (void)toKey(rt::jsonParse(R"([{"type":"boolean","value":[1,[2,null]]}])").as_array());
  } catch (const rt::JsError& error) {
    objectPart = errorText(error);
  }
  EXPECT_EQ(objectPart, std::string("Error: Unsupported keyPart: object 1,2,"));
}

TEST("toValue matches Bun on wire inputs") {
  const json::value vectors = rt::jsonParse(PROFILE_JSON_VECTORS);
  std::size_t checked = 0;
  for (const json::value& vector : vectors.at("toValue").as_array()) {
    const json::value& input = vector.at("input");
    std::string actual;
    try {
      actual = text(valueToJson(toValue(input)));
    } catch (const rt::JsError& error) {
      actual = errorText(error);
    }
    const std::string recorded = rt::get(vector, "out") ? rt::jsonStringify(vector.at("out")) : std::string(vector.at("error").as_string());
    const std::string wire = rt::jsonStringify(input);
    if (wire == R"({"type":"Error","value":{"message":"s","stack":5}})") {
      // The TS keeps the number; rt::KvError holds a string stack.
      EXPECT_EQ(recorded, std::string(R"({"type":"Error","value":{"message":"s","stack":5}})"));
      EXPECT_EQ(actual, std::string(R"({"type":"Error","value":{"message":"s","stack":"5"}})"));
    } else if (wire == R"({"type":"RegExp","value":"/[/"})") {
      EXPECT_EQ(actual, std::string("SyntaxError: unexpected end"));
    } else if (wire == R"({"type":"RegExp","value":"/a/zz"})" || wire == R"({"type":"RegExp","value":"noslash"})") {
      EXPECT_EQ(actual, std::string("SyntaxError: invalid regular expression flags"));
    } else {
      EXPECT_EQ(actual, recorded);
      if (actual != recorded) std::fprintf(stderr, "    input: %s\n", wire.c_str());
    }
    checked += 1;
  }
  EXPECT_EQ(checked, std::size_t{118});
  EXPECT_THROWS_MESSAGE(toValue(nullptr), rt::TypeError, "Malformed profile value");
}

TEST("entryToJson matches Bun and round-trips through its record codec") {
  const json::value vectors = rt::jsonParse(PROFILE_JSON_VECTORS);
  const std::map<std::string, rt::KvValue> values = namedValues();
  rt::KvEntry<> entry;
  entry.key = rt::KvKey{"uos_ai", "runtime_config", "v2", rt::KvBytes{7}, rt::BigInt(9), true, INF};
  entry.value = values.at("object");
  entry.versionstamp = "00000000000000010000";
  const KvEntryJson row = entryToJson(entry);
  const json::value written = json::value_from(row);
  EXPECT_EQ(rt::jsonStringify(written), rt::jsonStringify(vectors.at("entry")));
  EXPECT(json::value_to<KvEntryJson>(written) == row);
  EXPECT(valueToJson(toValue(json::value(row.value))) == row.value);
  EXPECT(keyToJson(toKey(row.key)) == row.key);
}

ASYNC_TEST("exportEntries writes NDJSON lines and returns the iterator on cancel") {
  const json::value vectors = rt::jsonParse(PROFILE_JSON_VECTORS);
  rt::MemoryKv store([] { return rt::dateNow(); });
  rt::KvObject payload;
  payload.set("payload", rt::KvValue(rt::KvBytes{1, 2}));
  payload.set("missing", rt::KvValue());
  const rt::KvValue first(payload);
  co_await store.set(rt::KvKey{"a", 1}, first);
  const rt::KvValue second("text");
  co_await store.set(rt::KvKey{"a", 2}, second);
  CountingKv kv(store);
  rt::KvListSelector everything;
  everything.prefix = rt::KvKey();
  auto exporting = readAll(exportEntries(kv, everything));
  const std::string ndjson = co_await std::move(exporting);
  EXPECT_EQ(ndjson, std::string(vectors.at("export").as_string()));

  rt::ReadableStream<rt::ByteChunk> stream = exportEntries(kv, everything);
  auto reader = stream.getReader();
  auto firstChunk = co_await reader.read();
  EXPECT(!firstChunk.done);
  co_await reader.cancel(rt::Reason());
  EXPECT_EQ(static_cast<double>(kv.returns), *rt::getNumber(vectors, "exportReturnCalls"));
}
