// Port of tests/local-admin-auth.test.ts, including auth/index and fresh-process timeline assertions.
// Links: tests/local_admin_auth_test.cpp
// src/kv.cpp src/request.cpp src/utils.cpp src/storage/kv.cpp
// src/storage/sqlite_readonly.cpp src/http.cpp src/auth/index.cpp src/auth/passport.cpp
// src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/auth/passport_sessions.cpp
// src/kernel/attestation.cpp src/auth/local_development_key.cpp src/auth/passkeys.cpp src/api_keys.cpp
// src/types.cpp src/paid_fallback/ledger_state.cpp src/kernel/policy_queue.cpp src/api_key_policy.cpp
// src/auth/local_admin.cpp src/kernel/quota_reservations.cpp src/authority/native_paid_contracts.cpp src/config.cpp
// src/authority/native_paid_policy.cpp src/kernel/quota_v2.cpp src/authority/native_kernel.cpp src/provider/surplus.cpp
// src/authority/native_paid.cpp src/analytics.cpp src/responses_stream.cpp src/storage/native_kv.cpp
// src/provider/anthropic_chat_responses.cpp src/provider/surplus_claude_chat_request.cpp src/provider/anthropic_response_wire.cpp src/provider/surplus_claude_request.cpp
// src/authority/native_kernel_contracts.cpp src/provider/anthropic_replay.cpp src/release.cpp src/paid_fallback/index.cpp
// src/paid_fallback/ledger_backfill.cpp src/provider/cerebras_rate_limits.cpp src/paid_fallback/ledger_admission.cpp src/auth/relay.cpp
// src/paid_fallback/ledger_settlement.cpp src/provider/metered.cpp src/paid_fallback/ledger_stats.cpp src/provider/anthropic_responses.cpp
// src/provider/anthropic_chat_stream.cpp src/models/codex_models_fetch.cpp src/codex/auth.cpp src/bounded_response_body.cpp
// src/codex/auth_refresh.cpp src/codex/dispatch.cpp src/models/codex_models.cpp src/models/codex_models_availability.cpp
// src/runtime_config.cpp src/authority/native_codex_contracts.cpp src/codex/native_auth.cpp src/provider/health.cpp
// src/defaults.cpp src/codex/external_auth.cpp src/provider/capacity_events.cpp src/provider/claude_code_accounts.cpp
// src/codex/routing_mutations.cpp src/authority/native_codex_capacity.cpp src/provider/capacity_contract.cpp src/codex/capacity_routing.cpp
// src/provider/claude_code_credentials.cpp src/authority/native_codex_mutations.cpp src/codex/rate_limit_429.cpp src/authority/native_codex_429.cpp
// src/authority/native_codex_evaluation.cpp src/codex/routing_state.cpp src/codex/banked_reset.cpp src/codex/native_auth_transport.cpp
// src/provider/capacity.cpp src/provider/claude_capacity_rollups.cpp src/cache/prompt_analytics_core.cpp src/provider/capacity_history.cpp
// src/provider/capacity_parse.cpp src/metered_quota.cpp src/cache/prompt_analytics_read.cpp src/provider/claude_capacity_history.cpp
// src/provider/capacity_rollups.cpp src/provider/claude_code_quota.cpp src/codex/banked_reset_provider.cpp src/runtime/websocket.cpp
// src/codex/reset_settings.cpp src/codex/experiment.cpp src/provider/claude_code_usage_schema.cpp src/provider/capacity_analytics.cpp
// src/codex/account_routing.cpp src/codex/routing_serial.cpp src/provider/selection.cpp src/authority/native_codex.cpp
// src/codex/routing_evaluation.cpp src/codex/overage_settings.cpp
#include "uos/auth/local_admin.hpp"
#include "uos/auth/index.hpp"
#include "uos/kv.hpp"

#include "uos/config.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::auth::local_admin;
  using uos::runtime::http::Addr;
  using uos::runtime::http::AddrUnix;
  using uos::runtime::http::NetAddr;

  namespace auth = uos::auth::index;
  struct AuthIsolation {
    rt::env::ScopedEnvironment environment{std::initializer_list<std::pair<std::string, std::string>>()};
    uos::config::Config saved = uos::config::config;
    AuthIsolation() { uos::config::config.isDeploy = false; uos::config::config.isProductionTimeline = false; uos::config::config.authTokens.clear(); uos::config::config.adminTokens.clear(); uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(nullptr); }
    ~AuthIsolation() { uos::config::config = saved; uos::kv::setKvForTest(nullptr); }
  };
  json::value clientOutcome(const auth::AuthenticateClientResult& value) { if (const auto* ok = std::get_if<auth::AuthenticateClientResultOk>(&value)) return json::value(auth::clientAuthMethodKind(ok->method)); return json::value(std::get<auth::AuthenticateClientResultNotOk>(value).response.status()); }
  json::value adminOutcome(const auth::AdminAuthResult& value) { if (const auto* ok = std::get_if<auth::AdminAuthResultOk>(&value)) return json::value(auth::adminAuthMethodKind(ok->method)); return json::value(std::get<auth::AdminAuthResultNotOk>(value).response.status()); }
  NetAddr tcpAddress(std::string hostname) { return NetAddr{"tcp", std::move(hostname), 8000}; }

  const ServeRuntimeOptions enabledOptions{true};
  const ServeRuntimeOptions disabledOptions{false};

  // assert.throws(fn, /pattern/): an Error whose message matches.
  bool throwsMatching(const std::function<void()>& run, const std::string& pattern) {
    try {
      run();
    } catch (const rt::JsError& error) {
      return rt::Regex(pattern, "").test(error.what());
    }
    return false;
  }

  rt::Request withHeaders(const rt::Request& input, rt::Headers headers) {
    rt::RequestInit init;
    init.headers = std::move(headers);
    return rt::Request(input, std::optional<rt::RequestInit>(std::move(init)));
  }

  // RAII form of the tests' `try { ... } finally { configureAdminAuthForListener(disabledOptions, ...) }`.
  struct ResetListener {
    ~ResetListener() { configureAdminAuthForListener(disabledOptions, tcpAddress("127.0.0.1")); }
  };

  // withProductionTimeline: `config.isProductionTimeline` changed for the scope.
  struct ProductionTimeline {
    bool previous;
    explicit ProductionTimeline(bool enabled) : previous(uos::config::config.isProductionTimeline) { uos::config::config.isProductionTimeline = enabled; }
    ~ProductionTimeline() { uos::config::config.isProductionTimeline = previous; }
  };

  const NetAddr macListenerAddress{"tcp", "0.0.0.0", 7999};

  constexpr std::string_view CHILD_MARKER = "UOS_LOCAL_ADMIN_TIMELINE_CHILD";
  constexpr const char* CHILD_TEST = "local admin timeline child";

  // How this binary was started (the SDK loader, its arguments and the binary).
  std::vector<std::string> selfCommand() {
#if defined(__APPLE__)
    return {uos::rt::process::execPath()};
#else
    const std::vector<std::uint8_t> bytes = rt::fs::readFileSync("/proc/self/cmdline");
    std::vector<std::string> argv;
    std::string current;
    for (const std::uint8_t byte : bytes) {
      if (byte == 0) {
        argv.push_back(current);
        current.clear();
      } else {
        current.push_back(static_cast<char>(byte));
      }
    }
    const bool viaLoader = !argv.empty() && rt::includes(argv[0], "ld-linux");
    const std::size_t keep = viaLoader ? 4 : 1;
    argv.resize(std::min(argv.size(), keep));
    return argv;
#endif
  }

  /** Runs the child case after loading config in a fresh process whose only timeline input is `DENO_TIMELINE`. */
  rt::Task<json::value> runTimelineProbe(std::optional<std::string> timeline) {
    std::vector<std::string> command = selfCommand();
    command.emplace_back(CHILD_TEST);
    std::vector<std::pair<std::string, std::string>> environment{{"PATH", rt::env::get("PATH").value_or("/usr/bin:/bin")}, {std::string(CHILD_MARKER), "1"}};
    if (timeline) environment.emplace_back("DENO_TIMELINE", *timeline);
    rt::process::SpawnOptions options;
    options.env = environment;
    options.stdout = rt::process::Stdio::pipe;
    options.stderr = rt::process::Stdio::inherit;
    const rt::process::Subprocess child = rt::process::spawn(command, options);
    rt::Response response{rt::BodyInit(child.stdout())};
    auto reading = response.text();
    const std::string text = co_await std::move(reading);
    auto exited = child.exited();
    const int code = co_await exited.wait();
    EXPECT_EQ(code, 0);
    json::value printed;
    for (const std::string& line : rt::split(text, "\n"))
      if (rt::startsWith(line, "PROBE ")) printed = rt::jsonParse(rt::slice(line, 6));
    co_return printed;
  }
}

ASYNC_TEST("local admin timeline child") {
  if (!rt::env::has(CHILD_MARKER)) co_return;
  uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(nullptr);
  std::string flag = "accepted";
  try { parseServeRuntimeOptions({"--disable-admin-auth"}, {uos::config::config.isDeploy, uos::config::config.isProductionTimeline}); }
  catch (const rt::JsError& error) { flag = error.what(); }
  json::object out; out["flag"] = flag;
  AWAIT(boot, auth::authenticateClient(rt::Request("http://127.0.0.1:7999/v1/responses", rt::RequestInit{.method = "POST", .body = rt::BodyInit(std::string("{}"))})));
  const auto* bootOk = std::get_if<auth::AuthenticateClientResultOk>(&boot);
  out["client"] = json::object{{"isDeploy", uos::config::config.isDeploy}, {"isProductionTimeline", uos::config::config.isProductionTimeline}, {"ok", bootOk != nullptr}, {"method", bootOk ? json::value(auth::clientAuthMethodKind(bootOk->method)) : json::value(nullptr)}, {"status", bootOk ? json::value(nullptr) : json::value(std::get<auth::AuthenticateClientResultNotOk>(boot).response.status())}};
  for (int index = 0; index < 2; ++index) {
    if (index == 0) configureAdminAuthForListener(enabledOptions, tcpAddress("127.0.0.1")); else configureMacLocalAdminAuthBypassForListener(macListenerAddress);
    const rt::Request client("http://127.0.0.1:7999/v1/models"), admin("http://127.0.0.1:7999/admin/api-keys"); configureAdminAuthPeerForRequest(tcpAddress("127.0.0.1"), client); configureAdminAuthPeerForRequest(tcpAddress("127.0.0.1"), admin);
    AWAIT(clientAuth, auth::authenticateClient(client)); AWAIT(adminAuth, auth::authenticateAdmin(admin)); out[index == 0 ? "listener" : "mac"] = json::object{{"client", clientOutcome(clientAuth)}, {"admin", adminOutcome(adminAuth)}};
  }
  configureAdminAuthForListener(disabledOptions, tcpAddress("127.0.0.1"));
  std::printf("PROBE %s\n", rt::jsonStringify(json::value(out)).c_str());
}

TEST("serve runtime options keep admin auth enabled by default") {
  EXPECT(parseServeRuntimeOptions({}, {false, false}) == ServeRuntimeOptions{false});
}

TEST("serve runtime options accept only one exact disable-admin-auth flag") {
  EXPECT(parseServeRuntimeOptions({"--disable-admin-auth"}, {false, false}) == ServeRuntimeOptions{true});
  EXPECT(throwsMatching([] { parseServeRuntimeOptions({"--disable-admin-auth", "--disable-admin-auth"}, {false, false}); }, "may only be specified once"));
  EXPECT(throwsMatching([] { parseServeRuntimeOptions({"--disable-admin-auth=true"}, {false, false}); }, "Unknown server argument"));
  EXPECT(throwsMatching([] { parseServeRuntimeOptions({"--disable-auth"}, {false, false}); }, "Unknown server argument"));
}

TEST("disable-admin-auth is rejected in Deno Deploy") {
  EXPECT(throwsMatching([] { parseServeRuntimeOptions({"--disable-admin-auth"}, {true, false}); }, "unavailable in Deno Deploy"));
}

TEST("disable-admin-auth is rejected on the production timeline") {
  EXPECT(throwsMatching([] { parseServeRuntimeOptions({"--disable-admin-auth"}, {false, true}); }, "unavailable on the production timeline"));
  EXPECT(parseServeRuntimeOptions({}, {false, true}) == ServeRuntimeOptions{false});
}

TEST("loopback detection accepts local TCP hostnames only") {
  for (const char* hostname : {"localhost", "LOCALHOST", "127.0.0.1", "127.42.9.3", "::1", "[::1]"}) {
    if (!isLoopbackHostname(hostname)) std::fprintf(stderr, "      hostname: %s\n", hostname);
    EXPECT_EQ(isLoopbackHostname(hostname), true);
  }
  // RFC 5737 documentation addresses stand in for non-loopback peers.
  for (const char* hostname : {"0.0.0.0", "::", "192.0.2.10", "198.51.100.1", "ai.ubq.fi", "127.0.0.1.example"}) {
    if (isLoopbackHostname(hostname)) std::fprintf(stderr, "      hostname: %s\n", hostname);
    EXPECT_EQ(isLoopbackHostname(hostname), false);
  }
}

TEST("admin auth can only be disabled on a loopback TCP listener") {
  EXPECT_EQ(shouldDisableAdminAuthForListener(disabledOptions, tcpAddress("0.0.0.0")), false);
  EXPECT_EQ(shouldDisableAdminAuthForListener(enabledOptions, tcpAddress("127.0.0.1")), true);
  EXPECT_EQ(shouldDisableAdminAuthForListener(enabledOptions, tcpAddress("::1")), true);
  EXPECT(throwsMatching([] { shouldDisableAdminAuthForListener(enabledOptions, tcpAddress("localhost")); }, "requires a loopback TCP listener"));
  EXPECT(throwsMatching([] { shouldDisableAdminAuthForListener(enabledOptions, tcpAddress("0.0.0.0")); }, "requires a loopback TCP listener"));
  // A socket path fixture, not a temporary file: nothing is created at it.
  EXPECT(throwsMatching([] { shouldDisableAdminAuthForListener(enabledOptions, AddrUnix{"/run/ai-ubq-fi.sock"}); }, "requires a loopback TCP listener"));
}

ASYNC_TEST("guarded runtime bypass grants local super-admin access only to loopback peers") {
  AuthIsolation isolation;
  const rt::Request localRequest("http://127.0.0.1/admin/api-keys"), remoteRequest("https://ai.ubq.fi/admin/api-keys");
  configureAdminAuthForListener(enabledOptions, tcpAddress("127.0.0.1"));
  EXPECT(!isAdminAuthDisabledForRequest(localRequest)); AWAIT(noPeer, auth::authenticateAdmin(localRequest)); EXPECT(std::holds_alternative<auth::AdminAuthResultNotOk>(noPeer));
  configureAdminAuthPeerForRequest(tcpAddress("192.0.2.10")); EXPECT(!isAdminAuthDisabledForRequest(localRequest)); AWAIT(forwarded, auth::authenticateAdmin(localRequest)); EXPECT(std::holds_alternative<auth::AdminAuthResultNotOk>(forwarded));
  configureAdminAuthPeerForRequest(AddrUnix{"/run/ai-ubq-fi.sock"}); EXPECT(!isAdminAuthDisabledForRequest(localRequest));
  configureAdminAuthPeerForRequest(tcpAddress("127.0.0.1"));
  {
    ResetListener reset; EXPECT(isAdminAuthDisabledForRequest(localRequest)); EXPECT(!isAdminAuthDisabledForRequest(remoteRequest));
    const std::vector<rt::Headers> crossOriginHeaders{rt::Headers{{"origin", "https://attacker.example"}}, rt::Headers{{"origin", "null"}}, rt::Headers{{"sec-fetch-site", "cross-site"}}, rt::Headers{{"sec-fetch-site", "same-site"}}};
    for (const auto& headers : crossOriginHeaders) { const auto request = withHeaders(localRequest, headers); EXPECT(!isAdminAuthDisabledForRequest(request)); AWAIT(refused, auth::authenticateAdmin(request)); EXPECT(std::holds_alternative<auth::AdminAuthResultNotOk>(refused)); }
    EXPECT(isAdminAuthDisabledForRequest(withHeaders(localRequest, rt::Headers{{"origin", "http://127.0.0.1"}, {"sec-fetch-site", "same-origin"}})));
    AWAIT(localAuth, auth::authenticateAdmin(localRequest)); const auto* accepted = std::get_if<auth::AdminAuthResultOk>(&localAuth); ASSERT(accepted); EXPECT_EQ(auth::adminAuthMethodKind(accepted->method), "disabled"); EXPECT(accepted->is_super_admin);
    AWAIT(loopback, auth::authenticateClient(rt::Request("http://127.42.9.3/v1/models"))); EXPECT_EQ(clientOutcome(loopback), json::value("disabled")); AWAIT(remote, auth::authenticateAdmin(remoteRequest)); EXPECT_EQ(adminOutcome(remote), json::value(401));
    AWAIT(whoami, auth::handleV1Auth(rt::Request("http://127.0.0.1/uos/auth"))); EXPECT_EQ(whoami.status(), 200); AWAIT(body, whoami.json()); const auto* authBody = rt::get(body, "auth"); ASSERT(authBody); EXPECT_EQ(rt::getBool(*authBody, "is_admin"), std::optional(true)); EXPECT_EQ(rt::getBool(*authBody, "is_super_admin"), std::optional(true)); EXPECT_EQ(rt::getString(*rt::get(*authBody, "method"), "kind"), std::optional<std::string_view>("disabled"));
  }
  EXPECT(!isAdminAuthDisabledForRequest(localRequest)); AWAIT(defaultAuth, auth::authenticateAdmin(localRequest)); EXPECT_EQ(adminOutcome(defaultAuth), json::value(401)); AWAIT(defaultWhoami, auth::handleV1Auth(rt::Request("http://127.0.0.1/uos/auth"))); EXPECT_EQ(defaultWhoami.status(), 200); AWAIT(body, defaultWhoami.json()); const auto* authBody = rt::get(body, "auth"); ASSERT(authBody); EXPECT_EQ(rt::getBool(*authBody, "is_admin"), std::optional(false)); EXPECT_EQ(rt::getBool(*authBody, "is_super_admin"), std::optional(false)); AWAIT(other, auth::authenticateClient(rt::Request("http://127.42.9.3/v1/models"))); EXPECT_EQ(clientOutcome(other), json::value(401));
}

TEST("the Mac LAN listener configurator accepts the wildcard TCP listener only") {
  EXPECT_EQ(configureMacLocalAdminAuthBypassForListener(macListenerAddress), true);
  {
    ResetListener reset;
    EXPECT(throwsMatching([] { configureMacLocalAdminAuthBypassForListener(tcpAddress("127.0.0.1")); }, "requires the 0\\.0\\.0\\.0 TCP listener"));
    // A socket path fixture, not a temporary file: nothing is created at it.
    EXPECT(throwsMatching([] { configureMacLocalAdminAuthBypassForListener(AddrUnix{"/run/ai-ubq-fi.sock"}); }, "requires the 0\\.0\\.0\\.0 TCP listener"));
    // The Mac entry point never relaxes the generic guard: the same
    // non-loopback listener still fails for `--disable-admin-auth` callers.
    EXPECT(throwsMatching([] { shouldDisableAdminAuthForListener(enabledOptions, macListenerAddress); }, "requires a loopback TCP listener"));
  }
  EXPECT_EQ(isAdminAuthDisabledForRequest(rt::Request("http://127.0.0.1/v1/models")), false);
}

ASYNC_TEST("the Mac LAN listener bypasses authentication for loopback peers only") {
  AuthIsolation isolation;
  const rt::Request localRequest("http://127.0.0.1/v1/models");
  const rt::Request forgedLoopbackHost("http://127.0.0.1/v1/models");
  const rt::Request unboundRequest("http://127.0.0.1/v1/models");

  EXPECT_EQ(configureMacLocalAdminAuthBypassForListener(macListenerAddress), true);
  ResetListener reset;
  // Fail closed until a loopback peer is observed for that exact request.
  EXPECT_EQ(isAdminAuthDisabledForRequest(localRequest), false);

  AWAIT(noPeer, auth::authenticateAdmin(localRequest)); EXPECT_EQ(adminOutcome(noPeer), json::value(401));
  configureAdminAuthPeerForRequest(tcpAddress("127.0.0.1"), localRequest);
  // A LAN peer that forges a loopback Host header keeps its own peer.
  configureAdminAuthPeerForRequest(tcpAddress("192.0.2.10"), forgedLoopbackHost);
  EXPECT_EQ(isAdminAuthDisabledForRequest(localRequest), true);
  EXPECT_EQ(isAdminAuthDisabledForRequest(forgedLoopbackHost), false);
  EXPECT_EQ(isAdminAuthDisabledForRequest(unboundRequest), false);
  AWAIT(localAuth, auth::authenticateAdmin(localRequest)); const auto* accepted = std::get_if<auth::AdminAuthResultOk>(&localAuth); ASSERT(accepted); EXPECT(accepted->is_super_admin); EXPECT_EQ(auth::adminAuthMethodKind(accepted->method), "disabled"); AWAIT(forged, auth::authenticateAdmin(forgedLoopbackHost)); EXPECT_EQ(adminOutcome(forged), json::value(401)); AWAIT(unbound, auth::authenticateAdmin(unboundRequest)); EXPECT_EQ(adminOutcome(unbound), json::value(401));
}

ASYNC_TEST("a concurrent LAN request cannot inherit a loopback peer") {
  AuthIsolation isolation;
  // Routing awaits several times before it authenticates, so the two requests
  // below really do overlap in production. A process-wide peer slot would let
  // the LAN request read the local request's loopback peer; each request keeps
  // its own peer instead. 127.42.9.3 is loopback but outside the legacy
  // dev-host list, so only the peer can decide here.
  const rt::Request localRequest("http://127.42.9.3/v1/models");
  const rt::Request lanRequest("http://127.42.9.3/v1/models");

  EXPECT_EQ(configureMacLocalAdminAuthBypassForListener(macListenerAddress), true);
  ResetListener reset;
  configureAdminAuthPeerForRequest(tcpAddress("127.0.0.1"), localRequest);
  // The LAN request arrives (and is bound to its own peer) before the local
  // request reaches its authentication check.
  configureAdminAuthPeerForRequest(tcpAddress("192.0.2.10"), lanRequest);
  co_await rt::microtask();

  EXPECT_EQ(isAdminAuthDisabledForRequest(lanRequest), false);
  EXPECT_EQ(isAdminAuthDisabledForRequest(localRequest), true);
  AWAIT(lanAuth, auth::authenticateClient(lanRequest)); EXPECT_EQ(clientOutcome(lanAuth), json::value(401)); AWAIT(localAuth, auth::authenticateClient(localRequest)); EXPECT_EQ(clientOutcome(localAuth), json::value("disabled"));
}

ASYNC_TEST("the production timeline refuses a configured listener or Mac loopback bypass") {
  AuthIsolation isolation;
  // Each enabler is reached in code, as a launcher would reach it; only the timeline differs.
  const std::vector<std::pair<std::string, std::function<void()>>> enablers{
    {"listener", [] { configureAdminAuthForListener(enabledOptions, tcpAddress("127.0.0.1")); }},
    {"mac", [] { configureMacLocalAdminAuthBypassForListener(macListenerAddress); }},
  };
  const auto loopbackRequest = [](std::string path) {
    const rt::Request request("http://127.0.0.1:7999" + path);
    configureAdminAuthPeerForRequest(tcpAddress("127.0.0.1"), request);
    return request;
  };
  for (const auto& [name, enable] : enablers) {
    enable();
    ResetListener reset;
    {
      ProductionTimeline timeline(false);
      if (!isAdminAuthDisabledForRequest(loopbackRequest("/v1/models"))) std::fprintf(stderr, "      enabler: %s\n", name.c_str());
      EXPECT_EQ(isAdminAuthDisabledForRequest(loopbackRequest("/v1/models")), true);
      AWAIT(admin, auth::authenticateAdmin(loopbackRequest("/admin/api-keys"))); EXPECT_EQ(adminOutcome(admin), json::value("disabled"));
    }
    {
      ProductionTimeline timeline(true);
      if (isAdminAuthDisabledForRequest(loopbackRequest("/v1/models"))) std::fprintf(stderr, "      enabler: %s\n", name.c_str());
      EXPECT_EQ(isAdminAuthDisabledForRequest(loopbackRequest("/v1/models")), false);
      AWAIT(client, auth::authenticateClient(loopbackRequest("/v1/models"))); EXPECT_EQ(clientOutcome(client), json::value(401)); AWAIT(admin, auth::authenticateAdmin(loopbackRequest("/admin/api-keys"))); EXPECT_EQ(adminOutcome(admin), json::value(401)); AWAIT(whoami, auth::handleV1Auth(loopbackRequest("/uos/auth"))); EXPECT_EQ(whoami.status(), 401);
    }
  }
}

ASYNC_TEST("DENO_TIMELINE=production refuses the disable-admin-auth flag and every enabled loopback bypass") {
  const std::optional<std::string> production("production"); AWAIT(refused, runTimelineProbe(production)); EXPECT_EQ(rt::getString(refused, "flag"), std::optional<std::string_view>("[ai.ubq.fi] --disable-admin-auth is unavailable on the production timeline (DENO_TIMELINE=production)."));
  const json::object denied{{"client", 401}, {"admin", 401}}; EXPECT(rt::deepEqual(*rt::get(refused, "listener"), denied)); EXPECT(rt::deepEqual(*rt::get(refused, "mac"), denied));
  const std::optional<std::string> none; AWAIT(accepted, runTimelineProbe(none)); EXPECT_EQ(rt::getString(accepted, "flag"), std::optional<std::string_view>("accepted")); const json::object allowed{{"client", "disabled"}, {"admin", "disabled"}}; EXPECT(rt::deepEqual(*rt::get(accepted, "listener"), allowed)); EXPECT(rt::deepEqual(*rt::get(accepted, "mac"), allowed));
}
ASYNC_TEST("DENO_TIMELINE=production disables the loopback hostname client-auth bypass at startup") {
  const std::optional<std::string> production("production"); AWAIT(refused, runTimelineProbe(production)); const json::object denied{{"isDeploy", false}, {"isProductionTimeline", true}, {"ok", false}, {"method", nullptr}, {"status", 401}}; EXPECT(rt::deepEqual(*rt::get(refused, "client"), denied));
  const std::optional<std::string> none; AWAIT(accepted, runTimelineProbe(none)); const json::object allowed{{"isDeploy", false}, {"isProductionTimeline", false}, {"ok", true}, {"method", "disabled"}, {"status", nullptr}}; EXPECT(rt::deepEqual(*rt::get(accepted, "client"), allowed));
}
ASYNC_TEST("a development checkout keeps the loopback hostname client-auth bypass") {
  AuthIsolation isolation; const std::vector<std::string> origins{"http://127.0.0.1:7999", "http://localhost:7999", "http://[::1]:7999"};
  for (const auto& origin : origins) { const auto req = rt::Request(origin + "/v1/responses", rt::RequestInit{.method = "POST", .body = rt::BodyInit(std::string("{}"))}); AWAIT(result, auth::authenticateClient(req)); EXPECT_EQ(clientOutcome(result), json::value("disabled")); }
  AWAIT(whoami, auth::handleV1Auth(rt::Request("http://127.0.0.1:7999/uos/auth"))); EXPECT_EQ(whoami.status(), 200); AWAIT(body, whoami.json()); EXPECT_EQ(rt::getString(*rt::get(body, "auth"), "mode"), std::optional<std::string_view>("disabled"));
}
ASYNC_TEST("the production VPS timeline refuses unauthenticated loopback client requests") {
  AuthIsolation isolation; ProductionTimeline timeline(true); const std::vector<std::string> origins{"http://127.0.0.1:7999", "http://localhost:7999", "http://[::1]:7999"};
  for (const auto& origin : origins) { const auto req = rt::Request(origin + "/v1/responses", rt::RequestInit{.method = "POST", .body = rt::BodyInit(std::string("{}"))}); AWAIT(result, auth::authenticateClient(req)); EXPECT_EQ(clientOutcome(result), json::value(401)); }
  AWAIT(whoami, auth::handleV1Auth(rt::Request("http://127.0.0.1:7999/uos/auth"))); EXPECT_EQ(whoami.status(), 401);
}

// Differential cases (no TS test covers them): the exact error texts, the Mac companion
// signal and unbinding one request's peer, as the TS source produces them.
TEST("differential: error texts, the Mac companion signal and a null request peer") {
  EXPECT_THROWS_MESSAGE(parseServeRuntimeOptions({"--disable-auth"}, {false, false}), rt::JsError,
                        "[ai.ubq.fi] Unknown server argument '--disable-auth'. Supported arguments: --disable-admin-auth");
  EXPECT_THROWS_MESSAGE(parseServeRuntimeOptions({"--disable-admin-auth", "--disable-admin-auth"}, {false, false}), rt::JsError,
                        "[ai.ubq.fi] Server argument '--disable-admin-auth' may only be specified once.");
  EXPECT_THROWS_MESSAGE(parseServeRuntimeOptions({"--disable-admin-auth"}, {true, true}), rt::JsError, "[ai.ubq.fi] --disable-admin-auth is unavailable in Deno Deploy.");
  EXPECT_THROWS_MESSAGE(shouldDisableAdminAuthForListener(enabledOptions, AddrUnix{"/run/ai-ubq-fi.sock"}), rt::JsError,
                        "[ai.ubq.fi] --disable-admin-auth requires a loopback TCP listener; got unix:///run/ai-ubq-fi.sock.");
  EXPECT_THROWS_MESSAGE(shouldDisableAdminAuthForListener(enabledOptions, tcpAddress("::")), rt::JsError,
                        "[ai.ubq.fi] --disable-admin-auth requires a loopback TCP listener; got tcp://[::]:8000.");
  EXPECT_THROWS_MESSAGE(shouldDisableAdminAuthForListener(enabledOptions, NetAddr{"udp", "127.0.0.1", 53}), rt::JsError,
                        "[ai.ubq.fi] --disable-admin-auth requires a loopback TCP listener; got udp://127.0.0.1:53.");
  EXPECT_THROWS_MESSAGE(configureMacLocalAdminAuthBypassForListener(tcpAddress("127.0.0.1")), rt::JsError,
                        "[ai.ubq.fi] the Mac local admin bypass requires the 0.0.0.0 TCP listener; got tcp://127.0.0.1:8000.");
  for (const char* hostname : {" [::1] ", "0:0:0:0:0:0:0:1", "127.0.0.01", "127.0.0", "127.0.0.1.", "-0.0.0.1", "127.256.0.1"}) {
    const bool expected = std::string_view(hostname) == " [::1] " || std::string_view(hostname) == "0:0:0:0:0:0:0:1";
    if (isLoopbackHostname(hostname) != expected) std::fprintf(stderr, "      hostname: %s\n", hostname);
    EXPECT_EQ(isLoopbackHostname(hostname), expected);
  }

  EXPECT_EQ(configureMacLocalAdminAuthBypassForListener(macListenerAddress), true);
  EXPECT_EQ(isMacCompanionServiceListener(), true);
  {
    ResetListener reset;
    const rt::Request localRequest("http://127.0.0.1/v1/models");
    configureAdminAuthPeerForRequest(tcpAddress("127.0.0.1"), localRequest);
    EXPECT_EQ(isAdminAuthDisabledForRequest(localRequest), true);
    configureAdminAuthPeerForRequest(std::nullopt, localRequest);
    EXPECT_EQ(isAdminAuthDisabledForRequest(localRequest), false);
  }
  EXPECT_EQ(isMacCompanionServiceListener(), false);
}
