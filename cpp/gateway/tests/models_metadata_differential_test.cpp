// Differential cases for src/models/metadata.ts beyond tests/model-metadata.test.ts: generated
// source combinations (null/undefined/garbage hint members, tier lists of every entry shape,
// windows at the 50,000-token reserve and the 2^53 boundary), raw Codex snapshot records, and the
// cached-catalog path of an omitted `openRouter`. Expected values were recorded from the
// TypeScript module under Bun 1.4.0 (tests/models_metadata_differential_vectors.inc).
#include "uos/models/metadata.hpp"

#include "uos/models/openrouter_models.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "models_metadata_differential_vectors.inc"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::models::metadata;
  using namespace uos::models::openrouter_models;

  std::optional<std::int64_t> windowOf(const json::value& value) {
    if (value.is_null()) return std::nullopt;
    return static_cast<std::int64_t>(*rt::getNumber(value));
  }

  OpenRouterModelMetadata openRouterOf(const json::value& value) {
    const json::object& object = value.as_object();
    OpenRouterModelMetadata metadata;
    metadata.id = std::string(object.at("id").as_string());
    metadata.context_window_tokens = windowOf(object.at("context_window_tokens"));
    metadata.max_context_window_tokens = windowOf(object.at("max_context_window_tokens"));
    if (const json::object* reasoning = object.at("reasoning").if_object()) {
      OpenRouterReasoning parsed;
      for (const json::value& effort : reasoning->at("supported_efforts").as_array()) parsed.supported_efforts.emplace_back(effort.as_string());
      const json::value& defaultEffort = reasoning->at("default_effort");
      if (!defaultEffort.is_null()) parsed.default_effort = std::string(defaultEffort.as_string());
      parsed.mandatory = reasoning->at("mandatory").as_bool();
      metadata.reasoning = parsed;
    }
    return metadata;
  }

  rt::Nullable<ModelMetadataHint> hintSource(const json::object& sources, std::string_view key) {
    const json::value* value = rt::get(sources, key);
    if (value == nullptr) return rt::undefined;
    if (value->is_null()) return nullptr;
    return json::value_to<ModelMetadataHint>(*value);
  }

  ModelMetadataSources sourcesOf(const json::object& sources) {
    ModelMetadataSources out;
    out.codex = hintSource(sources, "codex");
    out.codexSubscription = hintSource(sources, "codexSubscription");
    out.provider = hintSource(sources, "provider");
    if (const json::value* openRouter = rt::get(sources, "openRouter")) {
      if (openRouter->is_null()) out.openRouter = nullptr;
      else out.openRouter = openRouterOf(*openRouter);
    }
    return out;
  }

  std::string stringify(const ResolvedModelMetadata& resolved) { return rt::jsonStringify(json::value_from(resolved)); }

  // Reports one mismatching vector with its inputs.
  bool same(const std::string& actual, const json::value& expected, const json::value& input) {
    const std::string wanted = rt::jsonStringify(expected);
    if (actual == wanted) return true;
    EXPECT_EQ(actual, wanted);
    std::fprintf(stderr, "      input: %s\n", rt::jsonStringify(input).c_str());
    return false;
  }
}

TEST("differential: resolveModelMetadata over generated source combinations matches Bun") {
  resetOpenRouterModelsCacheForTest();
  const json::value vectors = rt::jsonParse(kMetadataVectors);
  int checked = 0;
  for (const json::value& row : vectors.at("resolve").as_array()) {
    const json::array& vector = row.as_array();
    const std::string id(vector[0].as_string());
    const ResolvedModelMetadata resolved = vector[1].is_null() ? resolveModelMetadata(id) : resolveModelMetadata(id, sourcesOf(vector[1].as_object()));
    same(stringify(resolved), vector[2], row);
    checked += 1;
  }
  EXPECT_EQ(checked, 209);
}

TEST("differential: codexSnapshotMetadataHint over raw records matches Bun") {
  resetOpenRouterModelsCacheForTest();
  const json::value vectors = rt::jsonParse(kMetadataVectors);
  int checked = 0;
  for (const json::value& row : vectors.at("snapshot").as_array()) {
    const json::array& vector = row.as_array();
    const std::optional<ModelMetadataHint> hint = codexSnapshotMetadataHint(vector[0].if_object());
    EXPECT_EQ(hint.has_value(), !vector[1].is_null());
    if (hint) same(rt::jsonStringify(json::value_from(*hint)), vector[1], vector[0]);
    ModelMetadataSources sources;
    sources.codex = rt::orNull(hint);
    sources.codexSubscription = codexSubscriptionMetadataHint();
    sources.openRouter = nullptr;
    same(stringify(resolveModelMetadata("snapshot-model", sources)), vector[2], vector[0]);
    checked += 1;
  }
  EXPECT_EQ(checked, 69);
}

ASYNC_TEST("differential: an omitted openRouter source reads the cached OpenRouter catalog") {
  resetOpenRouterModelsCacheForTest();
  const json::value vectors = rt::jsonParse(kMetadataCacheVectors);
  same(stringify(resolveModelMetadata("gpt-5.6-sol")), vectors.at("before"), json::value("gpt-5.6-sol"));

  const json::value payload = vectors.at("payload");
  FetchOpenRouterModelsOptions options;
  options.force = true;
  options.fetcher = rt::Fetch([payload](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
    const rt::ResponseInit init;
    co_return rt::Response::json(payload, init);
  });
  rt::Task<std::shared_ptr<const OpenRouterModelsSnapshot>> refresh = fetchOpenRouterModels(std::move(options));
  const std::shared_ptr<const OpenRouterModelsSnapshot> snapshot = co_await std::move(refresh);
  ASSERT(snapshot != nullptr);
  EXPECT_EQ(snapshot->models.size(), std::size_t{6});

  int checked = 0;
  for (const json::value& row : vectors.at("vectors").as_array()) {
    const json::array& vector = row.as_array();
    same(stringify(resolveModelMetadata(std::string(vector[0].as_string()), sourcesOf(vector[1].as_object()))), vector[2], row);
    checked += 1;
  }
  EXPECT_EQ(checked, 12);
  resetOpenRouterModelsCacheForTest();
}

TEST("ModelMetadataHint and ResolvedModelMetadata convert to and from JSON in declaration order") {
  ModelMetadataHint hint = codexSubscriptionMetadataHint();
  EXPECT_EQ(rt::jsonStringify(json::value_from(hint)), std::string(R"({"context_window_tokens":272000,"max_context_window_tokens":872000})"));
  hint.auto_compact_token_limit_tokens = nullptr;
  hint.supported_reasoning_levels = json::array{nullptr, "low", json::object{{"effort", "high"}}};
  hint.default_reasoning_effort = json::value(nullptr);
  const json::value written = json::value_from(hint);
  EXPECT_EQ(rt::jsonStringify(written),
            std::string(R"({"context_window_tokens":272000,"max_context_window_tokens":872000,"auto_compact_token_limit_tokens":null,)"
                        R"("supported_reasoning_levels":[null,"low",{"effort":"high"}],"default_reasoning_effort":null})"));
  const ModelMetadataHint read = json::value_to<ModelMetadataHint>(written);
  EXPECT(read.context_window_tokens == 272'000.0);
  EXPECT(read.auto_compact_token_limit_tokens == nullptr);
  EXPECT(read.effective_context_window_percent == rt::undefined);
  EXPECT(read.default_reasoning_effort == json::value(nullptr));
  EXPECT(json::value_to<ModelMetadataHint>(rt::jsonParse("{}")).default_reasoning_effort == std::nullopt);
  EXPECT_THROWS_MESSAGE(json::value_to<ModelMetadataHint>(rt::jsonParse(R"({"context_window_tokens":"1"})")), rt::TypeError,
                        "ModelMetadataHint.context_window_tokens is string, not number, null or undefined");
  EXPECT_THROWS_MESSAGE(json::value_to<ModelMetadataHint>(rt::jsonParse(R"({"supported_reasoning_levels":{}})")), rt::TypeError,
                        "ModelMetadataHint.supported_reasoning_levels is object, not array, null or undefined");

  ModelMetadataSources sources;
  sources.codexSubscription = codexSubscriptionMetadataHint();
  sources.openRouter = nullptr;
  const ResolvedModelMetadata resolved = resolveModelMetadata("codex-only-model", sources);
  const json::value resolvedJson = json::value_from(resolved);
  EXPECT_EQ(rt::jsonStringify(resolvedJson),
            std::string(R"({"context_window_tokens":272000,"max_context_window_tokens":872000,"auto_compact_token_limit_tokens":222000,)"
                        R"("effective_context_window_percent":95,"supported_reasoning_levels":null,"default_reasoning_effort":null,)"
                        R"("context_source":"codex_subscription","reasoning_source":"unknown"})"));
  EXPECT(json::value_to<ResolvedModelMetadata>(resolvedJson) == resolved);
  EXPECT_THROWS_MESSAGE(json::value_to<ResolvedModelMetadata>(rt::jsonParse(R"({"context_window_tokens":1})")), rt::TypeError,
                        "ResolvedModelMetadata.max_context_window_tokens is undefined, not number or null");
}
