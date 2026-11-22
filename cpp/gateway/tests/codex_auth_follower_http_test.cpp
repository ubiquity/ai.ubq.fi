// Links the sources of tests/codex_serial_routing_http_test.cpp
#include "tests/helpers/loopback_http.hpp"
#include "tests/helpers/responses_conformance_client.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/auth_import.hpp"
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
#include "uos/rt/kv_js.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/runtime/http.hpp"
#include "uos/runtime/permissions_linux.hpp"
#include "uos/runtime_config.hpp"
#include "uos/storage/kv.hpp"
#include "uos/upstream_auth.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <utility>

#if defined(__linux__) && defined(__aarch64__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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
    const std::shared_ptr<rt::Kv> store;
    const double now = rt::dateNow();
    uos::types::CodexAuthState first{accessToken(now + 600'000), "synthetic-refresh-first", "follower-first", now};
    const uos::types::CodexAuthState sibling{accessToken(now + 800'000), "synthetic-refresh-sibling", "follower-sibling", now};
    const std::string rotatedAccess = accessToken(now + 1'200'000);
    bool rejectFirst = false;
    bool withSibling = true;
    bool failSiblingStreamOnce = false;
    int tokenCalls = 0;
    std::function<rt::Response(const rt::Request&)> usage;
    std::string rejectedAccess;
    std::vector<std::string> upstreamAccounts;
    rt::console::Capture logs{{rt::console::Method::info}};
    std::optional<wire::HttpServer> upstream;
    std::optional<wire::HttpServer> gateway;
    std::optional<rt::GlobalFetchOverride> fetch;
    std::string origin;

    explicit Fixture(std::shared_ptr<rt::Kv> database = std::make_shared<rt::SqliteKv>(":memory:")) : store(std::move(database)) {
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
      if (usage) auth::setLeaderAuthReader({});
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
      if (path == "/backend-api/wham/usage" && usage) co_return usage(request);
      if (path != "/codex/responses") co_return rt::Response(nullptr, {.status = 404});
      const auto account = request.headers().get("ChatGPT-Account-ID").value_or("");
      upstreamAccounts.push_back(account);
      if (!rejectedAccess.empty() && request.headers().get("Authorization") == "Bearer " + rejectedAccess)
        co_return rt::Response::json(json::object{{"error", "synthetic bearer rejected"}}, {.status = 401});
      if (rejectFirst && account == first.account_id && request.headers().get("Authorization") == "Bearer " + first.access_token)
        co_return rt::Response::json(json::object{{"error", "synthetic bearer rejected"}}, {.status = 401});
      if (account == sibling.account_id && std::exchange(failSiblingStreamOnce, false)) {
        co_return rt::Response(
          R"(data: {"type":"response.created","response":{"id":"resp_abandoned_follower"}})" "\n\n"
          R"(data: {"type":"error","code":"server_error","message":"synthetic stream failure","param":null})" "\n\n",
          {.headers = rt::Headers{{"Content-Type", "text/event-stream"}}});
      }
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
        if (auto* url = std::get_if<std::string>(&input); url && *url == upstreamOrigin + "/backend-api/wham/usage")
          EXPECT_EQ(init.redirect, "manual");
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

ASYNC_TEST("follower failover and pre-output stream retry share one terminal record") {
  rt::env::ScopedOverride follower{{std::string(uos::upstream_auth::FOLLOWER_ENV), "1"}};
  Fixture fixture;
  fixture.rejectFirst = true;
  fixture.failSiblingStreamOnce = true;
  rt::Reason failure;
  try {
    co_await fixture.start();
    const auto before = co_await fixture.store->get(auth::CODEX_AUTH_POOL_KV_KEY);
    const json::object payload{{"model", MODEL}, {"input", "synthetic follower retry"}, {"stream", true}};
    auto requesting = fixture.request("/v1/responses", "POST", rt::jsonStringify(payload));
    const auto response = co_await std::move(requesting);
    const auto body = co_await response.text();
    EXPECT_EQ(response.status(), 200);
    EXPECT(body.find("resp_abandoned_follower") == std::string::npos);
    EXPECT(body.find("response.completed") != std::string::npos);
    const std::vector expected{fixture.first.account_id, fixture.sibling.account_id, fixture.sibling.account_id};
    EXPECT_EQ(fixture.upstreamAccounts, expected);
    EXPECT_EQ(fixture.tokenCalls, 0);
    const auto after = co_await fixture.store->get(auth::CODEX_AUTH_POOL_KV_KEY);
    EXPECT_EQ(after, before);
  } catch (...) { failure = std::current_exception(); }
  co_await fixture.stop();
  if (failure) std::rethrow_exception(failure);

  std::vector<json::value> terminals;
  for (const auto& call : fixture.logs.calls()) {
    if (call.args.size() > 1 && call.args[0] == "[ai.ubq.fi] request_terminal") terminals.push_back(rt::jsonParse(call.args[1].string()));
  }
  ASSERT_EQ(terminals.size(), 1U);
  EXPECT_EQ(rt::getString(terminals.front(), "upstream_auth_reason"), uos::upstream_auth::FOLLOWER_STALE);
  EXPECT_EQ(rt::getNumber(terminals.front(), "in_stream_retries"), 1);
  const auto* failures = rt::getArray(terminals.front(), "upstream_failure_events");
  ASSERT(failures);
  ASSERT_EQ(failures->size(), 1U);
  EXPECT_EQ(rt::getString(failures->front(), "code"), "server_error");
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

#if defined(__linux__) && defined(__aarch64__)
namespace {
  namespace fs = std::filesystem;
  namespace sqlite = rt::sqlite;
  namespace permissions = uos::runtime::permissions_linux;

  constexpr auto LEADER_WAIT_MS = 5'025;

  char readCommand(int socket, int timeout) {
    pollfd descriptor{socket, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, timeout);
    if (ready == 0) return '\0';
    char command = 0;
    if (ready < 0 || ::recv(socket, &command, 1, 0) != 1) throw rt::JsError("Leader fixture command channel closed");
    return command;
  }

  void sendCommand(int socket, char command) {
    if (::send(socket, &command, 1, MSG_NOSIGNAL) != 1) throw rt::JsError("Leader fixture could not send a command");
  }

  struct LiveWalFixture {
    const fs::path root = rt::fs::mkdtempSync(rt::os::tmpdir() + "/codex-leader-");
    const fs::path leader = root / "leader";
    const fs::path follower = root / "follower";
    const std::string source = (leader / uos::storage::kv::BUN_KV_DATABASE_PATH).string();
    const std::string destination = (follower / uos::storage::kv::BUN_KV_DATABASE_PATH).string();
    const double now = rt::dateNow();
    const std::string initial = accessToken(now + 700'000);
    const std::string rotated = accessToken(now + 800'000);
    const std::string rejected = accessToken(now + 1'600'000);
    int socket = -1;
    pid_t writer = -1;

    LiveWalFixture() {
      fs::create_directories(leader / ".data");
      fs::create_directories(follower / ".data");
      rt::fs::writeFileSync((leader / "private").string(), std::string_view("synthetic independent file"));
      int pair[2] = {-1, -1};
      if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) throw rt::JsError("Leader fixture socketpair failed");
      std::fflush(nullptr);
      writer = ::fork();
      if (writer == 0) {
        ::close(pair[0]);
        try {
          runWriter(pair[1]);
          ::_exit(0);
        } catch (const std::exception& error) {
          std::fprintf(stderr, "Leader fixture: %s\n", error.what());
          ::_exit(1);
        }
      }
      ::close(pair[1]);
      socket = pair[0];
      if (writer < 0 || readCommand(socket, 5'000) != 'Y') throw rt::JsError("Leader fixture failed to start");
    }

    ~LiveWalFixture() {
      if (socket >= 0) {
        try { sendCommand(socket, 'Q'); } catch (...) {}
        ::close(socket);
      }
      if (writer > 0) ::waitpid(writer, nullptr, 0);
      fs::remove_all(root);
    }

    void command(char value) const {
      sendCommand(socket, value);
      if (readCommand(socket, 5'000) != 'Y') throw rt::JsError("Leader fixture did not complete a write or checkpoint");
    }

    void runWriter(int channel) const {
      std::optional<sqlite::Database> db(std::in_place, source);
      db->run("PRAGMA journal_mode = WAL; PRAGMA wal_autocheckpoint = 100; PRAGMA busy_timeout = 100");
      db->run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT); "
              "INSERT INTO uos_kv_metadata VALUES ('codec', 'bun-jsc-v1'); "
              "CREATE TABLE kv (k BLOB PRIMARY KEY, v BLOB, v_encoding INTEGER, version INTEGER, expiration_ms INTEGER); "
              "CREATE TABLE writes (n INTEGER); INSERT INTO writes VALUES (0)");
      const auto store = [&](const std::string& bearer) {
        const uos::types::CodexAuthState credential{bearer, "synthetic leader refresh", "follower-first", now};
        const uos::types::CodexAuthPoolState pool{{credential}, now};
        const auto row = rt::kvJscSerialize(rt::KvValue(json::value_from(pool)));
        db->run("INSERT INTO kv VALUES (?, ?, 1, 1, -1) ON CONFLICT(k) DO UPDATE SET v = excluded.v, version = version + 1",
                rt::packKey(auth::CODEX_AUTH_POOL_KV_KEY), row);
      };
      store(initial);
      sendCommand(channel, 'Y');
      bool paused = false;
      for (;;) {
        const auto next = readCommand(channel, 5);
        if (next == 'Q') return;
        if (next == '\0') {
          if (db && !paused) db->run("UPDATE writes SET n = n + 1");
          continue;
        }
        if (next == 'P') paused = !paused;
        if (next == 'R') store(rotated);
        if (next == 'B') store(rejected);
        if (next == 'C') {
          db->run("UPDATE writes SET n = n + 1");
          const auto checkpoint = db->query("PRAGMA wal_checkpoint(TRUNCATE)").safeIntegers(true).get();
          if (!checkpoint || std::get<std::int64_t>(*checkpoint->get("busy")) != 0)
            throw rt::JsError("Leader fixture checkpoint was blocked");
        }
        if (next == 'S') db.reset();
        if (next == 'L') {
          db.emplace(source, sqlite::DatabaseOptions{.readwrite = true});
          db->run("PRAGMA journal_mode = DELETE; BEGIN EXCLUSIVE");
        }
        if (next == 'D') fs::remove(source);
        if (next == 'G') rt::fs::writeFileSync(source, std::string_view("garbage fixture database"));
        sendCommand(channel, 'Y');
      }
    }

    auto listing() const {
      std::map<fs::path, std::pair<std::uintmax_t, fs::file_time_type>> result;
      for (const auto& entry : fs::recursive_directory_iterator(leader)) {
        const auto path = entry.path().string();
        if (path == source || path == source + "-wal" || path == source + "-shm") continue;
        // The writer creates and removes sidecars in this directory. Record
        // its presence and every unrelated entry, with file size and mtime.
        if (entry.path() == leader / ".data") result.emplace(path, std::pair{0U, fs::file_time_type{}});
        else result.emplace(path, std::pair{entry.file_size(), entry.last_write_time()});
      }
      return result;
    }
  };

  rt::Task<void> expectReadOutcome(Fixture& fixture, std::string expected) {
    auto requesting = fixture.request("/health");
    const auto response = co_await std::move(requesting);
    const auto payload = co_await response.json();
    const auto* health = rt::get(payload, "upstream_auth");
    ASSERT(health);
    EXPECT_EQ(rt::getBool(*health, "leader_state"), true);
    EXPECT_EQ(rt::getString(*health, "leader_last_read_outcome"), expected);
    EXPECT(rt::getNumber(*health, "earliest_access_token_expires_in_ms").value_or(0) > 0);
    EXPECT(rt::getNumber(*health, "leader_last_success_age_ms").value_or(-1) >= 0);
  }

  rt::Task<void> exerciseLiveFollower(const LiveWalFixture& files) {
    const rt::env::ScopedOverride follower{{std::string(uos::upstream_auth::FOLLOWER_ENV), "1"}};
    auto opening = uos::storage::kv::openKv(files.destination);
    auto store = co_await std::move(opening);
    {
      Fixture fixture(store);
      fixture.withSibling = false;
      int checks = 0;
      fixture.usage = [&](const rt::Request& request) {
        EXPECT_EQ(request.method(), "GET");
        EXPECT_EQ(request.headers().get("ChatGPT-Account-ID"), "follower-first");
        // This runs during validation, while the follower request is still
        // pending. Both a write and a forced checkpoint must complete.
        files.command('C');
        ++checks;
        return rt::Response::json(json::object{}, {.status = request.headers().get("Authorization") == "Bearer " + files.rejected ? 401.0 : 200.0});
      };
      rt::Reason failure;
      auto originalClock = rt::globalDateNow();
      const auto restoreClock = rt::scopeExit([&] { rt::globalDateNow() = std::move(originalClock); });
      try {
        co_await fixture.start();
        auth::setLeaderAuthReader(uos::codex::auth_import::liveReader(files.source, store));
        EXPECT(co_await auth::checkLeaderAuth());
        co_await expectReadOutcome(fixture, "ok");
        co_await fixture.inference(200);
        EXPECT_EQ(checks, 1);

        files.command('R');
        fixture.rejectedAccess = files.initial;
        co_await fixture.inference(503);
        EXPECT_EQ(checks, 1);
        EXPECT(fixture.recordedFollowerReason());
        co_await rt::sleep(LEADER_WAIT_MS);
        co_await fixture.inference(200);
        EXPECT_EQ(checks, 2);
        co_await expectReadOutcome(fixture, "ok");

        files.command('B');
        rt::globalDateNow() = [&] { return files.now + 730'000; };
        co_await rt::sleep(LEADER_WAIT_MS);
        co_await fixture.inference(200);
        co_await expectReadOutcome(fixture, "invalid");
        EXPECT_EQ(checks, 3);

        files.command('S');
        co_await fixture.inference(200);
        files.command('L');
        co_await rt::sleep(LEADER_WAIT_MS);
        co_await fixture.inference(200);
        co_await expectReadOutcome(fixture, "busy");
        files.command('S');
        files.command('D');
        co_await rt::sleep(LEADER_WAIT_MS);
        co_await fixture.inference(200);
        co_await expectReadOutcome(fixture, "unreadable");
        files.command('G');
        co_await rt::sleep(LEADER_WAIT_MS);
        co_await fixture.inference(200);
        co_await expectReadOutcome(fixture, "invalid");
        EXPECT_EQ(checks, 3);
        EXPECT_EQ(fixture.tokenCalls, 0);

        const auto logs = fixture.logs.calls();
        EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const auto& line) {
          return line.text.find("Adopted leader Codex credentials for 1 account(s).") != std::string::npos;
        }), 2);
        EXPECT(std::none_of(logs.begin(), logs.end(), [&](const auto& line) {
          return line.text.find(files.initial) != std::string::npos || line.text.find(files.rotated) != std::string::npos ||
                 line.text.find(files.rejected) != std::string::npos || line.text.find("synthetic leader refresh") != std::string::npos;
        }));
        rt::globalDateNow() = [&] { return files.now + 900'000; };
        co_await fixture.inference(503);
        EXPECT_EQ(fixture.tokenCalls, 0);
      } catch (...) { failure = std::current_exception(); }
      co_await fixture.stop();
      if (failure) std::rethrow_exception(failure);
    }
    auto reopening = uos::storage::kv::openKv(files.destination);
    const auto persisted = co_await std::move(reopening);
    const auto entry = co_await persisted->get(auth::CODEX_AUTH_POOL_KV_KEY);
    const auto pool = auth::parseCodexAuthPool(entry.value.toJson());
    ASSERT(pool && pool->accounts.size() == 1);
    EXPECT(pool->accounts.front().access_token == files.rotated);
    persisted->close();
  }

  void expectOpenDenied(const std::string& path, int flags) {
    const int fd = ::open(path.c_str(), flags | O_CLOEXEC, 0600);
    const int error = errno;
    if (fd >= 0) ::close(fd);
    EXPECT(fd < 0 && error == EACCES);
  }
}

TEST("a confined follower adopts live WAL credentials and retains the last good one") {
  uos::tests::helpers::responses_conformance_client::stageGatewayAssets();
  const LiveWalFixture files;
  const auto before = files.listing();
  const int beforeFailures = rt::test::failures();
  std::fflush(nullptr);
  const pid_t child = ::fork();
  ASSERT(child >= 0);
  if (child == 0) {
    try {
      const auto package = fs::canonical(fs::path(__FILE__).parent_path() / "../../..");
      auto profile = permissions::gatewayLinuxPermissions(package.string(), files.follower.string()).gateway;
      profile.readFilesBelow.push_back((files.leader / ".data").string());
      const auto receipt = permissions::applyLinuxPermissions(profile);
      ASSERT(receipt.filesystemThreadSync && receipt.processThreadSync);
      expectOpenDenied((files.leader / "private").string(), O_RDONLY);
      for (const auto* suffix : {"", "-wal", "-shm"}) expectOpenDenied(files.source + suffix, O_RDWR);
      expectOpenDenied(files.source + "-journal", O_WRONLY | O_CREAT);
      EXPECT(::unlink(files.source.c_str()) == -1 && errno == EACCES);
      rt::Loop loop;
      rt::blockOn(loop, exerciseLiveFollower(files));
      std::fflush(nullptr);
      ::_exit(rt::test::failures() == beforeFailures ? 0 : 1);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "Confined follower fixture: %s\n", error.what());
    }
    ::_exit(1);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(files.listing(), before);
}

TEST("the live SQLite reader works in an unwritable directory and never creates a missing WAL index") {
  const LiveWalFixture files;
  files.command('P');
  const std::string shm = files.source + "-shm";
  const std::string data = (files.leader / ".data").string();
  ASSERT_EQ(::chmod(data.c_str(), 0500), 0);
  const auto restoreDirectory = rt::scopeExit([&] { ::chmod(data.c_str(), 0700); });
  for (const auto* suffix : {"", "-wal", "-shm"}) {
    const auto info = rt::fs::lstatSync(files.source + suffix);
    {
      const auto db = sqlite::Database::openLiveReadOnly(files.source);
      EXPECT(db.query("SELECT version FROM kv").get());
    }
    const auto after = rt::fs::lstatSync(files.source + suffix);
    EXPECT_EQ(after.ino, info.ino);
    EXPECT_EQ(after.size, info.size);
    EXPECT_EQ(after.mtimeNs, info.mtimeNs);
    EXPECT_EQ(after.ctimeNs, info.ctimeNs);
  }
  ASSERT_EQ(::chmod(data.c_str(), 0700), 0);
  fs::rename(shm, shm + ".saved");
  const auto restoreShm = rt::scopeExit([&] { fs::rename(shm + ".saved", shm); });
  EXPECT_THROWS(sqlite::Database::openLiveReadOnly(files.source).query("SELECT version FROM kv").get(), sqlite::SQLiteError);
  EXPECT(!fs::exists(shm));
}
#endif
