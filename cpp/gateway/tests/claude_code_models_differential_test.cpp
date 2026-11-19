// Links: tests/claude_code_models_differential_test.cpp src/provider/claude_code_models.cpp src/provider/claude_code.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/surplus_claude_request.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/responses_stream.cpp src/bounded_response_body.cpp src/inference_deadline.cpp src/utils.cpp src/kv.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp src/storage/value_compatibility.cpp src/storage/key_compatibility.cpp
// Differential cases for src/provider/claude-code-models.ts (tests/claude-code-models.test.ts needs modules of later
// waves). The vectors (tests/claude_code_models_differential_vectors.inc) were recorded from the TS under bun 1.4.0 with
// Date.now fixed, crypto.randomUUID a counter, the provider credential variables unset, an in-memory Deno KV and a
// scripted fetcher: paged discovery and its failures, claudeCodeReasoningLevels, and fetchClaudeCodeModels' module cache
// (TTL, forced refresh, stale fallback, expected bindings, cacheValidatedClaudeCodeModels, a shared pending refresh).
// Every case passes `kv` explicitly, so getKv() is never consulted.
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace uos;
namespace models = provider::claude_code_models;
namespace accounts = provider::claude_code_accounts;
namespace credentials = provider::claude_code_credentials;
using provider::claude_code::ClaudeCodeAuthHeaders;
using provider::claude_code::ClaudeCodeEnvLookup;

namespace {
  const char* const VECTORS =
#include "tests/claude_code_models_differential_vectors.inc"
    ;

  constexpr double NOW = 1'700'000'000'000;

  const json::object& vectors() {
    static const json::object parsed = rt::jsonParse(VECTORS).as_object();
    return parsed;
  }

  void expectJson(const json::value& actual, const json::value& expected, const std::string& label, const char* file, int line) {
    if (!rt::deepEqual(actual, expected))
      rt::test::report(file, line, label + "\n      actual:   " + json::serialize(actual) + "\n      expected: " + json::serialize(expected), false);
  }

  rt::crypto::RandomSource uuidSource() {
    auto counter = std::make_shared<std::uint64_t>(0);
    return [counter](std::span<std::uint8_t> bytes) {
      *counter += 1;
      for (auto& byte : bytes) byte = 0;
      for (std::size_t index = 0; index < 6 && index < bytes.size(); ++index)
        bytes[bytes.size() - 1 - index] = static_cast<std::uint8_t>((*counter >> (8 * index)) & 0xff);
    };
  }

  struct Reply {
    double status = 200;
    std::optional<std::string> body{};
    std::optional<std::string> throws{};
  };
  Reply pageReply(std::string_view data, std::string_view extra = "") {
    return {.status = 200, .body = "{\"data\":" + std::string(data) + ",\"has_more\":false" + std::string(extra) + "}"};
  }

  // The generator's module-level fetcher: records url, headers and redirect, then answers from `replies`.
  struct ScriptedFetch {
    std::shared_ptr<json::array> calls = std::make_shared<json::array>();
    std::shared_ptr<std::deque<Reply>> replies = std::make_shared<std::deque<Reply>>();

    rt::Fetch fetcher() const {
      auto recorded = calls;
      auto queue = replies;
      return rt::Fetch([recorded, queue](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
        json::object call;
        if (const rt::URL* url = std::get_if<rt::URL>(&input)) call["url"] = json::string(url->href());
        else call["url"] = json::string(std::get<std::string>(input));
        json::array headers;
        for (const auto& [name, value] : rt::Headers(init.headers.value_or(rt::Headers())).entries()) {
          json::array pair;
          pair.emplace_back(json::string(name));
          pair.emplace_back(json::string(value));
          headers.emplace_back(std::move(pair));
        }
        call["headers"] = std::move(headers);
        call["redirect"] = init.redirect ? json::value(json::string(*init.redirect)) : json::value(nullptr);
        recorded->emplace_back(std::move(call));
        if (queue->empty()) throw rt::JsError("no reply left");
        const Reply reply = queue->front();
        queue->pop_front();
        if (reply.throws) throw rt::TypeError(*reply.throws);
        rt::ResponseInit responseInit;
        responseInit.status = reply.status;
        responseInit.headers = rt::Headers{{"content-type", "application/json"}};
        return reply.body ? rt::Response(rt::BodyInit(*reply.body), responseInit) : rt::Response(nullptr, responseInit);
      });
    }
  };

  json::value errorOf(const rt::Reason& reason) {
    json::object out;
    try {
      std::rethrow_exception(reason);
    } catch (const models::ClaudeCodeModelsError& error) {
      out["name"] = json::string(error.name);
      out["message"] = json::string(error.message);
      out["status"] = error.status ? rt::makeNumber(*error.status) : json::value(nullptr);
    } catch (const rt::JsError& error) {
      out["name"] = json::string(error.name);
      out["message"] = json::string(error.message);
      out["status"] = nullptr;
    } catch (...) {
      out["thrown"] = rt::reasonToString(reason);
    }
    return out;
  }

  template <class T> json::value plain(const T& value) { return json::value_from(value); }
  template <class T> json::value plain(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }

  template <class T> rt::Task<json::value> run(rt::Task<T> task) {
    rt::Reason failure;
    json::object out;
    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(task);
        json::object undefinedMarker;
        undefinedMarker["$undefined"] = true;
        out["ok"] = std::move(undefinedMarker);
      } else {
        T value = co_await std::move(task);
        out["ok"] = plain(value);
      }
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) out["error"] = errorOf(failure);
    co_return json::value(std::move(out));
  }

  struct Isolation {
    Isolation() : env({{"CLAUDE_API_KEY", std::nullopt}, {"CLAUDE_CODE_OAUTH_TOKEN", std::nullopt}, {"CLAUDE_CODE_WORKSPACE_ID", std::nullopt}}), random(uuidSource()) {
      rt::globalDateNow() = [] { return NOW; };
      models::resetClaudeCodeModelsCacheForTest();
      credentials::resetClaudeCodeCredentialCacheForTest();
    }
    rt::env::ScopedOverride env;
    rt::crypto::ScopedRandomSource random;
  };

  ClaudeCodeEnvLookup catalogEnv() {
    return [](const std::string& name) -> std::optional<std::string> {
      if (name == "CLAUDE_API_KEY") return std::string("sk-ant-api-catalog");
      if (name == "CLAUDE_CODE_WORKSPACE_ID") return std::string("wrkspc_env");
      return std::nullopt;
    };
  }

  const std::string OPUS =
    R"({"id":"claude-opus-5-5","display_name":"Claude Opus 5.5","created_at":"2026-05-01T12:34:56Z","max_input_tokens":200000,"max_tokens":64000,)"
    R"("capabilities":{"thinking":{"types":{"disabled":{"supported":true},"adaptive":{"supported":true},"enabled":{"supported":false}}},)"
    R"("effort":{"supported":true,"none":{"supported":true},"low":{"supported":true},"medium":{"supported":false},"high":{"supported":true},"2":{"supported":true},"x":5},)"
    R"("image_input":{"supported":true},"pdf_input":{"supported":"yes"}}})";
  const std::string SONNET =
    R"({"id":"claude-sonnet-5-5","created_at":"garbage","max_input_tokens":1.5,"max_tokens":0,"capabilities":{"effort":{"supported":true,"low":{"supported":true}}}})";
  const std::string HAIKU = R"({"id":"claude-haiku-5","display_name":7,"created_at":"1969-12-31T23:59:59Z","capabilities":[]})";

  struct Step {
    json::value outcome;
    json::array calls;
  };

  // `step(steps, fn)`: the outcome and the fetches this step made.
#define MODELS_STEP(steps, fetch, task)                                                    \
  do {                                                                                     \
    const std::size_t uosBefore = (fetch).calls->size();                                   \
    auto uosTask = (task);                                                                 \
    auto uosRun = run(std::move(uosTask));                                                 \
    json::value uosOutcome = co_await std::move(uosRun);                                   \
    json::array uosCalls((fetch).calls->begin() + static_cast<std::ptrdiff_t>(uosBefore), (fetch).calls->end()); \
    json::object uosStep;                                                                  \
    uosStep["outcome"] = std::move(uosOutcome);                                            \
    uosStep["calls"] = std::move(uosCalls);                                                \
    (steps).emplace_back(std::move(uosStep));                                              \
  } while (0)
}

ASYNC_TEST("discovery follows cursors, filters ids and fails closed like the TS") {
  const json::object& expected = vectors().at("discovery").as_object();
  const ClaudeCodeAuthHeaders apiKey{.kind = "api_key", .secret = "sk-ant-api-models"};
  const ClaudeCodeAuthHeaders oauth{.kind = "oauth", .secret = "sk-ant-oat-models"};
  struct Case {
    std::string name;
    std::vector<Reply> script;
    ClaudeCodeAuthHeaders credential;
    std::optional<std::string> workspaceId;
    std::function<double()> now;
  };
  std::vector<Case> cases;
  cases.push_back({"single page", {pageReply("[" + OPUS + "," + SONNET + "," + HAIKU + R"(,{"id":"gpt-5"},{"id":"claude-)" + std::string(520, 'x') + "\"}]")}, apiKey,
                   "wrkspc_m", nullptr});
  cases.push_back({"pages with cursor",
                   {Reply{.body = "{\"data\":[" + SONNET + "," + OPUS + R"(],"has_more":true,"last_id":"claude-opus-5-5"})"},
                    Reply{.body = R"({"data":[{"id":"claude-sonnet-5-5","created_at":"garbage","max_input_tokens":1.5,"max_tokens":0,"capabilities":{"effort":{"supported":true,"low":{"supported":true}}},"display_name":"Sonnet again"},)" +
                                  HAIKU + R"(],"has_more":true,"last_id":"cursor 2"})"},
                    pageReply("[]")},
                   oauth, std::nullopt, [] { return 1234.5; }});
  cases.push_back({"repeated cursor",
                   {Reply{.body = std::string(R"({"data":[],"has_more":true,"last_id":"c1"})")}, Reply{.body = std::string(R"({"data":[],"has_more":true,"last_id":"c1"})")}},
                   apiKey, std::nullopt, nullptr});
  cases.push_back({"non-ok", {Reply{.status = 401, .body = std::string("nope")}}, apiKey, std::nullopt, nullptr});
  cases.push_back({"fetch throws", {Reply{.throws = std::string("offline")}}, apiKey, std::nullopt, nullptr});
  cases.push_back({"row without id", {pageReply(R"([{"id":" "}])")}, apiKey, std::nullopt, nullptr});
  cases.push_back({"row not a record", {pageReply("[5]")}, apiKey, std::nullopt, nullptr});
  cases.push_back({"has_more missing", {Reply{.body = std::string(R"({"data":[]})")}}, apiKey, std::nullopt, nullptr});
  cases.push_back({"empty last_id", {Reply{.body = std::string(R"({"data":[],"has_more":true,"last_id":""})")}}, apiKey, std::nullopt, nullptr});
  cases.push_back({"data not array", {Reply{.body = std::string(R"({"data":{}})")}}, apiKey, std::nullopt, nullptr});
  cases.push_back({"invalid json", {Reply{.body = std::string("{")}}, apiKey, std::nullopt, nullptr});
  cases.push_back({"empty body", {Reply{.status = 200}}, apiKey, std::nullopt, nullptr});
  cases.push_back({"oversized page", {pageReply("[]", ",\"pad\":\"" + std::string(2 * 1024 * 1024, 'x') + "\"")}, apiKey, std::nullopt, nullptr});
  std::vector<Reply> limit;
  for (int index = 0; index < 21; ++index) limit.push_back(Reply{.body = R"({"data":[],"has_more":true,"last_id":"c)" + std::to_string(index) + "\"}"});
  cases.push_back({"page limit", limit, apiKey, std::nullopt, nullptr});

  for (const Case& item : cases) {
    const Isolation isolation;
    ScriptedFetch fetch;
    for (const Reply& reply : item.script) fetch.replies->push_back(reply);
    models::DiscoveryOptions options;
    options.fetcher = fetch.fetcher();
    options.now = item.now;
    auto discovering = models::discoverClaudeCodeModels(item.credential, item.workspaceId, options);
    auto running = run(std::move(discovering));
    const json::value outcome = co_await std::move(running);
    json::object actual;
    actual["outcome"] = outcome;
    actual["calls"] = *fetch.calls;
    actual["left"] = fetch.replies->size();
    expectJson(json::value(std::move(actual)), expected.at(item.name), "discovery " + item.name, __FILE__, __LINE__);
  }
}

TEST("reasoning levels list disabled thinking first, then the observed efforts") {
  for (const json::value& item : vectors().at("levels").as_array()) {
    const models::ClaudeCodeModel model = json::value_to<models::ClaudeCodeModel>(item.at("input"));
    json::array levels;
    for (const std::string& level : models::claudeCodeReasoningLevels(model)) levels.emplace_back(json::string(level));
    expectJson(json::value(std::move(levels)), item.at("output"), "levels " + json::serialize(item.at("input")), __FILE__, __LINE__);
  }
}

ASYNC_TEST("the environment scope caches, refreshes, goes stale and drops its cache") {
  const Isolation isolation;
  ScriptedFetch fetch;
  json::array steps;
  auto clock = std::make_shared<double>(1'000);
  const std::function<double()> now = [clock] { return *clock; };
  const auto options = [&](bool force) {
    models::CatalogOptions catalog;
    catalog.kv = std::shared_ptr<rt::Kv>();
    catalog.env = catalogEnv();
    catalog.fetcher = fetch.fetcher();
    catalog.now = now;
    if (force) catalog.force = true;
    return catalog;
  };
  fetch.replies->push_back(pageReply("[" + OPUS + "]"));
  fetch.replies->push_back(pageReply("[" + SONNET + "]"));
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options(false)));
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options(false)));
  *clock += 300'001;
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options(false)));
  fetch.replies->push_back(Reply{.status = 500, .body = std::string("down")});
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options(true)));
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options(false)));
  *clock += 30'001;
  fetch.replies->push_back(Reply{.throws = std::string("offline")});
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options(false)));
  models::CatalogOptions mismatched = options(true);
  mismatched.expectedBinding =
    models::CatalogOptions::ExpectedBinding{.credential = ClaudeCodeAuthHeaders{.kind = "api_key", .secret = "other"}, .workspaceId = "wrkspc_env"};
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(mismatched));
  models::CatalogOptions unconfigured = options(false);
  unconfigured.env = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(unconfigured));
  fetch.replies->push_back(pageReply("[" + HAIKU + "]"));
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options(false)));
  expectJson(json::value(std::move(steps)), vectors().at("scenarios").at("environment scope"), "environment scope", __FILE__, __LINE__);
}

ASYNC_TEST("an account scope follows its binding, validated uploads and roster changes") {
  const Isolation isolation;
  ScriptedFetch fetch;
  json::array steps;
  const std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return NOW; });
  auto clock = std::make_shared<double>(5'000);
  const std::function<double()> now = [clock] { return *clock; };
  const auto options = [&]() {
    models::CatalogOptions catalog;
    catalog.kv = kv;
    catalog.fetcher = fetch.fetcher();
    catalog.now = now;
    return catalog;
  };
  auto resolving = accounts::resolveClaudeCodeAccount(kv, catalogEnv());
  const std::optional<accounts::ClaudeCodeAccountBinding> binding = co_await std::move(resolving);
  ASSERT(binding.has_value());
  fetch.replies->push_back(pageReply("[" + OPUS + "]"));
  models::CatalogOptions bound = options();
  bound.accountBinding = binding;
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(bound));
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options()));
  models::CatalogOptions expectedNull = options();
  expectedNull.expectedBinding =
    models::CatalogOptions::ExpectedBinding{.credential = ClaudeCodeAuthHeaders{.kind = "api_key", .secret = "sk-ant-api-catalog"}, .workspaceId = std::nullopt};
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(expectedNull));
  models::CatalogOptions expectedOther = options();
  expectedOther.expectedBinding =
    models::CatalogOptions::ExpectedBinding{.credential = ClaudeCodeAuthHeaders{.kind = "api_key", .secret = "sk-ant-api-catalog"}, .workspaceId = "wrkspc_x"};
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(expectedOther));
  const models::ClaudeCodeModelsSnapshot uploaded = json::value_to<models::ClaudeCodeModelsSnapshot>(rt::jsonParse(R"({"models":[{"id":"claude-x","created":1}],"updated_at_ms":7})"));
  MODELS_STEP(steps, fetch, models::cacheValidatedClaudeCodeModels(kv, "sk-ant-api-catalog", std::nullopt, uploaded));
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options()));
  const models::ClaudeCodeModelsSnapshot wrong = json::value_to<models::ClaudeCodeModelsSnapshot>(rt::jsonParse(R"({"models":[],"updated_at_ms":8})"));
  MODELS_STEP(steps, fetch, models::cacheValidatedClaudeCodeModels(kv, "wrong", std::nullopt, wrong));
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options()));
  accounts::AccountUpdate moved;
  moved.workspace = credentials::ClaudeCodeWorkspaceState{
    .id = "wrkspc_new", .name = std::nullopt, .source = "kv", .discovered_at_ms = std::nullopt, .verified_at_ms = std::nullopt, .last_error = std::nullopt};
  auto moving = accounts::updateClaudeCodeAccount(kv, moved);
  co_await std::move(moving);
  fetch.replies->push_back(pageReply("[" + SONNET + "]"));
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options()));
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(bound));
  const rt::KvValue emptied(rt::jsonParse(R"({"v":1,"accounts":[],"active_account_id":null,"active_reason":"x","updated_at_ms":1})"));
  auto emptying = kv->set(accounts::CLAUDE_CODE_ACCOUNTS_KEY, emptied);
  co_await std::move(emptying);
  MODELS_STEP(steps, fetch, models::fetchClaudeCodeModels(options()));
  expectJson(json::value(std::move(steps)), vectors().at("scenarios").at("account scope"), "account scope", __FILE__, __LINE__);
}

ASYNC_TEST("concurrent callers share one pending refresh") {
  const Isolation isolation;
  ScriptedFetch fetch;
  const std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return NOW; });
  auto resolving = accounts::resolveClaudeCodeAccount(kv, catalogEnv());
  co_await std::move(resolving);
  fetch.replies->push_back(pageReply("[" + OPUS + "]"));
  models::CatalogOptions options;
  options.kv = kv;
  options.fetcher = fetch.fetcher();
  options.now = [] { return 9'000.0; };
  rt::Promise<std::optional<models::ClaudeCodeModelsSnapshot>> first(models::fetchClaudeCodeModels(options));
  rt::Promise<std::optional<models::ClaudeCodeModelsSnapshot>> second(models::fetchClaudeCodeModels(options));
  const std::optional<models::ClaudeCodeModelsSnapshot> a = co_await first.wait();
  const std::optional<models::ClaudeCodeModelsSnapshot> b = co_await second.wait();
  json::array outcomes;
  outcomes.push_back(plain(a));
  outcomes.push_back(plain(b));
  json::object outcome;
  outcome["ok"] = std::move(outcomes);
  json::object step;
  step["outcome"] = std::move(outcome);
  step["calls"] = *fetch.calls;
  json::array steps;
  steps.emplace_back(std::move(step));
  expectJson(json::value(std::move(steps)), vectors().at("scenarios").at("shared pending refresh"), "shared pending refresh", __FILE__, __LINE__);
}
