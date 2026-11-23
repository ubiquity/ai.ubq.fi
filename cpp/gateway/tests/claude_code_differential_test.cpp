// Links: tests/claude_code_differential_test.cpp src/provider/claude_code.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/surplus_claude_request.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/responses_stream.cpp src/bounded_response_body.cpp src/inference_deadline.cpp src/utils.cpp
// Differential cases for fetchClaudeCodeResponses and discoverClaudeCodeWorkspace of src/provider/claude-code.ts (their
// direct bun tests need modules of later waves). The vectors (tests/claude_code_differential_vectors.inc) were recorded
// from the TS under bun 1.4.0 with Date.now fixed, crypto.randomUUID a counter, the provider credential variables unset,
// an in-memory Deno KV and scripted fetchers; each scenario compares the outcome (result or error), every fetch the
// module made (url, method, headers, body, redirect, whether a signal was passed) and the KV rows left behind.
#include "uos/provider/claude_code.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/rt/abort.hpp"
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
#include <variant>
#include <vector>

using namespace uos;
namespace cc = provider::claude_code;
namespace accounts = provider::claude_code_accounts;
namespace credentials = provider::claude_code_credentials;
using provider::claude_code::ClaudeCodeEnvLookup;

namespace {
  const char* const VECTORS =
#include "tests/claude_code_differential_vectors.inc"
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

  template <class T> json::value plain(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }

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
    std::vector<std::pair<std::string, std::string>> headers{};
    std::optional<std::string> body{};
    std::optional<std::string> throws{};
    bool hang = false;
  };

  // `new Promise<Response>(() => {})`: a fetch that never settles.
  rt::Task<rt::Response> hang() {
    const rt::Promise<rt::Response> never;
    rt::Response response = co_await never.wait();
    co_return response;
  }

  // `fetcherOf(replies)`: records each call, then answers with the next scripted reply.
  struct ScriptedFetch {
    std::shared_ptr<json::array> calls = std::make_shared<json::array>();
    std::shared_ptr<std::deque<Reply>> replies = std::make_shared<std::deque<Reply>>();

    rt::Fetch fetcher() const {
      auto recorded = calls;
      auto queue = replies;
      return rt::Fetch([recorded, queue](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
        json::object call;
        call["url"] = json::string(std::get<std::string>(input));
        call["method"] = init.method ? json::value(json::string(*init.method)) : json::value(nullptr);
        json::array headers;
        for (const auto& [name, value] : rt::Headers(init.headers.value_or(rt::Headers())).entries()) {
          json::array pair;
          pair.emplace_back(json::string(name));
          pair.emplace_back(json::string(value));
          headers.emplace_back(std::move(pair));
        }
        call["headers"] = std::move(headers);
        const std::string* text = init.body ? std::get_if<std::string>(&*init.body) : nullptr;
        call["body"] = text != nullptr ? json::value(json::string(*text)) : json::value(nullptr);
        call["redirect"] = init.redirect ? json::value(json::string(*init.redirect)) : json::value(nullptr);
        call["signal"] = init.signal.has_value();
        recorded->emplace_back(std::move(call));
        if (queue->empty()) throw rt::JsError("no reply left");
        const Reply reply = queue->front();
        queue->pop_front();
        if (reply.throws) throw rt::TypeError(*reply.throws);
        if (reply.hang) return hang();
        rt::ResponseInit responseInit;
        responseInit.status = reply.status;
        responseInit.headers = rt::Headers(reply.headers);
        rt::Response response = reply.body ? rt::Response(rt::BodyInit(*reply.body), responseInit) : rt::Response(nullptr, responseInit);
        return rt::fulfilledTask(std::move(response));
      });
    }
  };

  json::value errorOf(const rt::Reason& reason) {
    json::object out;
    try {
      std::rethrow_exception(reason);
    } catch (const cc::ClaudeCodeError& error) {
      out["name"] = json::string(error.name);
      out["message"] = json::string(error.message);
      out["code"] = json::string(error.code);
      out["status"] = rt::makeNumber(error.status);
      out["upstream_status"] = error.upstream_status ? rt::makeNumber(*error.upstream_status) : json::value(nullptr);
      return out;
    } catch (const rt::JsError& error) {
      out["name"] = json::string(error.name);
      out["message"] = json::string(error.message);
      out["code"] = error.code ? json::value(json::string(*error.code)) : json::value(nullptr);
      out["status"] = nullptr;
      out["upstream_status"] = nullptr;
      return out;
    } catch (...) {
      out["thrown"] = rt::reasonToString(reason);
      return out;
    }
  }

  rt::Task<json::value> resultOf(cc::ClaudeCodeResponsesResult result) {
    json::object out;
    out["account_binding"] = plain(result.account_binding);
    out["observed_at_ms"] = rt::makeNumber(result.observed_at_ms);
    out["request_id"] = result.request_id ? json::value(json::string(*result.request_id)) : json::value(nullptr);
    out["credential"] = json::value_from(result.credential);
    out["workspace"] = plain(result.workspace);
    out["prompt_cache_mode"] = result.prompt_cache_mode ? json::value(json::string(*result.prompt_cache_mode)) : json::value(nullptr);
    out["explicit_breakpoint_count"] = result.explicit_breakpoint_count ? rt::makeNumber(*result.explicit_breakpoint_count) : json::value(nullptr);
    out["quota"] = plain(result.quota);
    json::object classification;
    classification["kind"] = json::string(result.classification.kind);
    if (result.classification.exhaustion) {
      json::object exhaustion;
      exhaustion["window"] = json::string(result.classification.exhaustion->window);
      exhaustion["reset_at_ms"] =
        result.classification.exhaustion->reset_at_ms ? rt::makeNumber(*result.classification.exhaustion->reset_at_ms) : json::value(nullptr);
      classification["exhaustion"] = std::move(exhaustion);
    } else {
      classification["exhaustion"] = nullptr;
    }
    classification["retry_after_ms"] = result.classification.retry_after_ms ? rt::makeNumber(*result.classification.retry_after_ms) : json::value(nullptr);
    classification["detail"] = result.classification.detail ? json::value(json::string(*result.classification.detail)) : json::value(nullptr);
    out["classification"] = std::move(classification);
    json::object response;
    response["status"] = result.response.status();
    json::array headers;
    for (const auto& [name, value] : result.response.headers().entries()) {
      json::array pair;
      pair.emplace_back(json::string(name));
      pair.emplace_back(json::string(value));
      headers.emplace_back(std::move(pair));
    }
    response["headers"] = std::move(headers);
    const std::string text = co_await result.response.text();
    response["body"] = json::string(text);
    out["response"] = std::move(response);
    co_return json::value(std::move(out));
  }

  json::value discoveryResultOf(const cc::ClaudeCodeWorkspaceDiscoveryResult& result) {
    json::object out;
    if (const auto* found = std::get_if<cc::ClaudeCodeWorkspaceDiscoveryResultOk>(&result)) {
      out["ok"] = true;
      out["workspace"] = json::value_from(found->workspace);
      if (found->account_binding) out["account_binding"] = json::value_from(*found->account_binding);
    } else {
      const auto& failed = std::get<cc::ClaudeCodeWorkspaceDiscoveryResultNotOk>(result);
      out["ok"] = false;
      out["status"] = failed.status ? rt::makeNumber(*failed.status) : json::value(nullptr);
      out["error"] = failed.error ? json::value(json::string(*failed.error)) : json::value(nullptr);
    }
    return out;
  }

  // The KV rows the generator printed: every key the module can write, by `JSON.stringify(key)`.
  rt::Task<json::value> rowsOf(std::shared_ptr<rt::Kv> kv) {
    json::object rows;
    std::vector<rt::KvKey> keys{credentials::CLAUDE_CODE_AUTH_KV_KEY, credentials::CLAUDE_CODE_WORKSPACE_KV_KEY, accounts::CLAUDE_CODE_ACCOUNTS_KEY,
                                provider::claude_code_quota::CLAUDE_CODE_QUOTA_KV_KEY};
    for (const rt::KvKey& key : keys) {
      const rt::KvEntryMaybe<> entry = co_await kv->get(key);
      if (!entry.versionstamp) continue;
      json::array parts;
      for (const rt::KvKeyPart& part : key) parts.emplace_back(json::string(part.string()));
      rows[rt::jsonStringify(json::value(std::move(parts)))] = entry.value.toJson();
    }
    co_return json::value(std::move(rows));
  }

  struct Scenario {
    Scenario(std::string_view section_, std::string_view name_)
      : name(name_),
        expected(vectors().at(section_).at(name_).as_object()),
        env({{"CLAUDE_API_KEY", std::nullopt}, {"CLAUDE_CODE_OAUTH_TOKEN", std::nullopt}, {"CLAUDE_CODE_WORKSPACE_ID", std::nullopt}}),
        random(uuidSource()),
        kv(std::make_shared<rt::MemoryKv>([] { return NOW; })) {
      rt::globalDateNow() = [] { return NOW; };
      credentials::resetClaudeCodeCredentialCacheForTest();
    }
    rt::Task<void> finish(json::value outcome, const char* file, int line) {
      expectJson(outcome, expected.at("outcome"), name + " outcome", file, line);
      expectJson(json::value(*fetch.calls), expected.at("calls"), name + " calls", file, line);
      auto reading = rowsOf(kv);
      const json::value rows = co_await std::move(reading);
      expectJson(rows, expected.at("rows"), name + " rows", file, line);
    }

    std::string name;
    const json::object& expected;
    rt::env::ScopedOverride env;
    rt::crypto::ScopedRandomSource random;
    std::shared_ptr<rt::Kv> kv;
    ScriptedFetch fetch;
  };

  json::value okOf(json::value value) {
    json::object out;
    out["ok"] = std::move(value);
    return out;
  }
  json::value errorOutcome(const rt::Reason& reason) {
    json::object out;
    out["error"] = errorOf(reason);
    return out;
  }

  // `{ ok: await resultOf(await cc.fetchClaudeCodeResponses(body, options)) }` or `{ error }`.
  rt::Task<json::value> fetchOutcome(json::value body, cc::ClaudeCodeAuthenticatedFetchOptions options) {
    rt::Reason failure;
    json::value out;
    try {
      auto call = cc::fetchClaudeCodeResponses(std::move(body), std::move(options));
      cc::ClaudeCodeResponsesResult result = co_await std::move(call);
      auto describing = resultOf(std::move(result));
      json::value described = co_await std::move(describing);
      out = okOf(std::move(described));
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) out = errorOutcome(failure);
    co_return out;
  }

  rt::Task<json::value> discoverOutcome(cc::DiscoverClaudeCodeWorkspaceOptions options) {
    rt::Reason failure;
    json::value out;
    try {
      auto call = cc::discoverClaudeCodeWorkspace(std::move(options));
      const cc::ClaudeCodeWorkspaceDiscoveryResult result = co_await std::move(call);
      out = okOf(discoveryResultOf(result));
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) out = errorOutcome(failure);
    co_return out;
  }

  const credentials::ClaudeCodeResolvedCredential OAUTH{
    .kind = "oauth", .secret = "sk-ant-oat-fixture", .hint = "configured", .label = std::nullopt, .source = "kv", .updated_at_ms = 5};
  const credentials::ClaudeCodeResolvedCredential API_KEY{
    .kind = "api_key", .secret = "sk-ant-api-fixture", .hint = "configured", .label = std::nullopt, .source = "CLAUDE_API_KEY", .updated_at_ms = std::nullopt};

  json::value body() { return rt::jsonParse(R"({"model":"claude-opus-5-5","input":"hello","instructions":"be brief","stream":false,"prompt_cache_key":"affinity"})"); }

  std::string anthropicMessage(std::string_view text) {
    json::object message = rt::jsonParse(R"({"id":"msg_01","type":"message","role":"assistant","model":"claude-opus-5-5","content":[],"stop_reason":"end_turn",)"
                                          R"("stop_sequence":null,"usage":{"input_tokens":10,"output_tokens":2}})")
                             .as_object();
    json::object block;
    block["type"] = "text";
    block["text"] = json::string(text);
    message["content"].as_array().emplace_back(std::move(block));
    return rt::jsonStringify(json::value(std::move(message)));
  }

  std::vector<std::pair<std::string, std::string>> jsonHeaders() { return {{"content-type", "application/json"}, {"request-id", "req_123"}}; }

  Reply workspaceRequired() {
    return {.status = 400,
            .headers = {{"content-type", "application/json"}},
            .body = std::string(R"({"type":"error","error":{"type":"invalid_request_error","message":"This API key is not scoped to a workspace, so this request must include the anthropic-workspace-id header."}})")};
  }
  Reply workspaceList() {
    return {.status = 200,
            .headers = {{"content-type", "application/json"}},
            .body = std::string(R"({"data":[{"id":"wrkspc_archived","name":"Default","archived_at":"2026-01-01T00:00:00Z"},{"id":"nope","name":"Default"},)"
                                R"({"id":"wrkspc_first","name":"First"},{"id":" wrkspc_default ","name":"  DEFAULT "}]})")};
  }

  ClaudeCodeEnvLookup envOf(std::vector<std::pair<std::string, std::string>> values) {
    return [values](const std::string& name) -> std::optional<std::string> {
      for (const auto& [key, value] : values)
        if (key == name) return value;
      return std::nullopt;
    };
  }

#define RUN_FETCH(scenario, bodyValue, options)                                \
  do {                                                                         \
    auto uosTask = fetchOutcome((bodyValue), (options));                       \
    json::value uosOutcome = co_await std::move(uosTask);                      \
    auto uosFinish = (scenario).finish(std::move(uosOutcome), __FILE__, __LINE__); \
    co_await std::move(uosFinish);                                             \
  } while (0)

#define RUN_DISCOVER(scenario, options)                                        \
  do {                                                                         \
    auto uosTask = discoverOutcome((options));                                 \
    json::value uosOutcome = co_await std::move(uosTask);                      \
    auto uosFinish = (scenario).finish(std::move(uosOutcome), __FILE__, __LINE__); \
    co_await std::move(uosFinish);                                             \
  } while (0)
}

ASYNC_TEST("non-object, array and non-Claude bodies, and a missing credential, fail before any fetch") {
  {
    Scenario s("scenarios", "non-object body");
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    RUN_FETCH(s, json::value("text"), options);
  }
  {
    Scenario s("scenarios", "array body");
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    options.workspace = nullptr;
    RUN_FETCH(s, rt::jsonParse("[1]"), options);
  }
  {
    Scenario s("scenarios", "missing credential");
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.env = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
    RUN_FETCH(s, body(), options);
  }
  {
    Scenario s("scenarios", "non-claude model");
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    json::value other = body();
    other.as_object()["model"] = "gpt-5";
    RUN_FETCH(s, other, options);
  }
}

ASYNC_TEST("successful attempts carry the credential wire, workspace and normalized response") {
  {
    Scenario s("scenarios", "oauth success");
    s.fetch.replies->push_back({.status = 200, .headers = jsonHeaders(), .body = anthropicMessage("hi there")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    options.workspace = nullptr;
    options.fetcher = s.fetch.fetcher();
    RUN_FETCH(s, body(), options);
  }
  {
    Scenario s("scenarios", "api key with kv workspace");
    const rt::KvValue pinned(rt::jsonParse(R"({"id":"wrkspc_kv","name":"Pinned","source":"kv","discovered_at_ms":null,"verified_at_ms":null,"last_error":null})"));
    auto seeding = s.kv->set(credentials::CLAUDE_CODE_WORKSPACE_KV_KEY, pinned);
    co_await std::move(seeding);
    s.fetch.replies->push_back({.status = 200, .headers = {{"content-type", "application/json"}, {"x-request-id", " req_x "}}, .body = anthropicMessage("ok")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = API_KEY;
    options.kv = s.kv;
    options.fetcher = s.fetch.fetcher();
    RUN_FETCH(s, rt::jsonParse(R"({"model":"claude-sonnet-5-5","input":"hi","max_output_tokens":128})"), options);
  }
  {
    Scenario s("scenarios", "env credential and env workspace");
    s.fetch.replies->push_back({.status = 200, .headers = jsonHeaders(), .body = anthropicMessage("env")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.env = envOf({{"CLAUDE_API_KEY", "sk-ant-api-env"}, {"CLAUDE_CODE_WORKSPACE_ID", "wrkspc_env"}});
    options.fetcher = s.fetch.fetcher();
    RUN_FETCH(s, body(), options);
  }
}

ASYNC_TEST("rejections are classified and transport failures become upstream_unreachable") {
  {
    Scenario s("scenarios", "durable exhaustion");
    s.fetch.replies->push_back({.status = 429,
                                .headers = {{"content-type", "application/json"},
                                            {"anthropic-ratelimit-unified-5h-status", "rejected"},
                                            {"anthropic-ratelimit-unified-5h-reset", "1700018000"},
                                            {"anthropic-ratelimit-unified-5h-utilization", "1"}},
                                .body = std::string(R"({"type":"error","error":{"type":"rate_limit_error","message":"Rate limited"}})")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    options.workspace = nullptr;
    options.fetcher = s.fetch.fetcher();
    RUN_FETCH(s, body(), options);
  }
  {
    Scenario s("scenarios", "transient with retry-after");
    s.fetch.replies->push_back({.status = 429, .headers = {{"retry-after", "12"}}, .body = std::string("slow   down\n please")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    options.workspace = nullptr;
    options.fetcher = s.fetch.fetcher();
    RUN_FETCH(s, body(), options);
  }
  {
    Scenario s("scenarios", "billing rejection");
    s.fetch.replies->push_back({.status = 400,
                                .headers = {{"content-type", "application/json"}},
                                .body = std::string(R"({"type":"error","error":{"type":"invalid_request_error","message":"Your credit balance is too low"}})")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = API_KEY;
    options.workspace = nullptr;
    options.fetcher = s.fetch.fetcher();
    RUN_FETCH(s, body(), options);
  }
  {
    Scenario s("scenarios", "transport failure");
    s.fetch.replies->push_back({.throws = std::string("socket hang up")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    options.workspace = nullptr;
    options.fetcher = s.fetch.fetcher();
    RUN_FETCH(s, body(), options);
  }
}

ASYNC_TEST("a workspace requirement discovers one workspace and retries once") {
  {
    Scenario s("scenarios", "workspace discovery without binding");
    s.fetch.replies->push_back(workspaceRequired());
    s.fetch.replies->push_back(workspaceList());
    s.fetch.replies->push_back({.status = 200, .headers = jsonHeaders(), .body = anthropicMessage("after discovery")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = API_KEY;
    options.kv = s.kv;
    options.fetcher = s.fetch.fetcher();
    RUN_FETCH(s, rt::jsonParse(R"({"model":"claude-opus-5-5","input":"hello"})"), options);
  }
  {
    Scenario s("scenarios", "workspace discovery with binding");
    auto resolving = accounts::resolveClaudeCodeAccount(s.kv, envOf({{"CLAUDE_API_KEY", "sk-ant-api-bound"}}));
    const std::optional<accounts::ClaudeCodeAccountBinding> binding = co_await std::move(resolving);
    ASSERT(binding.has_value());
    s.fetch.replies->push_back(workspaceRequired());
    s.fetch.replies->push_back(workspaceList());
    s.fetch.replies->push_back({.status = 200, .headers = jsonHeaders(), .body = anthropicMessage("bound")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.accountBinding = binding;
    options.credential = binding->credential;
    options.kv = s.kv;
    options.fetcher = s.fetch.fetcher();
    RUN_FETCH(s, rt::jsonParse(R"({"model":"claude-opus-5-5","input":"hello"})"), options);
  }
  {
    Scenario s("scenarios", "workspace discovery fails");
    s.fetch.replies->push_back(workspaceRequired());
    s.fetch.replies->push_back({.status = 403, .headers = {}, .body = std::string("  forbidden \n listing ")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = API_KEY;
    options.kv = s.kv;
    options.fetcher = s.fetch.fetcher();
    RUN_FETCH(s, rt::jsonParse(R"({"model":"claude-opus-5-5","input":"hello"})"), options);
  }
  {
    Scenario s("scenarios", "disabled thinking keeps the first failure");
    s.fetch.replies->push_back(workspaceRequired());
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = API_KEY;
    options.kv = s.kv;
    options.supportsNativeDisabledThinking = true;
    options.fetcher = s.fetch.fetcher();
    RUN_FETCH(s, rt::jsonParse(R"({"model":"claude-opus-5-5","input":"hello","reasoning":{"effort":"none"}})"), options);
  }
}

ASYNC_TEST("dispatch hooks run once around the transport and an abort cancels before it") {
  {
    Scenario s("scenarios", "dispatch hooks");
    s.fetch.replies->push_back({.status = 200, .headers = jsonHeaders(), .body = anthropicMessage("hooks")});
    auto events = std::make_shared<json::array>();
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    options.workspace = credentials::ClaudeCodeResolvedWorkspace{.id = "wrkspc_given", .name = std::nullopt, .source = "env"};
    options.fetcher = s.fetch.fetcher();
    options.beforeDispatch = [events]() {
      events->emplace_back("before");
      api_key_policy::ApiKeyProviderDispatch dispatch{
        .markTransportStarted = [events] { events->emplace_back("started"); },
        .cancelBeforeTransport = [events] {
          events->emplace_back("cancel");
          return rt::fulfilledTask();
        },
      };
      return rt::fulfilledTask(std::optional<api_key_policy::ApiKeyProviderDispatch>(dispatch));
    };
    options.onDispatch = [events] { events->emplace_back("dispatch"); };
    auto call = fetchOutcome(body(), options);
    const json::value outcome = co_await std::move(call);
    json::object combined;
    combined["events"] = *events;
    if (const json::value* result = rt::get(outcome, "ok")) combined["result"] = *result;
    else combined["error"] = *rt::get(outcome, "error");
    auto finishing = s.finish(okOf(json::value(std::move(combined))), __FILE__, __LINE__);
    co_await std::move(finishing);
  }
  {
    Scenario s("scenarios", "aborted before transport");
    s.fetch.replies->push_back({.status = 200, .headers = jsonHeaders(), .body = anthropicMessage("never")});
    auto events = std::make_shared<json::array>();
    const rt::AbortController controller;
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    options.workspace = nullptr;
    options.signal = controller.signal();
    options.fetcher = s.fetch.fetcher();
    options.beforeDispatch = [events, controller]() {
      events->emplace_back("before");
      controller.abort(rt::makeReason(rt::JsError("caller went away")));
      api_key_policy::ApiKeyProviderDispatch dispatch{
        .markTransportStarted = [events] { events->emplace_back("started"); },
        .cancelBeforeTransport = [events] {
          events->emplace_back("cancel");
          return rt::fulfilledTask();
        },
      };
      return rt::fulfilledTask(std::optional<api_key_policy::ApiKeyProviderDispatch>(dispatch));
    };
    auto call = fetchOutcome(body(), options);
    const json::value outcome = co_await std::move(call);
    json::object combined;
    combined["events"] = *events;
    if (const json::value* error = rt::get(outcome, "error")) combined["error"] = *error;
    auto finishing = s.finish(okOf(json::value(std::move(combined))), __FILE__, __LINE__);
    co_await std::move(finishing);
  }
}

TEST("abandoning a Claude Code preheader request releases its combined abort signal") {
  const rt::AbortController caller;
  std::weak_ptr<rt::detail::AbortSignalState> signalState;
  {
    rt::FakeClock clock;
    rt::Loop loop(clock);
    const rt::LoopScope scope(loop);
    const auto entered = rt::withResolvers();
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = API_KEY;
    options.workspace = nullptr;
    options.signal = caller.signal();
    options.fetcher = rt::Fetch([&signalState, start = entered.resolve](rt::RequestInfo, rt::RequestInit init) {
      if (init.signal) signalState = init.signal->state();
      start();
      return hang();
    });
    const rt::Promise<cc::ClaudeCodeResponsesResult> pending(cc::fetchClaudeCodeResponses(body(), std::move(options)));
    rt::blockOn(loop, entered.promise.wait());
    EXPECT(pending.pending());
    EXPECT(!signalState.expired());
  }
  EXPECT(signalState.expired());
}

ASYNC_TEST("workspace discovery reports failures as data and stores discovered state") {
  const cc::ClaudeCodeAuthHeaders auth{.kind = "api_key", .secret = "sk-ant-api-discover"};
  {
    Scenario s("discovery", "fetch throws");
    s.fetch.replies->push_back({.throws = std::string("getaddrinfo ENOTFOUND")});
    cc::DiscoverClaudeCodeWorkspaceOptions options{.credential = auth};
    options.fetcher = s.fetch.fetcher();
    RUN_DISCOVER(s, options);
  }
  {
    Scenario s("discovery", "non-ok");
    s.fetch.replies->push_back({.status = 401, .body = std::string("  bad \t token ")});
    cc::DiscoverClaudeCodeWorkspaceOptions options{.credential = cc::ClaudeCodeAuthHeaders{.kind = "oauth", .secret = "t"}};
    options.fetcher = s.fetch.fetcher();
    RUN_DISCOVER(s, options);
  }
  {
    Scenario s("discovery", "empty list");
    s.fetch.replies->push_back({.status = 200, .body = std::string(R"({"data":[]})")});
    cc::DiscoverClaudeCodeWorkspaceOptions options{.credential = auth};
    options.fetcher = s.fetch.fetcher();
    RUN_DISCOVER(s, options);
  }
  {
    Scenario s("discovery", "not json");
    s.fetch.replies->push_back({.status = 200, .body = std::string("<html>")});
    cc::DiscoverClaudeCodeWorkspaceOptions options{.credential = auth};
    options.fetcher = s.fetch.fetcher();
    RUN_DISCOVER(s, options);
  }
  {
    Scenario s("discovery", "first entry without kv");
    s.fetch.replies->push_back({.status = 200, .body = std::string(R"({"data":[{"id":"wrkspc_a","name":null},{"id":"wrkspc_b","name":"b"}]})")});
    cc::DiscoverClaudeCodeWorkspaceOptions options{.credential = auth};
    options.fetcher = s.fetch.fetcher();
    RUN_DISCOVER(s, options);
  }
  {
    Scenario s("discovery", "stores discovered state");
    const rt::KvValue old(rt::jsonParse(R"({"id":"wrkspc_default","name":"Old","source":"discovered","discovered_at_ms":3,"verified_at_ms":4,"last_error":"x"})"));
    auto seeding = s.kv->set(credentials::CLAUDE_CODE_WORKSPACE_KV_KEY, old);
    co_await std::move(seeding);
    s.fetch.replies->push_back(workspaceList());
    cc::DiscoverClaudeCodeWorkspaceOptions options{.credential = auth};
    options.kv = s.kv;
    options.fetcher = s.fetch.fetcher();
    RUN_DISCOVER(s, options);
  }
  {
    Scenario s("discovery", "stale binding");
    auto resolving = accounts::resolveClaudeCodeAccount(s.kv, envOf({{"CLAUDE_API_KEY", "sk-ant-api-x"}}));
    const std::optional<accounts::ClaudeCodeAccountBinding> binding = co_await std::move(resolving);
    ASSERT(binding.has_value());
    accounts::AccountUpdate rotate;
    credentials::ClaudeCodeResolvedCredential rotated = binding->credential;
    rotated.secret = "sk-ant-api-rotated";
    rotate.credential = rotated;
    auto updating = accounts::updateClaudeCodeAccount(s.kv, rotate);
    co_await std::move(updating);
    s.fetch.replies->push_back(workspaceList());
    cc::DiscoverClaudeCodeWorkspaceOptions options{.credential = auth};
    options.kv = s.kv;
    options.accountBinding = binding;
    options.fetcher = s.fetch.fetcher();
    RUN_DISCOVER(s, options);
  }
  {
    Scenario s("discovery", "invalid roster");
    const rt::KvValue invalid(rt::jsonParse(R"({"v":1,"accounts":"bad"})"));
    auto seeding = s.kv->set(accounts::CLAUDE_CODE_ACCOUNTS_KEY, invalid);
    co_await std::move(seeding);
    s.fetch.replies->push_back(workspaceList());
    cc::DiscoverClaudeCodeWorkspaceOptions options{.credential = auth};
    options.kv = s.kv;
    options.fetcher = s.fetch.fetcher();
    RUN_DISCOVER(s, options);
  }
}

ASYNC_TEST("a caller abort during the fetch rejects with the caller's reason") {
  {
    Scenario s("scenarios", "abort during fetch");
    s.fetch.replies->push_back({.hang = true});
    const rt::AbortController controller;
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    options.workspace = nullptr;
    options.signal = controller.signal();
    options.fetcher = s.fetch.fetcher();
    auto call = fetchOutcome(body(), options);
    rt::Promise<json::value> pending(std::move(call));
    rt::setTimeout([controller] { controller.abort(rt::makeReason(rt::JsError("caller went away"))); }, 5);
    json::value outcome = co_await pending.wait();
    auto finishing = s.finish(std::move(outcome), __FILE__, __LINE__);
    co_await std::move(finishing);
  }
  {
    Scenario s("scenarios", "abort during fetch with a string reason");
    s.fetch.replies->push_back({.hang = true});
    const rt::AbortController controller;
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    options.workspace = nullptr;
    options.signal = controller.signal();
    options.fetcher = s.fetch.fetcher();
    auto call = fetchOutcome(body(), options);
    rt::Promise<json::value> pending(std::move(call));
    rt::setTimeout([controller] { controller.abort(rt::thrown(json::value("bye"))); }, 5);
    json::value outcome = co_await pending.wait();
    auto finishing = s.finish(std::move(outcome), __FILE__, __LINE__);
    co_await std::move(finishing);
  }
}

ASYNC_TEST("a sentinel upstream recorder wraps the response or records the fetch error") {
  auto events = std::make_shared<json::array>();
  sentinel::upstream_capture::SentinelUpstreamRecorder recorder;
  recorder.startAttempt = [events](sentinel::upstream_capture::SentinelUpstreamProvider provider) {
    events->emplace_back(json::string("start:" + provider));
    sentinel::upstream_capture::SentinelUpstreamAttemptHandle handle;
    handle.wrap = [events](rt::Response response) {
      events->emplace_back(json::string("wrap:" + std::to_string(response.status())));
      return response;
    };
    handle.recordFetchError = [events] { events->emplace_back("fetch-error"); };
    return handle;
  };
  // snapshotAndSeal and dispose stay empty: the module never calls them.
  {
    Scenario s("scenarios", "sentinel recorder success");
    events->clear();
    s.fetch.replies->push_back({.status = 200, .headers = jsonHeaders(), .body = anthropicMessage("recorded")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    options.workspace = nullptr;
    options.sentinelUpstreamRecorder = recorder;
    options.fetcher = s.fetch.fetcher();
    auto call = fetchOutcome(body(), options);
    const json::value outcome = co_await std::move(call);
    json::object combined;
    combined["events"] = *events;
    if (const json::value* result = rt::get(outcome, "ok")) combined["result"] = *result;
    else combined["error"] = *rt::get(outcome, "error");
    auto finishing = s.finish(okOf(json::value(std::move(combined))), __FILE__, __LINE__);
    co_await std::move(finishing);
  }
  {
    Scenario s("scenarios", "sentinel recorder fetch error");
    events->clear();
    s.fetch.replies->push_back({.throws = std::string("reset")});
    cc::ClaudeCodeAuthenticatedFetchOptions options;
    options.credential = OAUTH;
    options.workspace = nullptr;
    options.sentinelUpstreamRecorder = recorder;
    options.fetcher = s.fetch.fetcher();
    auto call = fetchOutcome(body(), options);
    const json::value outcome = co_await std::move(call);
    json::object combined;
    combined["events"] = *events;
    if (const json::value* error = rt::get(outcome, "error")) combined["error"] = *error;
    auto finishing = s.finish(okOf(json::value(std::move(combined))), __FILE__, __LINE__);
    co_await std::move(finishing);
  }
}
