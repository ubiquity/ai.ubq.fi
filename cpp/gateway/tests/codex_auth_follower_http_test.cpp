// Links the sources of tests/codex_serial_routing_http_test.cpp
#include "tests/helpers/loopback_http.hpp"
#include "tests/helpers/responses_conformance_client.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/codex/native_auth.hpp"
#include "uos/config.hpp"
#include "uos/handler/serve_handler.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/kv_sqlite.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/runtime/http.hpp"
#include "uos/runtime_config.hpp"
#include "uos/upstream_auth.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <utility>

namespace rt = uos::rt;
namespace json = uos::json;
namespace wire = uos::runtime::http;
namespace auth = uos::codex::auth;
namespace routing = uos::codex::account_routing;

namespace {
  constexpr std::string_view MODEL = "gpt-5.6-sol";
  constexpr std::string_view TOKEN = "synthetic-follower-admin-and-api-token";

  std::string accessToken(double expiryMs) {
    const auto claims = rt::jsonStringify(json::object{{"exp", expiryMs / 1000}});
    return "e30." + uos::utils::base64UrlEncode(rt::encodeUtf8(claims)) + ".synthetic";
  }

  struct Fixture {
    uos::tests::helpers::loopback_http::ScopedLoopbackFetch network;
    const uos::config::Config savedConfig = uos::config::config;
    const std::shared_ptr<rt::Kv> store = std::make_shared<rt::SqliteKv>(":memory:");
    const double now = rt::dateNow();
    uos::types::CodexAuthState first{accessToken(now + 600'000), "synthetic-refresh-first", "follower-first", now};
    const uos::types::CodexAuthState sibling{accessToken(now + 800'000), "synthetic-refresh-sibling", "follower-sibling", now};
    const std::string rotatedAccess = accessToken(now + 1'200'000);
    bool rejectFirst = false;
    bool withSibling = true;
    int tokenCalls = 0;
    std::vector<std::string> upstreamAccounts;
    rt::console::Capture logs{{rt::console::Method::info}};
    std::optional<wire::HttpServer> upstream;
    std::optional<wire::HttpServer> gateway;
    std::optional<rt::GlobalFetchOverride> fetch;
    std::string origin;

    Fixture() {
      uos::tests::helpers::responses_conformance_client::stageGatewayAssets();
      auto& cfg = uos::config::config;
      cfg.isDeploy = true;
      cfg.isProductionTimeline = true;
      cfg.codexAuthJsonB64.clear();
      cfg.authTokens = {std::string(TOKEN)};
      cfg.adminTokens = {std::string(TOKEN)};
      uos::kv::setKvForTest(store);
      resetCaches();
      uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const auto&) {});
    }

    ~Fixture() {
      fetch.reset();
      uos::kv::setKvForTest(nullptr);
      uos::config::config = savedConfig;
      resetCaches();
      uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest(nullptr);
      store->close();
    }

    static void resetCaches() {
      auth::resetCodexAuthCacheForTest();
      routing::resetCodexAccountRoutingForTest();
      uos::runtime_config::resetRuntimeConfigCacheForTest();
      uos::provider::selection::resetProviderSelectionCacheForTest();
      uos::provider::health::resetProviderHealthThrottleForTest();
      uos::models::codex_models_availability::resetCodexAccountModelsCacheForTest();
    }

    rt::Task<rt::Response> script(rt::Request request) {
      const std::string path = rt::URL(request.url()).pathname();
      if (path == "/oauth/token") {
        ++tokenCalls;
        EXPECT_EQ(request.method(), "POST");
        co_return rt::Response::json(json::object{{"access_token", rotatedAccess}, {"refresh_token", "synthetic-rotated-refresh"}});
      }
      if (path != "/codex/responses") co_return rt::Response(nullptr, {.status = 404});
      const auto account = request.headers().get("ChatGPT-Account-ID").value_or("");
      upstreamAccounts.push_back(account);
      if (rejectFirst && account == first.account_id && request.headers().get("Authorization") == "Bearer " + first.access_token)
        co_return rt::Response::json(json::object{{"error", "synthetic bearer rejected"}}, {.status = 401});
      const json::object completed{
        {"id", "resp_follower"}, {"object", "response"}, {"created_at", now / 1000}, {"status", "completed"}, {"model", MODEL},
        {"output", json::array{json::object{{"type", "message"}, {"role", "assistant"}, {"id", "msg_follower"}, {"status", "completed"},
          {"content", json::array{json::object{{"type", "output_text"}, {"text", "served"}, {"annotations", json::array{}}}}}}}},
        {"usage", json::object{{"input_tokens", 2}, {"output_tokens", 1}, {"total_tokens", 3}}},
      };
      const auto events = "data: " + rt::jsonStringify(json::object{{"type", "response.created"}, {"response", json::object{{"id", "resp_follower"}}}}) +
        "\n\ndata: " + rt::jsonStringify(json::object{{"type", "response.output_text.delta"}, {"delta", "served"}}) +
        "\n\ndata: " + rt::jsonStringify(json::object{{"type", "response.completed"}, {"response", completed}}) + "\n\n";
      co_return rt::Response(events, {.headers = rt::Headers{{"Content-Type", "text/event-stream"}}});
    }

    rt::Task<void> start() {
      uos::types::CodexAuthPoolState pool{{first}, now};
      if (withSibling) pool.accounts.push_back(sibling);
      auto writingPool = store->set(auth::CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(pool)));
      co_await std::move(writingPool);
      const json::object catalog{{"source", "codex_cli"}, {"client_version", "0.145.0"}, {"updated_at_ms", now},
        {"models", json::array{json::object{{"slug", MODEL}, {"context_window", 272'000}, {"max_context_window", 1'000'000},
          {"default_reasoning_level", "low"}, {"supported_reasoning_levels", json::array{"none", "low"}}}}}};
      auto writingCatalog = store->set(auth::CODEX_MODELS_KV_KEY, rt::KvValue(catalog));
      co_await std::move(writingCatalog);
      const json::object runtime{{"version", 2}, {"default_model", MODEL}, {"default_reasoning_effort", "low"}, {"codex_models", catalog}, {"updated_at_ms", now}};
      auto writingRuntime = store->set(uos::runtime_config::RUNTIME_CONFIG_V2_KEY, rt::KvValue(runtime));
      co_await std::move(writingRuntime);

      wire::ServeHttpOptions options;
      options.hostname = "127.0.0.1";
      options.port = 0;
      options.onListen = [](const auto&) {};
      upstream = wire::serveHttp(options, [this](rt::Request request, wire::ServeHandlerInfo) { return script(std::move(request)); });
      const auto endpoint = co_await upstream->ready.wait();
      const std::string upstreamOrigin = "http://127.0.0.1:" + std::to_string(static_cast<int>(endpoint.port));
      uos::config::config.codexBaseUrl = upstreamOrigin + "/codex";
      fetch.emplace(rt::Fetch([upstreamOrigin](rt::RequestInfo input, rt::RequestInit init) {
        if (auto* url = std::get_if<std::string>(&input); url && *url == auth::CODEX_REFRESH_TOKEN_URL) *url = upstreamOrigin + "/oauth/token";
        return rt::defaultFetch()(std::move(input), std::move(init));
      }));
      gateway = wire::serveHttp(options, uos::handler::serve_handler::createServeHandler());
      const auto gatewayEndpoint = co_await gateway->ready.wait();
      origin = "http://127.0.0.1:" + std::to_string(static_cast<int>(gatewayEndpoint.port));
    }

    rt::Task<void> stop() {
      if (gateway) co_await gateway->shutdown().wait();
      if (upstream) co_await upstream->shutdown().wait();
      co_await uos::codex::dispatch::awaitPendingCodexProbeTransitions();
    }

    rt::Task<rt::Response> request(std::string path, std::string method = "GET", std::optional<std::string> body = std::nullopt) {
      rt::RequestInit init;
      init.method = std::move(method);
      init.headers = rt::Headers{{"Authorization", "Bearer " + std::string(TOKEN)}, {"Content-Type", "application/json"}};
      if (body) init.body = rt::BodyInit(*body);
      return rt::fetch(origin + path, init);
    }

    rt::Task<void> inference(int expectedStatus) {
      const json::object payload{{"model", MODEL}, {"input", "synthetic follower prompt"}, {"stream", false}};
      auto requesting = request("/v1/responses", "POST", rt::jsonStringify(payload));
      const auto response = co_await std::move(requesting);
      const auto body = co_await response.json();
      EXPECT_EQ(response.status(), expectedStatus);
      if (expectedStatus == 200) EXPECT_EQ(rt::getString(body, "status"), "completed");
      else if (const auto* error = rt::get(body, "error")) EXPECT_EQ(rt::getString(*error, "code"), "codex_auth_invalid");
      else EXPECT(false);
    }

    bool recordedFollowerReason() const {
      return std::any_of(logs.calls().begin(), logs.calls().end(), [](const auto& entry) {
        return entry.text.find("request_terminal") != std::string::npos &&
          entry.text.find("\"upstream_auth_reason\":\"upstream_auth_follower_stale\"") != std::string::npos;
      });
    }
  };

  rt::Task<void> checkInference(std::optional<std::string> mode, bool expired, bool rejected, bool withSibling) {
    rt::env::ScopedOverride follower{{std::string(uos::upstream_auth::FOLLOWER_ENV), mode}};
    Fixture fixture;
    if (expired) fixture.first.access_token = accessToken(fixture.now - 60'000);
    fixture.rejectFirst = rejected;
    fixture.withSibling = withSibling;
    const bool following = mode == "1";
    rt::Reason failure;
    try {
      co_await fixture.start();
      const auto before = co_await fixture.store->get(auth::CODEX_AUTH_POOL_KV_KEY);
      auto calling = fixture.inference(following && !withSibling ? 503 : 200);
      co_await std::move(calling);
      const auto after = co_await fixture.store->get(auth::CODEX_AUTH_POOL_KV_KEY);
      EXPECT_EQ(fixture.tokenCalls, following ? 0 : 1);
      if (following) {
        EXPECT_EQ(after, before);
        const std::vector<std::string> expected = expired
          ? (withSibling ? std::vector{fixture.sibling.account_id} : std::vector<std::string>{})
          : std::vector{fixture.first.account_id, fixture.sibling.account_id};
        EXPECT_EQ(fixture.upstreamAccounts, expected);
      } else {
        EXPECT(after.versionstamp != before.versionstamp);
        EXPECT_EQ(fixture.upstreamAccounts, std::vector{fixture.first.account_id});
      }
    } catch (...) { failure = std::current_exception(); }
    co_await fixture.stop();
    if (failure) std::rethrow_exception(failure);
    EXPECT_EQ(fixture.recordedFollowerReason(), following);
  }
}

ASYNC_TEST("follower skips an expired bearer and serves on the sibling without calling OAuth") {
  auto checking = checkInference("1", true, false, true);
  co_await std::move(checking);
}

ASYNC_TEST("follower skips refresh after a 401 and serves on the sibling") {
  auto checking = checkInference("1", false, true, true);
  co_await std::move(checking);
}

ASYNC_TEST("follower keeps the ordinary client error when the only bearer is stale") {
  auto checking = checkInference("1", true, false, false);
  co_await std::move(checking);
}

ASYNC_TEST("only exact 1 enables follower mode and default refresh calls OAuth once") {
  auto unset = checkInference(std::nullopt, true, false, true);
  co_await std::move(unset);
  auto other = checkInference("1 ", true, false, true);
  co_await std::move(other);
}

ASYNC_TEST("follower health reports time only and admin auth writers return a conflict") {
  rt::env::ScopedOverride follower{{std::string(uos::upstream_auth::FOLLOWER_ENV), "1"}};
  Fixture fixture;
  rt::Reason failure;
  try {
    co_await fixture.start();
    auto checkingHealth = fixture.request("/health");
    const auto response = co_await std::move(checkingHealth);
    const auto body = co_await response.text();
    EXPECT_EQ(response.status(), 200);
    const auto payload = rt::jsonParse(body);
    const auto* status = rt::get(payload, "upstream_auth");
    ASSERT(status);
    EXPECT_EQ(rt::getBool(*status, "follower"), true);
    const auto expiresIn = rt::getNumber(*status, "earliest_access_token_expires_in_ms");
    EXPECT(expiresIn && *expiresIn <= 600'000 && *expiresIn > 590'000);
    EXPECT(body.find("synthetic") == std::string::npos);
    EXPECT(body.find("refresh_sha256") == std::string::npos);

    const auto before = co_await fixture.store->get(auth::CODEX_AUTH_POOL_KV_KEY);
    const std::string poolLine = R"({"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_auth"}],"value":{"type":"string","value":"synthetic-replacement"},"versionstamp":"00000000000000000000"})";
    const std::vector<std::pair<std::string, std::string>> requests{
      {"/admin/codex/auth", "{}"},
      {"/admin/providers/codex/cache-scope-experiment", ""},
      {"/admin/kv-migration/import?profile=local&write=1&overwrite=1", poolLine},
    };
    for (const auto& [path, input] : requests) {
      auto requesting = fixture.request(path, "POST", input);
      const auto denied = co_await std::move(requesting);
      const auto detail = co_await denied.text();
      EXPECT_EQ(denied.status(), 409);
      EXPECT(detail.find(uos::upstream_auth::FOLLOWER_ENV) != std::string::npos);
    }
    const auto after = co_await fixture.store->get(auth::CODEX_AUTH_POOL_KV_KEY);
    EXPECT_EQ(after, before);
    EXPECT_EQ(fixture.tokenCalls, 0);
    EXPECT(fixture.upstreamAccounts.empty());
  } catch (...) { failure = std::current_exception(); }
  co_await fixture.stop();
  if (failure) std::rethrow_exception(failure);
}

ASYNC_TEST("follower leaves native credentials and their app-server untouched") {
  rt::env::ScopedOverride follower{{std::string(uos::upstream_auth::FOLLOWER_ENV), "1"}};
  Fixture fixture;
  bool nativeRead = false;
  bool nativeRefresh = false;
  uos::codex::native_auth::NativeAuthHooks hooks;
  hooks.codexHome = "/fake-codex-home";
  hooks.readAuth = [&nativeRead]() -> rt::Task<json::value> { nativeRead = true; co_return nullptr; };
  hooks.refresh = [&nativeRefresh](auto, auto, auto) -> rt::Task<void> { nativeRefresh = true; co_return; };
  uos::codex::native_auth::setNativeCodexAuthHooksForTest(std::move(hooks));
  const auto clearHooks = rt::scopeExit([] { uos::codex::native_auth::setNativeCodexAuthHooksForTest(std::nullopt); });
  fixture.first.native_owner = uos::types::CodexAuthState::NativeOwner{.codex_home = "/fake-codex-home", .generation_hash = std::string(64, 'a')};
  fixture.rejectFirst = true;
  rt::Reason failure;
  try {
    co_await fixture.start();
    auto inference = fixture.inference(200);
    co_await std::move(inference);
    EXPECT_EQ(fixture.tokenCalls, 0);
    EXPECT(!nativeRead);
    EXPECT(!nativeRefresh);
  } catch (...) { failure = std::current_exception(); }
  co_await fixture.stop();
  if (failure) std::rethrow_exception(failure);
}
