// Port of tests/codex-auth-repair.test.ts. The assertions on scripts/codex-auth-repair.ts (reportStatus,
// accountAction, anyCodexAuthSlotNeedsRepair) in "the repair CLI renders an inconclusive slot as
// inconclusive and never probes sources for it" are not ported: scripts/ is outside the C++ port; the
// planCodexAuthRepair part of that case is.
// The last case is differential: planCodexAuthRepair excludes the chosen candidate from `rejected` by
// identity, so an identical twin of the chosen candidate is still reported (values recorded from Bun).
// Every case clears HOME, CODEX_HOME, CODEX_AUTH_JSON_B64 and config.codexAuthJsonB64 for its scope.
// Links: tests/codex_auth_repair_test.cpp src/codex/auth_repair.cpp src/codex/auth.cpp src/codex/native_auth.cpp
//        src/codex/native_auth_transport.cpp src/codex/external_auth.cpp src/codex/capacity_routing.cpp src/codex/rate_limit_429.cpp
//        src/codex/routing_mutations.cpp src/codex/routing_state.cpp src/authority/native_codex_429.cpp
//        src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
//        src/authority/native_codex_mutations.cpp src/bounded_response_body.cpp src/config.cpp src/kv.cpp
//        src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/health.cpp src/release.cpp
//        src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp
//        src/runtime/http.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/value_compatibility.cpp src/utils.cpp src/types.cpp
#include "uos/codex/auth_repair.hpp"

#include "uos/config.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::codex::auth_repair;
  using uos::types::CodexAuthPoolState;
  using uos::types::CodexAuthState;

  // No credential home and no seeded auth.json for the case's scope.
  struct IsolatedEnvironment {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    std::string savedAuthJson = uos::config::config.codexAuthJsonB64;
    IsolatedEnvironment() { uos::config::config.codexAuthJsonB64.clear(); }
    ~IsolatedEnvironment() { uos::config::config.codexAuthJsonB64 = savedAuthJson; }
    IsolatedEnvironment(const IsolatedEnvironment&) = delete;
    IsolatedEnvironment& operator=(const IsolatedEnvironment&) = delete;
  };

  const double NOW_MS = rt::Date::UTC(2026, 8, 16, 18, 0, 0);

  /** Drops base64url `=` padding without a backtracking-prone trailing regex. */
  std::string stripBase64Padding(const std::string& value) {
    std::size_t end = value.size();
    while (end > 0 && value[end - 1] == '=') end -= 1;
    return value.substr(0, end);
  }

  /** Builds a three-part token whose payload carries the requested `exp` claim. */
  std::string tokenExpiringAt(std::optional<double> expMs) {
    const auto encode = [](const json::value& value) {
      const std::string base64 = rt::replaceAll(rt::replaceAll(rt::btoa(rt::jsonStringify(value)), "+", "-"), "/", "_");
      return stripBase64Padding(base64);
    };
    json::object payload;
    if (expMs) payload["exp"] = rt::makeNumber(rt::Math::floor(*expMs / 1000));
    return "header." + encode(payload) + ".signature";
  }

  CodexAuthState account(const std::string& accountId, std::optional<double> expMs, std::optional<std::string> refresh = std::nullopt) {
    return CodexAuthState{
      .access_token = tokenExpiringAt(expMs),
      .refresh_token = refresh ? *refresh : "refresh-" + accountId,
      .account_id = accountId,
      .updated_at_ms = NOW_MS - 60'000,
    };
  }

  CodexAuthPoolState poolOf(std::vector<CodexAuthState> accounts) { return CodexAuthPoolState{.accounts = std::move(accounts), .updated_at_ms = NOW_MS - 60'000}; }

  struct CandidateOptions {
    std::optional<std::string> refresh{};
    // `updated_at_ms?: number | null`: undefined is NOW_MS.
    rt::Nullable<double> updated_at_ms{};
  };

  CodexAuthCandidate candidate(const std::string& source, const std::string& accountId, std::optional<double> expMs, const CandidateOptions& options = {}) {
    return CodexAuthCandidate{
      .source = source,
      .account_id = accountId,
      .access_token = tokenExpiringAt(expMs),
      .refresh_token = options.refresh ? *options.refresh : "refresh-" + accountId,
      .updated_at_ms = options.updated_at_ms.isUndefined() ? std::optional<double>(NOW_MS) : options.updated_at_ms.toOptional(),
    };
  }

  std::optional<double> expFromToken(const std::string& token) {
    const std::vector<std::string> parts = rt::split(token, ".");
    const std::string part = parts.size() > 1 ? parts[1] : "";
    if (part.empty()) return std::nullopt;
    try {
      const json::value parsed = rt::jsonParse(rt::atob(rt::replaceAll(rt::replaceAll(part, "-", "+"), "_", "/")));
      const std::optional<double> exp = rt::getNumber(parsed, "exp");
      return exp ? std::optional<double>(*exp * 1000) : std::nullopt;
    } catch (const rt::JsError&) {
      return std::nullopt;
    }
  }

  AssessedCodexAuthCandidate assessed(const CodexAuthCandidate& value, CodexAuthProbeResult probe) {
    return AssessedCodexAuthCandidate{.candidate = value, .access_exp_ms = expFromToken(value.access_token), .probe = std::move(probe)};
  }

  CodexAuthProbeResult valid(double status = 200) { return CodexAuthProbeResultValid{.status = status}; }
  CodexAuthProbeResult invalid(double status = 401) { return CodexAuthProbeResultInvalid{.status = status}; }
  CodexAuthProbeResult inconclusive(std::optional<double> status = std::nullopt) { return CodexAuthProbeResultInconclusive{.status = status}; }

  CodexAuthRepairAccount localAccount(double slot, const CodexAuthState& value, CodexAuthProbeResult probe, std::optional<bool> refreshDiverged = std::nullopt) {
    return CodexAuthRepairAccount{.slot = slot, .account = value, .probe = std::move(probe), .refresh_diverged = refreshDiverged};
  }

  std::vector<std::string> rejectedLabels(const std::vector<CodexAuthRepairRejection>& rejected) {
    std::vector<std::string> labels;
    for (const CodexAuthRepairRejection& entry : rejected) labels.push_back(entry.source + ":" + entry.outcome);
    return labels;
  }
}

TEST("isCodexAuthCandidateUsable requires an accepted token that has not expired") {
  IsolatedEnvironment isolated;
  const CodexAuthCandidate live = candidate("vps-pool", "acct-a", NOW_MS + 3'600'000);
  EXPECT_EQ(isCodexAuthCandidateUsable(assessed(live, valid()), NOW_MS), true);
  EXPECT_EQ(isCodexAuthCandidateUsable(assessed(live, valid(429)), NOW_MS), true);
  EXPECT_EQ(isCodexAuthCandidateUsable(assessed(live, invalid()), NOW_MS), false);
  EXPECT_EQ(isCodexAuthCandidateUsable(assessed(live, inconclusive()), NOW_MS), false);

  // A token the upstream still accepts can be seconds from expiry; the claim is
  // checked on its own so a stale credential is never adopted.
  const CodexAuthCandidate expired = candidate("vps-pool", "acct-a", NOW_MS - 1);
  EXPECT_EQ(isCodexAuthCandidateUsable(assessed(expired, valid()), NOW_MS), false);
}

TEST("rankCodexAuthCandidates prefers the longest-lived credential and drops unusable ones") {
  IsolatedEnvironment isolated;
  const AssessedCodexAuthCandidate shortLived = assessed(candidate("vps-pool", "acct-a", NOW_MS + 60'000), valid());
  const AssessedCodexAuthCandidate longLived = assessed(candidate("local-auth-json", "acct-a", NOW_MS + 86'400'000), valid());
  const AssessedCodexAuthCandidate rejected = assessed(candidate("vps-auth-json", "acct-a", NOW_MS + 172'800'000), invalid());

  const std::vector<AssessedCodexAuthCandidate> ranked = rankCodexAuthCandidates({shortLived, longLived, rejected}, NOW_MS);
  std::vector<std::string> sources;
  for (const AssessedCodexAuthCandidate& entry : ranked) sources.push_back(entry.candidate.source);
  EXPECT_EQ(sources, (std::vector<std::string>{"local-auth-json", "vps-pool"}));
}

TEST("rankCodexAuthCandidates breaks expiry ties by freshness and then by source order") {
  IsolatedEnvironment isolated;
  const double expMs = NOW_MS + 3'600'000;
  const CodexAuthCandidate stale = candidate("vps-auth-json", "acct-a", expMs, {.updated_at_ms = NOW_MS - 90'000});
  const CodexAuthCandidate fresh = candidate("vps-pool", "acct-a", expMs, {.updated_at_ms = NOW_MS - 1'000});
  const CodexAuthCandidate untimed = candidate("vps-pool", "acct-b", expMs, {.updated_at_ms = nullptr});

  const std::vector<AssessedCodexAuthCandidate> ranked = rankCodexAuthCandidates({assessed(stale, valid()), assessed(fresh, valid()), assessed(untimed, valid())}, NOW_MS);
  std::vector<std::optional<double>> updated;
  for (const AssessedCodexAuthCandidate& entry : ranked) updated.push_back(entry.candidate.updated_at_ms);
  EXPECT_EQ(updated, (std::vector<std::optional<double>>{NOW_MS - 1'000, NOW_MS - 90'000, std::nullopt}));

  const std::vector<AssessedCodexAuthCandidate> sameExpirySameFreshness{
    assessed(candidate("vps-auth-json", "acct-a", expMs), valid()),
    assessed(candidate("local-auth-json", "acct-a", expMs), valid()),
  };
  std::vector<std::string> sources;
  for (const AssessedCodexAuthCandidate& entry : rankCodexAuthCandidates(sameExpirySameFreshness, NOW_MS)) sources.push_back(entry.candidate.source);
  EXPECT_EQ(sources, (std::vector<std::string>{"local-auth-json", "vps-auth-json"}));
}

TEST("planCodexAuthRepair leaves an authenticating account untouched") {
  IsolatedEnvironment isolated;
  const CodexAuthState local = account("acct-a", NOW_MS + 3'600'000);
  const CodexAuthRepairPlan plan = planCodexAuthRepair({
    .accounts = {localAccount(1, local, valid())},
    .candidates = {assessed(candidate("vps-pool", "acct-a", NOW_MS + 86'400'000), valid())},
    .nowMs = NOW_MS,
  });

  EXPECT_EQ(plan.selections.size(), std::size_t{0});
  EXPECT_EQ(plan.unrepairable.size(), std::size_t{0});
  const std::vector<CodexAuthRepairPlan::Healthy> expectedHealthy{{.slot = 1, .account_id = "acct-a", .refresh_diverged = std::nullopt}};
  EXPECT(plan.healthy == expectedHealthy);
}

TEST("planCodexAuthRepair treats a probe-valid but already expired local token as broken") {
  IsolatedEnvironment isolated;
  const CodexAuthRepairPlan plan = planCodexAuthRepair({
    .accounts = {localAccount(1, account("acct-a", NOW_MS - 1'000), valid())},
    .candidates = {assessed(candidate("vps-pool", "acct-a", NOW_MS + 3'600'000), valid())},
    .nowMs = NOW_MS,
  });

  EXPECT_EQ(plan.healthy.size(), std::size_t{0});
  ASSERT_EQ(plan.selections.size(), std::size_t{1});
  EXPECT_EQ(plan.selections[0].candidate.source, "vps-pool");
}

TEST("planCodexAuthRepair preserves an unexpired slot whose probe was inconclusive") {
  IsolatedEnvironment isolated;
  // A timeout and a 403/5xx are all undecidable: none of them is evidence that
  // the credential is unusable, so a valid candidate must not be adopted.
  const std::vector<CodexAuthProbeResult> undecidableProbes{inconclusive(std::nullopt), inconclusive(403), inconclusive(503)};
  for (const CodexAuthProbeResult& probe : undecidableProbes) {
    const CodexAuthPoolState pool = poolOf({account("acct-a", NOW_MS + 3'600'000)});
    const CodexAuthRepairPlan plan = planCodexAuthRepair({
      .accounts = {localAccount(1, pool.accounts[0], probe)},
      .candidates = {assessed(candidate("vps-pool", "acct-a", NOW_MS + 86'400'000), valid())},
      .nowMs = NOW_MS,
    });

    EXPECT(plan.selections.empty());
    EXPECT(plan.unrepairable.empty());
    EXPECT(plan.healthy.empty());
    EXPECT(plan.inconclusive == (std::vector<CodexAuthRepairPlan::Inconclusive>{
                                  {.slot = 1, .account_id = "acct-a", .probe = std::get<CodexAuthProbeResultInconclusive>(probe), .refresh_diverged = std::nullopt},
                                }));

    const std::optional<CodexAuthPoolState> applied = applyCodexAuthPlan(pool, plan, NOW_MS);
    ASSERT(applied.has_value());
    EXPECT(applied->accounts == pool.accounts);
    EXPECT_EQ(applied->accounts[0].access_token, pool.accounts[0].access_token);
  }
}

TEST("planCodexAuthRepair repairs a slot whose access token has verifiably expired even when the probe was inconclusive") {
  IsolatedEnvironment isolated;
  const CodexAuthRepairPlan plan = planCodexAuthRepair({
    .accounts = {localAccount(1, account("acct-a", NOW_MS - 1'000), inconclusive(503))},
    .candidates = {assessed(candidate("vps-pool", "acct-a", NOW_MS + 3'600'000), valid())},
    .nowMs = NOW_MS,
  });

  EXPECT(plan.inconclusive.empty());
  ASSERT_EQ(plan.selections.size(), std::size_t{1});
  EXPECT_EQ(plan.selections[0].candidate.source, "vps-pool");
}

TEST("the repair CLI renders an inconclusive slot as inconclusive and never probes sources for it") {
  IsolatedEnvironment isolated;
  const CodexAuthPoolState pool = poolOf({account("acct-a", NOW_MS + 3'600'000)});
  const CodexAuthRepairPlan plan = planCodexAuthRepair({
    .accounts = {localAccount(1, pool.accounts[0], inconclusive(403))},
    .candidates = {assessed(candidate("vps-pool", "acct-a", NOW_MS + 86'400'000), valid())},
    .nowMs = NOW_MS,
  });

  // The run cannot certify the pool, so it must not report health, and the row
  // must name the undecidable state instead of calling it kept.
  // reportStatus / accountAction / anyCodexAuthSlotNeedsRepair (scripts/codex-auth-repair.ts) are not ported.
  ASSERT_EQ(plan.inconclusive.size(), std::size_t{1});
  EXPECT(plan.healthy.empty());
  const CodexAuthRepairPlan healthy = planCodexAuthRepair({.accounts = {localAccount(1, pool.accounts[0], valid())}, .candidates = {}, .nowMs = NOW_MS});
  EXPECT_EQ(healthy.healthy.size(), std::size_t{1});
}

TEST("planCodexAuthRepair repairs a rejected slot from the best candidate for that account") {
  IsolatedEnvironment isolated;
  const CodexAuthRepairPlan plan = planCodexAuthRepair({
    .accounts = {localAccount(1, account("acct-a", NOW_MS + 3'600'000), invalid())},
    .candidates =
      {
        assessed(candidate("local-auth-json", "acct-a", NOW_MS + 60'000), valid()),
        assessed(candidate("vps-pool", "acct-a", NOW_MS + 86'400'000), valid()),
        assessed(candidate("vps-auth-json", "acct-a", NOW_MS + 172'800'000), invalid()),
      },
    .nowMs = NOW_MS,
  });

  ASSERT_EQ(plan.selections.size(), std::size_t{1});
  EXPECT_EQ(plan.selections[0].slot, 1.0);
  EXPECT_EQ(plan.selections[0].account_id, "acct-a");
  EXPECT_EQ(plan.selections[0].candidate.source, "vps-pool");
  EXPECT_EQ(rejectedLabels(plan.selections[0].rejected), (std::vector<std::string>{"local-auth-json:valid", "vps-auth-json:invalid"}));
}

TEST("planCodexAuthRepair never adopts another account's credential") {
  IsolatedEnvironment isolated;
  const CodexAuthRepairPlan plan = planCodexAuthRepair({
    .accounts = {localAccount(1, account("acct-a", NOW_MS + 3'600'000), invalid())},
    .candidates = {assessed(candidate("vps-pool", "acct-b", NOW_MS + 86'400'000), valid())},
    .nowMs = NOW_MS,
  });

  EXPECT_EQ(plan.selections.size(), std::size_t{0});
  ASSERT_EQ(plan.unrepairable.size(), std::size_t{1});
  EXPECT_EQ(plan.unrepairable[0].reason, "no_candidates");
}

TEST("planCodexAuthRepair reports a blocked account when no candidate authenticates") {
  IsolatedEnvironment isolated;
  const CodexAuthRepairPlan plan = planCodexAuthRepair({
    .accounts = {localAccount(1, account("acct-a", NOW_MS + 3'600'000), invalid())},
    .candidates =
      {
        assessed(candidate("vps-pool", "acct-a", NOW_MS + 86'400'000), invalid()),
        assessed(candidate("vps-auth-json", "acct-a", NOW_MS + 3'600'000), inconclusive()),
      },
    .nowMs = NOW_MS,
  });

  EXPECT_EQ(plan.selections.size(), std::size_t{0});
  ASSERT_EQ(plan.unrepairable.size(), std::size_t{1});
  EXPECT_EQ(plan.unrepairable[0].reason, "no_valid_candidate");
  EXPECT_EQ(rejectedLabels(plan.unrepairable[0].rejected), (std::vector<std::string>{"vps-pool:invalid", "vps-auth-json:inconclusive"}));
}

TEST("planCodexAuthRepair decides each slot independently") {
  IsolatedEnvironment isolated;
  const CodexAuthRepairPlan plan = planCodexAuthRepair({
    .accounts = {localAccount(1, account("acct-a", NOW_MS + 3'600'000), valid(), true), localAccount(2, account("acct-b", NOW_MS + 3'600'000), invalid())},
    .candidates =
      {
        assessed(candidate("vps-pool", "acct-a", NOW_MS + 86'400'000, {.refresh = "other"}), valid()),
        assessed(candidate("vps-pool", "acct-b", NOW_MS + 86'400'000), valid()),
      },
    .nowMs = NOW_MS,
  });

  std::vector<double> slots;
  for (const CodexAuthRepairSelection& entry : plan.selections) slots.push_back(entry.slot);
  EXPECT_EQ(slots, (std::vector<double>{2}));
  const std::vector<CodexAuthRepairPlan::Healthy> expectedHealthy{{.slot = 1, .account_id = "acct-a", .refresh_diverged = true}};
  EXPECT(plan.healthy == expectedHealthy);
}

TEST("applyCodexAuthPlan replaces only the affected account and preserves the rest") {
  IsolatedEnvironment isolated;
  const CodexAuthState untouched = account("acct-a", NOW_MS + 3'600'000);
  const CodexAuthState broken = account("acct-b", NOW_MS + 3'600'000);
  const CodexAuthPoolState pool = poolOf({untouched, broken});
  const CodexAuthCandidate replacement = candidate("vps-pool", "acct-b", NOW_MS + 86'400'000, {.refresh = "refresh-from-vps"});
  const CodexAuthRepairPlan plan = planCodexAuthRepair({
    .accounts = {localAccount(1, untouched, valid()), localAccount(2, broken, invalid())},
    .candidates = {assessed(replacement, valid())},
    .nowMs = NOW_MS,
  });

  const std::optional<CodexAuthPoolState> applied = applyCodexAuthPlan(pool, plan, NOW_MS);
  ASSERT(applied.has_value());
  ASSERT_EQ(applied->accounts.size(), std::size_t{2});
  EXPECT_EQ(applied->accounts[0].access_token, untouched.access_token);
  EXPECT_EQ(applied->accounts[0].refresh_token, untouched.refresh_token);
  EXPECT_EQ(applied->accounts[0].account_id, "acct-a");
  EXPECT_EQ(applied->accounts[1].account_id, "acct-b");
  EXPECT_EQ(applied->accounts[1].access_token, replacement.access_token);
  EXPECT_EQ(applied->accounts[1].refresh_token, "refresh-from-vps");
  EXPECT_EQ(applied->accounts[1].updated_at_ms, NOW_MS);

  // The stored pool is replaced by a new value; the caller holds the old shape.
  // (`assert.notEqual(applied, pool)` is an identity check: the C++ result is a separate value.)
  EXPECT(!(*applied == pool));
  EXPECT_EQ(pool.accounts[1].access_token, broken.access_token);
}

TEST("applyCodexAuthPlan refuses a candidate that expires during assessment") {
  IsolatedEnvironment isolated;
  const CodexAuthState untouched = account("acct-a", NOW_MS + 3'600'000);
  const CodexAuthState broken = account("acct-b", NOW_MS + 3'600'000);
  const CodexAuthPoolState pool = poolOf({untouched, broken});
  const CodexAuthCandidate replacement = candidate("vps-pool", "acct-b", NOW_MS + 1'000, {.refresh = "refresh-from-vps"});
  const CodexAuthRepairPlan plan = planCodexAuthRepair({
    .accounts = {localAccount(1, untouched, valid()), localAccount(2, broken, invalid())},
    .candidates = {assessed(replacement, valid())},
    .nowMs = NOW_MS,
  });

  EXPECT_EQ(plan.selections.size(), std::size_t{1});
  EXPECT(!applyCodexAuthPlan(pool, plan, NOW_MS + 1'000).has_value());
  EXPECT_EQ(pool.accounts[1].access_token, broken.access_token);
}

TEST("applyCodexAuthPlan with an empty plan returns an equivalent pool") {
  IsolatedEnvironment isolated;
  const CodexAuthPoolState pool = poolOf({account("acct-a", NOW_MS + 3'600'000)});
  const CodexAuthRepairPlan plan = planCodexAuthRepair({.accounts = {localAccount(1, pool.accounts[0], valid())}, .candidates = {}, .nowMs = NOW_MS});
  const std::optional<CodexAuthPoolState> applied = applyCodexAuthPlan(pool, plan, NOW_MS);
  ASSERT(applied.has_value());
  EXPECT(applied->accounts == pool.accounts);
}

TEST("codexAuthCandidateFromAuthJson accepts a real auth.json shape and rejects junk") {
  IsolatedEnvironment isolated;
  const std::optional<CodexAuthCandidate> parsed = codexAuthCandidateFromAuthJson(
    json::value(json::object{
      {"auth_mode", "chatgpt"},
      {"tokens", json::object{{"access_token", tokenExpiringAt(NOW_MS + 3'600'000)}, {"refresh_token", "refresh-a"}, {"account_id", "acct-a"}}},
      {"last_refresh", "2026-09-12T21:16:41.259598Z"},
    }),
    "vps-auth-json", NOW_MS);
  ASSERT(parsed.has_value());
  EXPECT_EQ(parsed->account_id, "acct-a");
  EXPECT_EQ(parsed->refresh_token, "refresh-a");
  EXPECT_EQ(parsed->updated_at_ms, std::optional<double>(NOW_MS));

  EXPECT(!codexAuthCandidateFromAuthJson(json::value(json::object{{"tokens", json::object{}}}), "vps-auth-json", std::nullopt).has_value());
  EXPECT(!codexAuthCandidateFromAuthJson(json::value(nullptr), "vps-auth-json", std::nullopt).has_value());
  EXPECT(!codexAuthCandidateFromAuthJson(json::value(json::object{{"tokens", json::object{{"access_token", "a"}, {"refresh_token", "b"}}}}), "vps-auth-json", std::nullopt)
            .has_value());
}

TEST("codexAuthCandidatesFromPool reads every stored account and rejects malformed pools") {
  IsolatedEnvironment isolated;
  const CodexAuthPoolState pool = poolOf({account("acct-a", NOW_MS + 3'600'000), account("acct-b", NOW_MS + 7'200'000)});
  const std::vector<CodexAuthCandidate> candidates = codexAuthCandidatesFromPool(json::value_from(pool), "vps-pool");
  std::vector<std::string> labels;
  for (const CodexAuthCandidate& entry : candidates) labels.push_back(entry.source + ":" + entry.account_id);
  EXPECT_EQ(labels, (std::vector<std::string>{"vps-pool:acct-a", "vps-pool:acct-b"}));

  EXPECT(codexAuthCandidatesFromPool(json::value(nullptr), "vps-pool").empty());
  EXPECT(codexAuthCandidatesFromPool(json::value(json::object{{"accounts", json::array{}}}), "vps-pool").empty());
  EXPECT(codexAuthCandidatesFromPool(json::value(json::object{{"accounts", json::array{json::object{{"account_id", "only-an-id"}}}}, {"updated_at_ms", rt::makeNumber(NOW_MS)}}),
                                     "vps-pool")
           .empty());
}

TEST("codexAuthCandidateKey deduplicates the same credential seen from two sources") {
  IsolatedEnvironment isolated;
  const CodexAuthCandidate first = candidate("local-auth-json", "acct-a", NOW_MS + 3'600'000);
  CodexAuthCandidate second = first;
  second.source = "vps-pool";
  const CodexAuthCandidate other = candidate("vps-pool", "acct-a", NOW_MS + 7'200'000);
  EXPECT_EQ(codexAuthCandidateKey(first), codexAuthCandidateKey(second));
  EXPECT(codexAuthCandidateKey(first) != codexAuthCandidateKey(other));
  EXPECT_EQ(codexAuthCandidateKey(first), "acct-a" + std::string(1, '\0') + first.access_token);
}

TEST("differential: an identical twin of the chosen candidate is still rejected, and plans serialize in declaration order") {
  IsolatedEnvironment isolated;
  // Bun: two structurally equal candidate objects; `assessed !== chosen` keeps the twin in `rejected`.
  const AssessedCodexAuthCandidate chosen = assessed(candidate("vps-pool", "acct-a", NOW_MS + 86'400'000), valid());
  const CodexAuthRepairPlan plan = planCodexAuthRepair({
    .accounts = {localAccount(1, account("acct-a", NOW_MS + 3'600'000), invalid()), localAccount(2, account("acct-b", NOW_MS + 3'600'000), inconclusive(503), false)},
    .candidates = {chosen, chosen, assessed(candidate("vps-auth-json", "acct-a", std::nullopt, {.updated_at_ms = nullptr}), valid())},
    .nowMs = NOW_MS,
  });
  EXPECT_EQ(rt::jsonStringify(json::value_from(plan)),
            "{\"selections\":[{\"slot\":1,\"account_id\":\"acct-a\",\"candidate\":{\"source\":\"vps-pool\",\"account_id\":\"acct-a\",\"access_token\":\"" +
              chosen.candidate.access_token +
              "\",\"refresh_token\":\"refresh-acct-a\",\"updated_at_ms\":1789581600000},\"access_exp_ms\":1789668000000,\"rejected\":[{\"source\":\"vps-pool\","
              "\"outcome\":\"valid\",\"status\":200},{\"source\":\"vps-auth-json\",\"outcome\":\"valid\",\"status\":200}]}],\"unrepairable\":[],\"healthy\":[],"
              "\"inconclusive\":[{\"slot\":2,\"account_id\":\"acct-b\",\"probe\":{\"outcome\":\"inconclusive\",\"status\":503},\"refresh_diverged\":false}]}");
  // The strict reader accepts what the writer produced.
  EXPECT(json::value_to<CodexAuthRepairPlan>(json::value_from(plan)) == plan);
}
