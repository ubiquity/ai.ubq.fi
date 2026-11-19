// Port of tests/openrouter-models.test.ts, plus differential cases recorded from
// src/models/openrouter-models.ts under Bun 1.4.0 (tests/openrouter_models_vectors.inc).
#include "uos/models/openrouter_models.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "openrouter_models_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::models::openrouter_models;

namespace {
  json::value payload(const std::vector<json::value>& models) {
    json::array data(models.begin(), models.end());
    return json::object{{"data", std::move(data)}};
  }

  const json::value solEntry = rt::jsonParse(
    R"({"id":"openai/gpt-5.6-sol","context_length":1050000,"top_provider":{"context_length":1050000,"max_completion_tokens":128000},)"
    R"("reasoning":{"supported_efforts":["max","xhigh","high","medium","low","none"],"default_effort":"medium","mandatory":false}})");

  rt::Response jsonResponse(const json::value& body, double status = 200) {
    rt::ResponseInit init;
    init.status = status;
    init.headers = rt::Headers{{"content-type", "application/json"}};
    return rt::Response(rt::BodyInit(rt::jsonStringify(body)), init);
  }

  std::optional<std::string> idOf(const std::optional<OpenRouterModelMetadata>& model) { return model ? std::optional<std::string>(model->id) : std::nullopt; }

  // withCleanCache: the global fetch rejects, and the module's cache and test fetcher are reset after.
  struct CleanCache {
    rt::GlobalFetchOverride fetch{rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
      throw rt::JsError("External network is not available in this catalog fixture");
      co_return rt::Response();
    })};
    CleanCache() { resetOpenRouterModelsCacheForTest(); }
    ~CleanCache() {
      setOpenRouterModelsFetchForTest({});
      resetOpenRouterModelsCacheForTest();
    }
  };

  rt::Task<std::shared_ptr<const OpenRouterModelsSnapshot>> fetchModels(FetchOpenRouterModelsOptions options = {}) {
    rt::Task<std::shared_ptr<const OpenRouterModelsSnapshot>> call = fetchOpenRouterModels(std::move(options));
    std::shared_ptr<const OpenRouterModelsSnapshot> snapshot = co_await std::move(call);
    co_return snapshot;
  }

  json::value metadataJson(const OpenRouterModelMetadata& model) {
    json::object object;
    rt::objectDefine(object, "id", json::value(model.id));
    rt::objectDefine(object, "context_window_tokens", model.context_window_tokens ? json::value(*model.context_window_tokens) : json::value(nullptr));
    rt::objectDefine(object, "max_context_window_tokens", model.max_context_window_tokens ? json::value(*model.max_context_window_tokens) : json::value(nullptr));
    if (model.reasoning) {
      json::array efforts(model.reasoning->supported_efforts.begin(), model.reasoning->supported_efforts.end());
      rt::objectDefine(object, "reasoning",
                       json::object{{"supported_efforts", std::move(efforts)},
                                    {"default_effort", model.reasoning->default_effort ? json::value(*model.reasoning->default_effort) : json::value(nullptr)},
                                    {"mandatory", model.reasoning->mandatory}});
    } else {
      rt::objectDefine(object, "reasoning", nullptr);
    }
    if (model.output_modalities) rt::objectDefine(object, "output_modalities", json::array(model.output_modalities->begin(), model.output_modalities->end()));
    return object;
  }
}

TEST("the upstream payload keeps context, provider context, and reasoning efforts") {
  const std::vector<OpenRouterModelMetadata> models = openRouterModelsFromPayload(payload({
    solEntry,
    rt::jsonParse(R"({"id":"z-ai/glm-5.3","context_length":1310720,"top_provider":{"context_length":1400000},"reasoning":{"supported_efforts":["max","high"],"default_effort":"max","mandatory":true}})"),
    rt::jsonParse(R"({"id":"","context_length":10})"),
    rt::jsonParse(R"({"id":"no-metadata/model"})"),
    json::value("not-an-object"),
    solEntry,
  }));
  ASSERT_EQ(models.size(), std::size_t{3});
  EXPECT(models[0] == (OpenRouterModelMetadata{"openai/gpt-5.6-sol", 1'050'000, 1'050'000, OpenRouterReasoning{{"max", "xhigh", "high", "medium", "low", "none"}, "medium", false}}));
  EXPECT_EQ(models[1].max_context_window_tokens, std::optional<std::int64_t>(1'400'000));
  EXPECT(models[1].reasoning == (OpenRouterReasoning{{"max", "high"}, "max", true}));
  EXPECT(models[2] == (OpenRouterModelMetadata{"no-metadata/model", std::nullopt, std::nullopt, std::nullopt}));
}

TEST("a reasoning block without efforts or a default is not metadata") {
  const std::vector<OpenRouterModelMetadata> models = openRouterModelsFromPayload(payload({rt::jsonParse(R"({"id":"a/b","reasoning":{"mandatory":true}})")}));
  ASSERT_EQ(models.size(), std::size_t{1});
  EXPECT(!models[0].reasoning);
}

TEST("ids join through ordered tiers, and an ambiguous key resolves to nothing") {
  const std::vector<OpenRouterModelMetadata> models = openRouterModelsFromPayload(payload({
    solEntry,
    rt::jsonParse(R"({"id":"nvidia/nemotron-3-ultra-550b-a55b","context_length":262144,"top_provider":{"context_length":262144}})"),
    rt::jsonParse(R"({"id":"xiaomi/mimo-v2.5","context_length":1048576,"top_provider":{"context_length":1048576}})"),
    rt::jsonParse(R"({"id":"tencent/hy3","context_length":262144,"top_provider":{"context_length":262144}})"),
    rt::jsonParse(R"({"id":"tencent/hy3:free","context_length":131072,"top_provider":{"context_length":131072}})"),
    rt::jsonParse(R"({"id":"~deepseek/deepseek-flash-latest","context_length":1048576,"top_provider":{"context_length":1048576}})"),
  }));

  // Exact slug.
  EXPECT_EQ(idOf(matchOpenRouterModel(models, "gpt-5.6-sol")), "openai/gpt-5.6-sol");
  // Exact upstream id, and a provider-prefixed gateway id through the same slug tier.
  EXPECT_EQ(idOf(matchOpenRouterModel(models, "openai/gpt-5.6-sol")), "openai/gpt-5.6-sol");
  EXPECT_EQ(idOf(matchOpenRouterModel(models, "openlux/gpt-5.6-sol")), "openai/gpt-5.6-sol");
  // Vendor prefix and punctuation differences.
  EXPECT_EQ(idOf(matchOpenRouterModel(models, "nvidia-nemotron-3-ultra-550b-a55b")), "nvidia/nemotron-3-ultra-550b-a55b");
  EXPECT_EQ(idOf(matchOpenRouterModel(models, "xiaomi-mimo-v2-5")), "xiaomi/mimo-v2.5");
  // A `-free` suffix resolves to the `:free` variant, not to the base model.
  EXPECT_EQ(idOf(matchOpenRouterModel(models, "hy3-free")), "tencent/hy3:free");
  EXPECT_EQ(idOf(matchOpenRouterModel(models, "hy3")), "tencent/hy3");
  // Without a `:free` row upstream, the billing marker falls back to the base
  // model of the same name rather than leaving the id unknown.
  const std::vector<OpenRouterModelMetadata> baseOnly =
    openRouterModelsFromPayload(payload({rt::jsonParse(R"({"id":"tencent/hy3","context_length":262144,"top_provider":{"context_length":262144}})")}));
  EXPECT_EQ(idOf(matchOpenRouterModel(baseOnly, "hy3-free")), "tencent/hy3");
  const std::vector<OpenRouterModelMetadata> batchOnly =
    openRouterModelsFromPayload(payload({rt::jsonParse(R"({"id":"openai/gpt-oss-120b:batch","context_length":131072,"top_provider":{"context_length":131072}})")}));
  EXPECT_EQ(idOf(matchOpenRouterModel(batchOnly, "gpt-oss-120b")), "openai/gpt-oss-120b:batch");
  // A preview or dated suffix names a different snapshot and must not be folded in.
  const std::vector<OpenRouterModelMetadata> previewOnly =
    openRouterModelsFromPayload(payload({rt::jsonParse(R"({"id":"tencent/hy3-preview","context_length":262144,"top_provider":{"context_length":262144}})")}));
  EXPECT_EQ(idOf(matchOpenRouterModel(previewOnly, "hy3-preview")), "tencent/hy3-preview");
  EXPECT_EQ(idOf(matchOpenRouterModel(previewOnly, "hy3")), std::nullopt);
  // A rolling alias still answers an id that exists nowhere else.
  EXPECT_EQ(idOf(matchOpenRouterModel(models, "deepseek-flash")), "~deepseek/deepseek-flash-latest");
  // Internal ids have no upstream row and must stay unknown.
  EXPECT_EQ(idOf(matchOpenRouterModel(models, "codex-auto-review")), std::nullopt);
  EXPECT_EQ(idOf(matchOpenRouterModel(models, "gpt-reserve")), std::nullopt);
  EXPECT_EQ(idOf(matchOpenRouterModel(models, "")), std::nullopt);

  // Two upstream models claiming one key cannot identify either of them.
  const std::vector<OpenRouterModelMetadata> ambiguous = openRouterModelsFromPayload(payload({
    rt::jsonParse(R"({"id":"alpha/shared-model","context_length":100,"top_provider":{"context_length":100}})"),
    rt::jsonParse(R"({"id":"beta/shared-model","context_length":200,"top_provider":{"context_length":200}})"),
  }));
  EXPECT_EQ(idOf(matchOpenRouterModel(ambiguous, "shared-model")), std::nullopt);
  EXPECT_EQ(idOf(matchOpenRouterModel(ambiguous, "alpha/shared-model")), "alpha/shared-model");
}

ASYNC_TEST("a snapshot is cached, coalesced, and survives a failed refresh") {
  const CleanCache clean;
  int calls = 0;
  std::string mode = "ok";
  const OpenRouterFetch fetcher([&calls, &mode](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
    calls += 1;
    if (mode == "fail") throw rt::JsError("upstream down");
    if (mode == "empty") co_return jsonResponse(payload({}));
    co_return jsonResponse(payload({solEntry}));
  });
  setOpenRouterModelsFetchForTest(fetcher);

  EXPECT_EQ(idOf(openRouterMetadataFor("gpt-5.6-sol")), std::nullopt);
  const std::shared_ptr<const OpenRouterModelsSnapshot> first = co_await fetchModels();
  EXPECT_EQ(calls, 1);
  EXPECT(first && first->models.size() == 1);
  EXPECT_EQ(idOf(openRouterMetadataFor("gpt-5.6-sol")), "openai/gpt-5.6-sol");
  EXPECT_EQ(idOf(openRouterMetadataFor("gpt-reserve")), std::nullopt);

  // Within the TTL the cache answers without another request.
  co_await fetchModels();
  EXPECT_EQ(calls, 1);

  // A failed forced refresh keeps the last good snapshot and backs off.
  mode = "fail";
  const std::shared_ptr<const OpenRouterModelsSnapshot> afterFailure = co_await fetchModels({.force = true});
  EXPECT_EQ(calls, 2);
  EXPECT(afterFailure && afterFailure->models.size() == 1);
  EXPECT_EQ(idOf(openRouterMetadataFor("gpt-5.6-sol")), "openai/gpt-5.6-sol");
  co_await fetchModels();
  EXPECT_EQ(calls, 2);

  // An empty catalog is a failed refresh, not an empty world.
  mode = "empty";
  const std::shared_ptr<const OpenRouterModelsSnapshot> afterEmpty = co_await fetchModels({.force = true});
  EXPECT_EQ(calls, 3);
  EXPECT(afterEmpty && afterEmpty->models.size() == 1);

  // A non-OK status is also a failure rather than a silent empty catalog.
  const OpenRouterFetch rejecting([&calls](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
    calls += 1;
    co_return jsonResponse(json::object{{"error", "nope"}}, 429);
  });
  setOpenRouterModelsFetchForTest(rejecting);
  const std::shared_ptr<const OpenRouterModelsSnapshot> afterHttpFailure = co_await fetchModels({.force = true});
  EXPECT_EQ(calls, 4);
  EXPECT(afterHttpFailure && afterHttpFailure->models.size() == 1);
}

ASYNC_TEST("no fetcher means no request and no metadata") {
  const CleanCache clean;
  const std::shared_ptr<const OpenRouterModelsSnapshot> snapshot = co_await fetchModels();
  EXPECT(snapshot == nullptr);
  EXPECT_EQ(idOf(openRouterMetadataFor("gpt-5.6-sol")), std::nullopt);
}

ASYNC_TEST("an index allocation failure keeps the previous catalog and its lookup rows") {
  const CleanCache clean;
  setOpenRouterModelsFetchForTest(OpenRouterFetch([](rt::RequestInfo, rt::RequestInit) {
    return jsonResponse(payload({solEntry}));
  }));
  auto first = co_await fetchModels();
  ASSERT(first);
  const std::weak_ptr previous = first;
  first.reset();

  const json::value replacement = rt::jsonParse(R"({"id":"openai/gpt-replacement","context_length":8192})");
  setOpenRouterModelsFetchForTest(OpenRouterFetch([replacement](rt::RequestInfo, rt::RequestInit) {
    return jsonResponse(payload({replacement}));
  }));
  const FetchOpenRouterModelsOptions force{.force = true};
  setOpenRouterIndexAllocationFailureForTest(true);
  const auto failed = co_await fetchModels(force);
  ASSERT(!previous.expired());
  EXPECT_EQ(failed, previous.lock());
  EXPECT_EQ(idOf(openRouterMetadataFor("gpt-5.6-sol")), "openai/gpt-5.6-sol");
  EXPECT_EQ(idOf(openRouterMetadataFor("gpt-replacement")), std::nullopt);

  setOpenRouterIndexAllocationFailureForTest(false);
  const auto refreshed = co_await fetchModels(force);
  EXPECT(refreshed != failed);
  EXPECT_EQ(idOf(openRouterMetadataFor("gpt-5.6-sol")), std::nullopt);
  EXPECT_EQ(idOf(openRouterMetadataFor("gpt-replacement")), "openai/gpt-replacement");
}

ASYNC_TEST("differential: coalesced refreshes share one request and cachedOnly never fetches") {
  const CleanCache clean;
  int calls = 0;
  const OpenRouterFetch fetcher([&calls](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
    calls += 1;
    co_await rt::sleep(10);
    co_return jsonResponse(payload({solEntry}));
  });
  setOpenRouterModelsFetchForTest(fetcher);
  const std::shared_ptr<const OpenRouterModelsSnapshot> cachedOnly = co_await fetchModels({.cachedOnly = true});
  EXPECT(cachedOnly == nullptr);
  EXPECT_EQ(calls, 0);
  const rt::Promise<std::shared_ptr<const OpenRouterModelsSnapshot>> first(fetchOpenRouterModels());
  const rt::Promise<std::shared_ptr<const OpenRouterModelsSnapshot>> second(fetchOpenRouterModels());
  const std::shared_ptr<const OpenRouterModelsSnapshot> a = co_await first.wait();
  const std::shared_ptr<const OpenRouterModelsSnapshot> b = co_await second.wait();
  EXPECT_EQ(calls, 1);
  EXPECT(a != nullptr && a == b && a == openRouterModelsSnapshot());
  warmOpenRouterModels();
  EXPECT_EQ(calls, 1);
}

TEST("differential: payload parsing, endpoint paths and tiered matching") {
  const json::value vectors = rt::jsonParse(kOpenRouterVectors);
  for (const json::value& entry : *rt::getArray(vectors, "parsed")) {
    const json::array& row = entry.get_array();
    const std::vector<OpenRouterModelMetadata> models = openRouterModelsFromPayload(row[0]);
    json::array actual;
    json::array capabilities;
    for (const OpenRouterModelMetadata& model : models) {
      actual.push_back(metadataJson(model));
      const std::vector<std::string> paths = openRouterModelEndpointPaths(model);
      capabilities.push_back(json::array{json::array(paths.begin(), paths.end()), openRouterModelSupportsText(model), openRouterModelSupportsDecisions(model)});
    }
    EXPECT_EQ(rt::jsonStringify(actual), rt::jsonStringify(row[1]));
    EXPECT_EQ(rt::jsonStringify(capabilities), rt::jsonStringify(row[2]));
  }
  json::array catalogRows;
  for (const json::value& id : *rt::getArray(vectors, "catalog")) catalogRows.push_back(json::object{{"id", id}});
  const std::vector<OpenRouterModelMetadata> catalog = openRouterModelsFromPayload(json::value(json::object{{"data", catalogRows}}));
  for (const json::value& entry : *rt::getArray(vectors, "matches")) {
    const json::array& row = entry.get_array();
    const std::optional<std::string> expected = row[1].is_null() ? std::nullopt : std::optional<std::string>(std::string(row[1].get_string()));
    EXPECT_EQ(idOf(matchOpenRouterModel(catalog, std::string_view(row[0].get_string()))), expected);
  }
}
