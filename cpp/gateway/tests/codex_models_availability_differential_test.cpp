// Differential cases for the pure functions of src/models/codex-models-availability.ts: the
// defensive parse of stored values (JS property order with array-index account ids, arrays and
// typed arrays read through Object.entries, "__proto__" keys), the bounded rejection map (sort by
// newest then localeCompare, NaN and Infinity observations) and the catalog merge. The TS results
// were recorded with JSON.stringify under Bun 1.4.0 (tests/codex_models_availability_differential_vectors.inc).
// Links: tests/codex_models_availability_differential_test.cpp src/models/codex_models_availability.cpp src/utils.cpp src/kv.cpp src/storage/kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp src/storage/value_compatibility.cpp
//        src/storage/key_compatibility.cpp src/authority/contracts.cpp
#include "uos/models/codex_models_availability.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "codex_models_availability_differential_vectors.inc"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace availability = uos::models::codex_models_availability;
  using availability::CodexAccountCatalogInput;
  using availability::CodexAccountModelsStore;

  const json::object& recorded() {
    static const json::object value = rt::jsonParse(AVAILABILITY_RECORDED).as_object();
    return value;
  }

  std::string expected(std::string_view group, std::string_view name) { return std::string(recorded().at(group).as_object().at(name).as_string()); }

  // JSON.stringify(store) / JSON.stringify(null).
  std::string stringify(const std::optional<CodexAccountModelsStore>& store) { return store ? rt::jsonStringify(availability::toJson(*store)) : "null"; }

  CodexAccountModelsStore parsed(std::string_view text) { return *availability::parseCodexAccountModelsStore(rt::KvValue(rt::jsonParse(text))); }

  CodexAccountCatalogInput entry(std::string accountId, std::string clientVersion, json::array slugs, std::optional<double> updatedAtMs = std::nullopt) {
    return CodexAccountCatalogInput{.accountId = std::move(accountId), .clientVersion = std::move(clientVersion), .slugs = std::move(slugs), .updatedAtMs = updatedAtMs};
  }

  // The fixtures of the recording script.
  CodexAccountModelsStore base() {
    return parsed(R"({"accounts":{"a":{"client_version":"v","slugs":["m0"],"updated_at_ms":1}},"unsupported":{"a":{"x":5,"y":5,"B":5,"b":5,"10":7},"c":{"old":1}}})");
  }
  CodexAccountModelsStore store2() {
    return parsed(R"({"accounts":{"a":{"client_version":"v","slugs":["m0"],"updated_at_ms":1}},"unsupported":{"a":{"m1":5,"m2":6},"b":{"m1":7}}})");
  }
}

TEST("availability parse: stored values as the TS reads them") {
  for (const auto& [name, vector] : recorded().at("parse").as_object()) {
    const std::string input(vector.as_object().at("input").as_string());
    const std::optional<CodexAccountModelsStore> store = availability::parseCodexAccountModelsStore(rt::KvValue(rt::jsonParse(input)));
    EXPECT_EQ(stringify(store), std::string(vector.as_object().at("output").as_string()));
  }
}

TEST("availability parse: Uint8Array and Int16Array entries read through Object.entries") {
  rt::KvObject unsupported;
  unsupported.set("k", rt::KvValue(rt::KvBytes{5, 0, 6}));
  rt::KvArrayBufferView int16{.kind = rt::KvViewKind::Int16Array, .buffer = {}, .byteOffset = 0, .byteLength = 4};
  const std::int16_t elements[2] = {-1, 300};
  int16.buffer.resize(4);
  std::memcpy(int16.buffer.data(), elements, 4);
  unsupported.set("j", rt::KvValue(int16));
  rt::KvObject value;
  value.set("unsupported", rt::KvValue(std::move(unsupported)));
  EXPECT_EQ(stringify(availability::parseCodexAccountModelsStore(rt::KvValue(std::move(value)))), std::string(recorded().at("parseBytes").as_string()));
}

TEST("availability with: bounded rejection maps") {
  const double MAX = availability::CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_AGE_MS;
  EXPECT_EQ(stringify(availability::withCodexModelUnsupported(base(), "a", "z", 5, 5)), expected("with", "ties sort by localeCompare after index keys"));
  EXPECT_EQ(stringify(availability::withCodexModelUnsupported(base(), "a", "m", rt::Number::NaN, 100)), expected("with", "NaN observation is pruned"));
  EXPECT_EQ(stringify(availability::withCodexModelUnsupported(base(), "c", "new", 1 + MAX + 1)), expected("with", "aged entries are pruned"));
  EXPECT_EQ(stringify(availability::withCodexModelUnsupported(base(), "__proto__", "m", 5)), expected("with", "proto account id is an own key"));
  EXPECT_EQ(stringify(availability::withCodexModelUnsupported(base(), "a", "inf", rt::Number::POSITIVE_INFINITY, 10)), expected("with", "infinite observation is kept"));
  EXPECT_EQ(stringify(availability::withCodexModelUnsupported(base(), "a", "y", 8, 8)), expected("with", "existing model moves nowhere"));
}

TEST("availability merge: catalogs clear their own learned rejections") {
  std::vector<CodexAccountCatalogInput> clearing;
  clearing.push_back(entry("a", "v2", json::array{"m1", " m3 ", 4}));
  clearing.push_back(entry("c", "v", json::array{}, 9));
  EXPECT_EQ(stringify(availability::mergeCodexAccountCatalogs(store2(), clearing, 100)), expected("merge", "clears advertised models and keeps the rest"));
  std::vector<CodexAccountCatalogInput> all;
  all.push_back(entry("b", "v", json::array{"m1"}));
  EXPECT_EQ(stringify(availability::mergeCodexAccountCatalogs(store2(), all, 100)), expected("merge", "an account whose models all clear leaves unsupported"));
  EXPECT_EQ(stringify(availability::mergeCodexAccountCatalogs(store2(), {}, 100)), expected("merge", "no entries returns the store"));
  std::vector<CodexAccountCatalogInput> proto;
  proto.push_back(entry("__proto__", "v", json::array{"m1"}));
  EXPECT_EQ(stringify(availability::mergeCodexAccountCatalogs(store2(), proto, 100)), expected("merge", "proto account id is not set"));
  std::vector<CodexAccountCatalogInput> indexed;
  indexed.push_back(entry("z", "v", json::array{}));
  indexed.push_back(entry("7", "v", json::array{}));
  indexed.push_back(entry("07", "v", json::array{}));
  EXPECT_EQ(stringify(availability::mergeCodexAccountCatalogs(availability::emptyCodexAccountModelsStore(), indexed, 3)), expected("merge", "index-like account ids"));
}
