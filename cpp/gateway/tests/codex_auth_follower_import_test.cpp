// Links: src/codex/{auth_import,auth_repair,auth,native_auth,native_auth_transport,external_auth,capacity_routing,rate_limit_429,routing_mutations,routing_state}.cpp
//   src/authority/{native_codex_429,native_codex_capacity,native_codex_contracts,native_codex_evaluation,native_codex_mutations}.cpp
//   src/{bounded_response_body,config,kv,release,types,utils}.cpp src/provider/{capacity_parse,claude_code_accounts,claude_code_credentials,health}.cpp
//   src/runtime/{http_body,http_early_sse,http_tasks,http_upgrade,http,websocket}.cpp
//   src/storage/{key_compatibility,kv,native_kv,sqlite_readonly,value_compatibility}.cpp
#include "uos/codex/auth_import.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/storage/kv.hpp"
#include "uos/utils.hpp"

namespace rt = uos::rt;
namespace json = uos::json;
namespace auth = uos::codex::auth;
namespace sqlite = rt::sqlite;
using uos::types::CodexAuthPoolState;
using uos::types::CodexAuthState;

namespace {
  const rt::KvKey UNRELATED{"fixture", "unrelated"};
  constexpr std::string_view USAGE_URL = "http://127.0.0.1:1/backend-api/wham/usage";

  CodexAuthState credential(std::string account, std::string generation, double now, double lifetime) {
    const auto claims = rt::jsonStringify(json::object{{"exp", (now + lifetime) / 1000}});
    return {"e30." + uos::utils::base64UrlEncode(rt::encodeUtf8(claims)) + "." + generation,
      "synthetic-refresh-" + generation, std::move(account), now};
  }

  rt::Task<void> seed(std::string path, CodexAuthPoolState pool, std::string unrelated) {
    auto opening = uos::storage::kv::openKv(path);
    const auto store = co_await std::move(opening);
    const rt::KvValue value(std::move(unrelated));
    co_await store->set(UNRELATED, value);
    auto writing = store->set(auth::CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(pool)));
    co_await std::move(writing);
    store->close();
  }

  sqlite::Database checkpoint(const std::string& path) {
    return sqlite::Database("file:" + path + "?immutable=1", sqlite::constants::SQLITE_OPEN_READONLY | sqlite::constants::SQLITE_OPEN_URI);
  }

  struct Fixture {
    const std::string dir = rt::fs::mkdtempSync(rt::os::tmpdir() + "/codex-auth-import-");
    const std::string source = dir + "/source.sqlite3";
    const std::string destination = dir + "/destination.sqlite3";
    const std::string savedBase = uos::config::config.codexBaseUrl;
    const rt::env::ScopedOverride follower{{"UOS_GATEWAY_UPSTREAM_AUTH_FOLLOWER", "1"}};
    const double now = rt::dateNow();
    const CodexAuthState oldA = credential("account-a", "old-a", now, 800'000);
    const CodexAuthState oldB = credential("account-b", "old-b", now, 700'000);
    const CodexAuthState newA = credential("account-a", "new-a", now + 1, 1'200'000);
    const CodexAuthState newB = credential("account-b", "new-b", now + 1, 1'300'000);
    int probes = 0;
    int otherCalls = 0;
    int statusB = 200;
    rt::GlobalFetchOverride fetch{rt::Fetch([this](rt::RequestInfo input, rt::RequestInit init) {
      return usage(std::move(input), std::move(init));
    })};

    Fixture() { uos::config::config.codexBaseUrl = "http://127.0.0.1:1/codex"; }
    ~Fixture() {
      uos::config::config.codexBaseUrl = savedBase;
      rt::fs::rmSync(dir, {.recursive = true, .force = true});
    }

    rt::Task<rt::Response> usage(rt::RequestInfo input, rt::RequestInit init) {
      const auto* url = std::get_if<std::string>(&input);
      if (!url || *url != USAGE_URL) {
        ++otherCalls;
        throw rt::JsError("The import fixture allows only the local usage probe.");
      }
      ++probes;
      EXPECT_EQ(init.method, "GET");
      EXPECT_EQ(init.redirect, "manual");
      ASSERT(init.headers);
      const auto account = init.headers->get("ChatGPT-Account-ID");
      EXPECT(account == newA.account_id || account == newB.account_id);
      EXPECT_EQ(init.headers->get("Authorization"), "Bearer " + (account == newA.account_id ? newA.access_token : newB.access_token));
      const int status = account == newA.account_id ? 200 : statusB;
      co_return rt::Response("{}", {.status = static_cast<double>(status), .headers = rt::Headers{{"Location", "http://127.0.0.1:1/oauth/token"}}});
    }

    rt::Task<void> prepare(bool updateBoth) {
      const CodexAuthPoolState oldPool{{oldA, oldB}, now};
      auto writeFollower = seed(destination, oldPool, "follower data");
      co_await std::move(writeFollower);
      CodexAuthPoolState newPool{{newA}, now + 1};
      if (updateBoth) newPool.accounts.push_back(newB);
      auto writeLeader = seed(source, newPool, "leader data");
      co_await std::move(writeLeader);
    }

    rt::Task<std::size_t> run() const { return uos::codex::auth_import::importCodexAuthPool(source, destination); }
  };
}

ASYNC_TEST("stopped-state auth import adopts the newer account and preserves unrelated raw rows") {
  Fixture fixture;
  auto preparing = fixture.prepare(false);
  co_await std::move(preparing);
  // A full serving KV opener would delete this expired record as a side effect.
  const auto key = rt::packKey(UNRELATED);
  {
    const sqlite::Database db(fixture.destination, {.readwrite = true});
    db.run("UPDATE kv SET expiration_ms = 1 WHERE k = ?", key);
  }
  const std::string rowSql = "SELECT v, version, seq, expiration_ms FROM kv WHERE k = ?";
  const auto before = checkpoint(fixture.destination).query(rowSql).get(key);
  ASSERT(before);
  const auto sourceBytes = rt::fs::readFileSync(fixture.source);
  EXPECT_EQ(co_await fixture.run(), 1U);
  EXPECT_EQ(co_await fixture.run(), 0U);
  EXPECT_EQ(fixture.probes, 1);
  EXPECT_EQ(fixture.otherCalls, 0);
  EXPECT_EQ(rt::fs::readFileSync(fixture.source), sourceBytes);
  const auto db = checkpoint(fixture.destination);
  const auto after = db.query(rowSql).get(key);
  ASSERT(after);
  for (const auto* column : {"v", "version", "seq", "expiration_ms"})
    EXPECT_EQ(*after->get(column), *before->get(column));
  const auto stored = db.query("SELECT v FROM kv WHERE k = ?").get(rt::packKey(auth::CODEX_AUTH_POOL_KV_KEY));
  ASSERT(stored);
  const auto value = rt::kvJscDeserialize(std::get<sqlite::Bytes>(*stored->get("v")));
  const auto pool = auth::parseCodexAuthPool(rt::storedJsonView(value));
  ASSERT(pool);
  EXPECT_EQ(pool->accounts, (std::vector<CodexAuthState>{fixture.newA, fixture.oldB}));
}

ASYNC_TEST("stopped-state auth import aborts the whole update on a rejected or redirected bearer") {
  Fixture fixture;
  auto preparing = fixture.prepare(true);
  co_await std::move(preparing);
  const auto before = rt::fs::readFileSync(fixture.destination);
  for (const int status : {401, 302}) {
    fixture.statusB = status;
    bool rejected = false;
    try { co_await fixture.run(); }
    catch (const rt::JsError& error) { rejected = error.message == "The Codex usage probe did not accept a leader access token."; }
    EXPECT(rejected);
    EXPECT_EQ(rt::fs::readFileSync(fixture.destination), before);
  }
  EXPECT_EQ(fixture.probes, 4);
  EXPECT_EQ(fixture.otherCalls, 0);
}
