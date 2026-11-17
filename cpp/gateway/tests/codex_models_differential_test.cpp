// Differential cases for src/models/codex-models.ts beyond its bun tests, recorded from Bun
// 1.4.0 running the TypeScript module (tests/codex_models_differential_vectors.inc): the
// exact JSON.stringify of every result (member order, filtered and deduplicated lists,
// integer-like keys), -0 results, JavaScriptCore's TypeError texts, and the value
// equivalent of the module's "same snapshot" reference results.
#include "uos/models/codex_models.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "codex_models_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::models::codex_models;

namespace {
  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(CODEX_MODELS_DIFFERENTIAL_VECTORS);
    return parsed;
  }

  const json::array& vectorList(std::string_view name) { return vectors().at(name).as_array(); }

  std::string text(const json::value& value) {
    const json::string& string = value.as_string();
    return std::string(string.data(), string.size());
  }

  json::value parseText(const json::value& value) { return rt::jsonParse(text(value)); }

  std::string stringify(const std::optional<PromptCacheCapabilities>& value) { return value ? rt::jsonStringify(toJson(*value)) : "null"; }
  std::string stringify(const std::optional<RuntimePromptCacheCapabilities>& value) { return value ? rt::jsonStringify(toJson(*value)) : "null"; }
  std::string stringify(const std::optional<PromptCacheProvider>& value) { return value ? rt::jsonStringify(toJson(*value)) : "null"; }
  std::string stringify(const std::optional<json::object>& value) { return value ? rt::jsonStringify(json::value(*value)) : "null"; }
  std::string stringify(const std::optional<std::string>& value) { return value ? rt::jsonQuote(*value) : "null"; }
  std::string stringify(const std::optional<std::array<double, 3>>& value) {
    if (!value) return "null";
    json::array parts;
    for (const double part : *value) parts.push_back(rt::makeNumber(part));
    return rt::jsonStringify(parts);
  }

  bool isNegativeZero(double value) { return value == 0 && std::signbit(value); }

  bool isNegativeZero(const json::value* value) { return value && value->is_double() && isNegativeZero(value->get_double()); }

  // Overrides recorded as JSON text; "__INFINITY__" stands for Infinity, null for null.
  CodexModelsPayloadOverrides overridesFrom(const json::value& recorded) {
    const json::value parsed = parseText(recorded);
    CodexModelsPayloadOverrides overrides;
    if (const std::optional<std::string_view> source = rt::getString(parsed, "source")) overrides.source = std::string(*source);
    if (const std::optional<std::string_view> clientVersion = rt::getString(parsed, "clientVersion")) overrides.clientVersion = std::string(*clientVersion);
    if (const json::value* updatedAtMs = rt::get(parsed, "updatedAtMs")) {
      if (updatedAtMs->is_string() && updatedAtMs->get_string() == "__INFINITY__") overrides.updatedAtMs = rt::Number::POSITIVE_INFINITY;
      else overrides.updatedAtMs = rt::getNumber(*updatedAtMs);
    }
    return overrides;
  }

  void reportMismatch(int line, const std::string& what, const std::string& actual, const std::string& expected) {
    if (actual != expected) ::uos::rt::test::report(__FILE__, line, what + "\n      actual:   " + actual + "\n      expected: " + expected, false);
  }
}

TEST("normalizePromptCacheCapabilities and compactPromptCacheCapabilities match Bun byte for byte") {
  const json::array& cases = vectorList("capVectors");
  const json::array& negativeZeros = vectorList("capNegZero");
  EXPECT_EQ(cases.size(), std::size_t{59});
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const json::value& vector = cases[index];
    const json::value input = parseText(vector.at("input"));
    const std::optional<PromptCacheCapabilities> normalized = normalizePromptCacheCapabilities(input);
    reportMismatch(__LINE__, "normalize " + text(vector.at("input")), stringify(normalized), text(vector.at("normalized")));
    reportMismatch(__LINE__, "compact " + text(vector.at("input")), stringify(compactPromptCacheCapabilities(input)), text(vector.at("compact")));
    for (const json::value& path : negativeZeros[index].as_array()) {
      // "<provider>.<controls|scope>.<field>": the TS keeps -0, which JSON.stringify prints as 0.
      const PromptCacheCapabilitiesV1* versioned = normalized ? std::get_if<PromptCacheCapabilitiesV1>(&*normalized) : nullptr;
      ASSERT(versioned != nullptr);
      const std::string field = text(path);
      const PromptCacheProvider& provider = versioned->providers.at(0);
      if (field == "0.controls.verified_at_ms") EXPECT(provider.controls && isNegativeZero(provider.controls->verified_at_ms));
      else if (field == "0.scope.verified_at_ms") EXPECT(provider.scope && isNegativeZero(provider.scope->verified_at_ms));
      else if (field == "0.scope.reproducible_cycles") EXPECT(provider.scope && isNegativeZero(provider.scope->reproducible_cycles));
      else ::uos::rt::test::report(__FILE__, __LINE__, "unexpected -0 path " + field, false);
    }
  }
}

TEST("mergePromptCacheCapabilities matches Bun, including undefined sides") {
  for (const json::value& vector : vectorList("mergeVectors")) {
    const std::optional<PromptCacheCapabilities> merged = mergePromptCacheCapabilities(parseText(vector.at("previous")), parseText(vector.at("next")));
    reportMismatch(__LINE__, "merge " + text(vector.at("previous")) + " <- " + text(vector.at("next")), stringify(merged), text(vector.at("output")));
  }
  // previous, next and both undefined, with the first recorded `previous` as the defined side.
  const json::value defined = parseText(vectorList("mergeVectors").at(0).at("previous"));
  const json::value& undefinedSides = vectors().at("mergeUndefined");
  EXPECT_EQ(stringify(mergePromptCacheCapabilities(nullptr, &defined)), text(undefinedSides.at("prevUndefined")));
  EXPECT_EQ(stringify(mergePromptCacheCapabilities(&defined, nullptr)), text(undefinedSides.at("nextUndefined")));
  EXPECT_EQ(stringify(mergePromptCacheCapabilities(nullptr, nullptr)), text(undefinedSides.at("both")));
}

TEST("normalizeCodexModelsPayload matches Bun byte for byte, including -0 counts") {
  const json::array& cases = vectorList("payloadVectors");
  const json::array& negativeZeros = vectorList("payloadNegZero");
  EXPECT_EQ(cases.size(), std::size_t{19});
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const json::value& vector = cases[index];
    const json::value input = parseText(vector.at("input"));
    const std::optional<CodexModelsSnapshot> snapshot =
      vector.at("overrides").is_null() ? normalizeCodexModelsPayload(input) : normalizeCodexModelsPayload(input, overridesFrom(vector.at("overrides")));
    reportMismatch(__LINE__, "payload " + text(vector.at("input")), stringify(snapshot), text(vector.at("output")));
    for (const json::value& path : negativeZeros[index].as_array()) {
      ASSERT(snapshot.has_value());
      const std::string field = text(path);
      if (field == "updated_at_ms") {
        EXPECT(isNegativeZero(rt::get(*snapshot, "updated_at_ms")));
        continue;
      }
      // "<model index>.<member>"
      const std::size_t dot = field.find('.');
      const json::value* model = rt::get(snapshot->at("models").as_array(), static_cast<std::size_t>(std::stoul(field.substr(0, dot))));
      EXPECT(model != nullptr && isNegativeZero(rt::get(*model, field.substr(dot + 1))));
    }
  }
}

TEST("an absent update time falls back to the clock") {
  const double before = rt::dateNow();
  const std::optional<CodexModelsSnapshot> snapshot = normalizeCodexModelsPayload(rt::jsonParse(R"({ "models": [{ "slug": "t" }] })"));
  const double after = rt::dateNow();
  ASSERT(snapshot.has_value());
  const std::optional<double> updatedAtMs = rt::getNumber(*snapshot, "updated_at_ms");
  EXPECT(updatedAtMs && *updatedAtMs >= before && *updatedAtMs <= after);
  EXPECT(!rt::hasOwn(*snapshot, "client_version"));
}

TEST("mergeCodexModelPromptCacheCapabilities matches Bun and returns next unchanged when nothing is added") {
  for (const json::value& vector : vectorList("modelMergeVectors")) {
    const CodexModelsSnapshot next = parseText(vector.at("next")).as_object();
    const CodexModelsSnapshot previous = parseText(vector.at("previous")).as_object();
    const CodexModelsSnapshot merged = mergeCodexModelPromptCacheCapabilities(next, previous);
    reportMismatch(__LINE__, "merge models " + text(vector.at("next")), rt::jsonStringify(merged), text(vector.at("output")));
    // The TS returns `next` itself when no entry changed (the `same` flag is the identity check).
    if (vector.at("same").as_bool()) EXPECT_EQ(rt::jsonStringify(merged), rt::jsonStringify(next));
  }
}

TEST("mergeCodexModelPromptCacheCapabilities throws JavaScriptCore's TypeError when next.models is not an array") {
  const CodexModelsSnapshot previous = rt::jsonParse(R"({ "source": "s", "models": [{ "slug": "a" }], "updated_at_ms": 1 })").as_object();
  for (const json::value& vector : vectorList("typeErrors")) {
    CodexModelsSnapshot next;
    next["source"] = "s";
    if (!vector.at("undefinedModels").as_bool()) {
      next["models"] = parseText(vector.at("models"));
      next["updated_at_ms"] = 1;
    }
    const std::string error = text(vector.at("error"));
    const std::string prefix = "TypeError: ";
    ASSERT(error.starts_with(prefix));
    EXPECT_THROWS_MESSAGE(mergeCodexModelPromptCacheCapabilities(next, previous), rt::TypeError, error.substr(prefix.size()));
  }
  // An empty previous list returns before `next.models` is read.
  const CodexModelsSnapshot unread = rt::jsonParse(R"({ "source": "s", "models": "none", "updated_at_ms": 1 })").as_object();
  const CodexModelsSnapshot emptyPrevious = rt::jsonParse(R"({ "source": "s", "models": [], "updated_at_ms": 1 })").as_object();
  EXPECT_EQ(rt::jsonStringify(mergeCodexModelPromptCacheCapabilities(unread, emptyPrevious)), text(vectors().at("noThrowWhenPreviousEmpty")));
}

TEST("withCodexModelPromptCacheScope matches Bun byte for byte") {
  for (const json::value& vector : vectorList("withVectors")) {
    const CodexModelsSnapshot snapshot = parseText(vector.at("snapshot")).as_object();
    const PromptCacheScope scope = json::value_to<PromptCacheScope>(parseText(vector.at("scope")));
    const std::optional<CodexModelsSnapshot> attached = withCodexModelPromptCacheScope(snapshot, text(vector.at("slug")), text(vector.at("provider")), scope);
    reportMismatch(__LINE__, "attach " + text(vector.at("slug")) + " " + text(vector.at("provider")) + " " + text(vector.at("scope")), stringify(attached),
                   text(vector.at("output")));
  }
}

TEST("slug lookups, provider lookups and eligibility match Bun") {
  const CodexModelsSnapshot snapshot = parseText(vectors().at("lookupSnapshot")).as_object();
  for (const json::value& vector : vectorList("lookupVectors")) {
    const std::string slug = text(vector.at("slug"));
    reportMismatch(__LINE__, "unique " + slug, stringify(getUniqueCodexModelBySlug(snapshot, slug)), text(vector.at("unique")));
    reportMismatch(__LINE__, "provider " + slug, stringify(getCodexModelPromptCacheProvider(snapshot, slug, CODEX_CHATGPT_PROMPT_CACHE_PROVIDER)),
                   text(vector.at("provider")));
    EXPECT_EQ(isCodexModelPromptCacheScopeExperimentEligible(snapshot, slug), vector.at("eligible").as_bool());
  }
  for (const json::value& vector : vectorList("defaultModels")) {
    const CodexModelsSnapshot candidate = parseText(vector.at("snapshot")).as_object();
    EXPECT_EQ(stringify(getCodexModelsSnapshotDefaultModel(candidate)), text(vector.at("output")));
  }
}

TEST("isConcretePromptCacheScope matches Bun") {
  for (const json::value& vector : vectorList("concrete")) {
    const PromptCacheScope scope = json::value_to<PromptCacheScope>(parseText(vector.at("scope")));
    const bool concrete = vector.at("cycles").is_null() ? isConcretePromptCacheScope(scope) : isConcretePromptCacheScope(scope, *rt::getNumber(vector.at("cycles")));
    EXPECT_EQ(concrete, vector.at("output").as_bool());
  }
  const PromptCacheScope scope = json::value_to<PromptCacheScope>(parseText(vectorList("concrete").at(0).at("scope")));
  EXPECT_EQ(isConcretePromptCacheScope(scope, rt::Number::NaN), vectors().at("concreteNaN").as_bool());
}

TEST("client version parsing and comparison match Bun") {
  for (const json::value& vector : vectorList("versionVectors")) {
    reportMismatch(__LINE__, "parse " + rt::jsonQuote(text(vector.at("value"))), stringify(parseCodexClientVersion(text(vector.at("value")))),
                   text(vector.at("parsed")));
  }
  for (const json::value& vector : vectorList("comparePairs")) {
    const std::optional<double> compared = compareCodexClientVersions(text(vector.at("left")), text(vector.at("right")));
    EXPECT_EQ(compared, rt::getNumber(vector.at("output")));
  }
}

TEST("value conversions of the prompt-cache records round-trip and reject other shapes") {
  const json::value capabilities = parseText(vectorList("capVectors").at(17).at("normalized"));
  const PromptCacheCapabilitiesV1 typed = json::value_to<PromptCacheCapabilitiesV1>(capabilities);
  EXPECT_EQ(rt::jsonStringify(json::value_from(typed)), rt::jsonStringify(capabilities));
  EXPECT(json::value_to<PromptCacheCapabilities>(json::value(false)) == PromptCacheCapabilities(PromptCacheCapabilitiesUnsupported{}));
  EXPECT(json::value_to<PromptCacheCapabilities>(capabilities) == PromptCacheCapabilities(typed));
  EXPECT_EQ(rt::jsonStringify(json::value_from(PromptCacheCapabilities(PromptCacheCapabilitiesUnsupported{}))), std::string("false"));
  EXPECT_THROWS_MESSAGE(json::value_to<PromptCacheScope>(rt::jsonParse(R"({ "probe_profile": 1 })")), rt::TypeError,
                        "PromptCacheScope.probe_profile is number, not string");
  EXPECT_THROWS_MESSAGE(json::value_to<PromptCacheCapabilitiesUnsupported>(json::value(true)), rt::TypeError, "PromptCacheCapabilities is boolean, not false");
}
