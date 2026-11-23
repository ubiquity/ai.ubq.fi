// Links: tests/claude_code_quota_differential_test.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage.cpp src/provider/claude_code_usage_schema.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/utils.cpp
// Differential cases for src/provider/claude-code-quota.ts and parseClaudeCodeUsagePayload of
// src/provider/claude-code-usage.ts. The vectors (tests/claude_code_quota_differential_vectors.inc) were recorded from the
// TS modules under bun 1.4.0 with Date.now fixed, crypto.randomUUID a counter, the provider credential variables unset,
// console.warn captured and an in-memory Deno KV whose operations complete one macrotask later. The KV scenarios repeat
// the generator's steps one for one. Do not link src/provider/capacity.cpp here: it installs its own observation recorder.
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/claude_code_usage.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace uos;
namespace quota = provider::claude_code_quota;
namespace accounts = provider::claude_code_accounts;
using provider::claude_code::ClaudeCodeEnvLookup;

namespace {
  const char* const VECTORS =
#include "tests/claude_code_quota_differential_vectors.inc"
    ;

  constexpr double NOW = 1'700'000'000'000;

  const json::object& vectors() {
    static const json::object parsed = rt::jsonParse(VECTORS).as_object();
    return parsed;
  }
  const json::array& pureCases(std::string_view fn) { return rt::getObject(vectors(), "pure")->at(fn).as_array(); }
  const json::array& scenarioSteps(std::string_view name) { return rt::getObject(vectors(), "scenarios")->at(name).as_array(); }

  void expectJson(const json::value& actual, const json::value& expected, const std::string& label, const char* file, int line) {
    if (!rt::deepEqual(actual, expected))
      rt::test::report(file, line, label + "\n      actual:   " + json::serialize(actual) + "\n      expected: " + json::serialize(expected), false);
  }
#define EXPECT_JSON(actual, expected, label) expectJson((actual), (expected), (label), __FILE__, __LINE__)

  template <class T> json::value plain(const T& value) { return json::value_from(value); }
  json::value plain(const quota::ClaudeCodeQuotaExhaustion& value) {
    json::object out;
    out["window"] = json::string(value.window);
    out["reset_at_ms"] = value.reset_at_ms ? rt::makeNumber(*value.reset_at_ms) : json::value(nullptr);
    return out;
  }
  template <class T> json::value plain(const std::optional<T>& value) { return value ? plain(*value) : json::value(nullptr); }
  json::value plain(const json::value& value) { return value; }
  json::value plain(const rt::KvKey& key) {
    json::array out;
    for (const rt::KvKeyPart& part : key) out.emplace_back(json::string(part.string()));
    return out;
  }

  json::value ok(json::value value) {
    json::object out;
    out["ok"] = std::move(value);
    return out;
  }

  template <class T> rt::Task<json::value> capture(rt::Task<T> task) {
    rt::Reason error;
    json::value out;
    try {
      T value = co_await std::move(task);
      out = ok(plain(value));
    } catch (...) {
      error = std::current_exception();
    }
    if (error) {
      json::object failed;
      failed["error"] = rt::reasonToString(error);
      out = std::move(failed);
    }
    co_return out;
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

  // The generator's recorder: `events.push(event)`, and a throw for the "explode" claim.
  std::vector<json::value>& recordedEvents() {
    static std::vector<json::value> events;
    return events;
  }
  rt::Task<void> recordEvent(std::shared_ptr<rt::Kv>, quota::ClaudeCodeAcceptedObservation event) {
    recordedEvents().push_back(json::value_from(event));
    if (event.observation.representative_claim == std::optional<std::string>("explode")) throw rt::JsError("history down");
    co_return;
  }
  rt::Task<void> otherRecorder(std::shared_ptr<rt::Kv>, quota::ClaudeCodeAcceptedObservation) { co_return; }

  double numberOf(const json::value& value) {
    if (value.is_string() && value.get_string() == "NaN") return rt::Number::NaN;
    return rt::toNumber(value);
  }

  rt::Headers headersOf(const json::value& pairs) {
    std::vector<rt::Headers::Entry> entries;
    for (const json::value& pair : pairs.as_array())
      entries.emplace_back(std::string(pair.as_array()[0].as_string()), std::string(pair.as_array()[1].as_string()));
    return rt::Headers(entries);
  }

  std::optional<quota::ClaudeCodeQuotaObservation> observationOf(const json::value& value) {
    if (value.is_null()) return std::nullopt;
    return json::value_to<quota::ClaudeCodeQuotaObservation>(value);
  }

  struct Scenario {
    explicit Scenario(std::string_view name_)
      : name(name_),
        expected(scenarioSteps(name_)),
        env({{"CLAUDE_API_KEY", std::nullopt}, {"CLAUDE_CODE_OAUTH_TOKEN", std::nullopt}, {"CLAUDE_CODE_WORKSPACE_ID", std::nullopt}}),
        random(uuidSource()),
        warnings({rt::console::Method::warn}),
        kv(std::make_shared<rt::MemoryKv>([] { return NOW; })) {
      rt::globalDateNow() = [] { return NOW; };
      recordedEvents().clear();
      dispose = quota::registerClaudeCodeObservationRecorder(recordEvent);
    }
    ~Scenario() { dispose(); }
    void step(const json::value& actual, const char* file, int line) {
      const std::string label = name + " step " + std::to_string(index);
      if (index >= expected.size()) rt::test::report(file, line, label + ": more steps than the generator recorded", false);
      else expectJson(actual, expected[index], label, file, line);
      ++index;
    }
    rt::Task<void> finish(const char* file, int line) {
      const rt::KvEntryMaybe<> entry = co_await kv->get(quota::CLAUDE_CODE_QUOTA_KV_KEY);
      json::object out;
      out["quota"] = entry.value.toJson();
      json::array events;
      for (const json::value& event : recordedEvents()) events.push_back(event);
      out["events"] = std::move(events);
      json::array texts;
      for (const rt::console::Call& call : warnings.calls()) texts.emplace_back(json::string(call.text));
      out["warnings"] = std::move(texts);
      step(out, file, line);
      if (index != expected.size()) rt::test::report(file, line, name + ": fewer steps than the generator recorded", false);
    }

    std::string name;
    const json::array& expected;
    std::size_t index = 0;
    rt::env::ScopedOverride env;
    rt::crypto::ScopedRandomSource random;
    rt::console::Capture warnings;
    std::shared_ptr<rt::Kv> kv;
    std::function<void()> dispose;
  };
#define STEP(scenario, task)                                        \
  do {                                                              \
    auto uosStepTask = (task);                                      \
    auto uosStepCapture = capture(std::move(uosStepTask));          \
    json::value uosStepResult = co_await std::move(uosStepCapture); \
    (scenario).step(uosStepResult, __FILE__, __LINE__);             \
  } while (0)

  std::optional<quota::ClaudeCodeQuotaObservation> observed(std::vector<rt::Headers::Entry> pairs, double at) {
    return quota::observeClaudeCodeQuota(rt::Headers(pairs), at);
  }

  quota::QuotaOutcome outcome(std::optional<quota::ClaudeCodeQuotaObservation> observation) {
    quota::QuotaOutcome input;
    input.observation = std::move(observation);
    return input;
  }

  rt::Task<json::value> guardsAt(std::shared_ptr<rt::Kv> kv) {
    auto loading = quota::loadClaudeCodeQuotaState(kv);
    const std::optional<quota::ClaudeCodeQuotaState> state = co_await std::move(loading);
    json::array out;
    for (const double at : {NOW, NOW + 30'000, NOW + 700'000, 1'700'086'400'000.0 + 300'000}) {
      json::array pair;
      pair.emplace_back(quota::claudeCodeQuotaBlocked(state, at));
      pair.emplace_back(quota::claudeCodeQuotaCoolingDown(state, at));
      out.emplace_back(std::move(pair));
    }
    co_return json::value(std::move(out));
  }

  rt::Task<json::value> proofsFor(std::shared_ptr<rt::Kv> kv, accounts::ClaudeCodeAccountBinding binding) {
    auto loading = quota::loadClaudeCodeQuotaState(kv, binding);
    const std::optional<quota::ClaudeCodeQuotaState> state = co_await std::move(loading);
    json::array out;
    for (const char* model : {"claude-opus", "claude-sonnet", "claude-x"}) {
      json::array pair;
      pair.push_back(plain(quota::claudeCodePoolProofForModel(state, binding, model)));
      pair.push_back(plain(quota::claudeCodePoolProofForModel(state, binding, model, NOW + 400'000)));
      out.emplace_back(std::move(pair));
    }
    co_return json::value(std::move(out));
  }

  rt::Task<json::value> rawValue(std::shared_ptr<rt::Kv> kv, rt::KvKey key) {
    const rt::KvEntryMaybe<> entry = co_await kv->get(std::move(key));
    co_return entry.value.toJson();
  }
}

TEST("pure quota helpers match the TS") {
  for (const json::value& item : pureCases("observe")) {
    const json::value& input = item.at("input");
    EXPECT_JSON(plain(quota::observeClaudeCodeQuota(headersOf(input.at("headers")), numberOf(input.at("nowMs")))), item.at("output"),
                "observe " + json::serialize(input));
  }
  for (const json::value& item : pureCases("sanitize")) {
    const json::value& input = item.at("input");
    const json::value* raw = rt::get(input, "raw");
    const rt::KvValue value = raw != nullptr ? rt::KvValue(*raw) : rt::KvValue();
    const json::value* maxLength = rt::get(input, "maxLength");
    const std::optional<std::string> result =
      maxLength != nullptr ? quota::sanitizeClaudeCodeErrorMessage(value, rt::toNumber(*maxLength)) : quota::sanitizeClaudeCodeErrorMessage(value);
    EXPECT_JSON(result ? json::value(*result) : json::value(nullptr), item.at("output"), "sanitize " + json::serialize(input));
  }
  for (const json::value& item : pureCases("parseError"))
    EXPECT_JSON(plain(quota::parseClaudeCodeQuotaError(rt::KvValue(item.at("input")))), item.at("output"), "parseError " + json::serialize(item.at("input")));
  for (const json::value& item : pureCases("exhaustion")) {
    const json::value& input = item.at("input");
    EXPECT_JSON(plain(quota::claudeCodeExhaustionFromResponse(rt::toNumber(input.at("status")), observationOf(input.at("observation")))), item.at("output"),
                "exhaustion " + json::serialize(input));
  }
  for (const json::value& item : pureCases("parseState"))
    EXPECT_JSON(plain(quota::parseClaudeCodeQuotaState(rt::KvValue(item.at("input")))), item.at("output"), "parseState " + json::serialize(item.at("input")));
  for (const json::value& item : pureCases("merge")) {
    const json::value& input = item.at("input");
    const std::optional<quota::ClaudeCodeQuotaState> previous =
      input.at("previous").is_null() ? std::nullopt : std::optional(json::value_to<quota::ClaudeCodeQuotaState>(input.at("previous")));
    const quota::ClaudeCodeQuotaObservation observation = json::value_to<quota::ClaudeCodeQuotaObservation>(input.at("observation"));
    std::optional<quota::ClaudeCodeQuotaExhaustion> block;
    if (!input.at("block").is_null())
      block = quota::ClaudeCodeQuotaExhaustion{.window = std::string(input.at("block").at("window").as_string()),
                                               .reset_at_ms = rt::getNumber(input.at("block").as_object(), "reset_at_ms")};
    EXPECT_JSON(plain(quota::mergeClaudeCodeQuotaState(previous, observation, block, rt::toNumber(input.at("nowMs")))), item.at("output"),
                "merge " + json::serialize(input));
  }
  for (const json::value& item : pureCases("usagePayload")) {
    const json::value& input = item.at("input");
    EXPECT_JSON(plain(provider::claude_code_usage::parseClaudeCodeUsagePayload(input.at("payload"), rt::toNumber(input.at("observedAtMs")))), item.at("output"),
                "usagePayload " + json::serialize(input));
  }
}

TEST("observation recorder registration follows the TS identity rules") {
  const json::array& expected = vectors().at("registration").as_array();
  const std::function<void()> dispose = quota::registerClaudeCodeObservationRecorder(recordEvent);
  json::value same;
  try {
    (void)quota::registerClaudeCodeObservationRecorder(recordEvent);
    same = ok("same recorder accepted");
  } catch (const rt::JsError& error) {
    same = json::object{{"error", error.toString()}};
  }
  EXPECT_JSON(same, expected[0], "same recorder");
  json::value other;
  try {
    (void)quota::registerClaudeCodeObservationRecorder(otherRecorder);
    other = ok("other recorder accepted");
  } catch (const rt::JsError& error) {
    other = json::object{{"error", error.toString()}};
  }
  EXPECT_JSON(other, expected[1], "different recorder");
  dispose();
  json::value afterDispose;
  std::function<void()> disposeOther;
  try {
    disposeOther = quota::registerClaudeCodeObservationRecorder(otherRecorder);
    afterDispose = ok("other recorder accepted");
  } catch (const rt::JsError& error) {
    afterDispose = json::object{{"error", error.toString()}};
  }
  EXPECT_JSON(afterDispose, expected[2], "after dispose");
  if (disposeOther) disposeOther();
}

ASYNC_TEST("unbound outcomes") {
  Scenario s("unbound outcomes");
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, outcome(std::nullopt)));
  const auto first = observed({{"anthropic-ratelimit-unified-5h-status", "allowed"}, {"anthropic-ratelimit-unified-5h-utilization", "0.25"}}, NOW - 1000);
  quota::QuotaOutcome served = outcome(first);
  served.served = true;
  served.nowMs = NOW - 1000;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, served));
  const auto limited = observed({{"anthropic-ratelimit-unified-7d-status", "rejected"},
                                 {"anthropic-ratelimit-unified-7d-reset", "1700086400"},
                                 {"anthropic-ratelimit-unified-representative-claim", "seven_day"}},
                                NOW);
  quota::QuotaOutcome exhausted = outcome(limited);
  exhausted.exhaustion = quota::claudeCodeExhaustionFromResponse(429, limited);
  exhausted.response_status = 429;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, exhausted));
  quota::QuotaOutcome transient = outcome(std::nullopt);
  transient.transient = quota::QuotaOutcome::Transient{.retry_after_ms = 30'000};
  transient.nowMs = NOW + 10;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, transient));
  quota::QuotaOutcome zeroRetry = outcome(std::nullopt);
  zeroRetry.transient = quota::QuotaOutcome::Transient{.retry_after_ms = 0};
  zeroRetry.nowMs = NOW + 20;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, zeroRetry));
  quota::QuotaOutcome billing = outcome(std::nullopt);
  billing.config_error = quota::QuotaOutcome::ConfigError{.kind = "billing", .status = 402, .message = "  billing   blocked \n now ", .retry_after_ms = std::nullopt};
  billing.nowMs = NOW + 30;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, billing));
  quota::QuotaOutcome workspace = outcome(std::nullopt);
  workspace.config_error = quota::QuotaOutcome::ConfigError{.kind = "workspace", .status = std::nullopt, .message = std::nullopt, .retry_after_ms = 5};
  workspace.nowMs = NOW + 31;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, workspace));
  quota::QuotaOutcome servedAgain = outcome(std::nullopt);
  servedAgain.served = true;
  servedAgain.nowMs = NOW + 40;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, servedAgain));
  const auto direct = provider::claude_code_usage::parseClaudeCodeUsagePayload(rt::jsonParse(R"({"five_hour":{"utilization":42,"resets_at":"2026-10-25T23:00:00Z"}})"), NOW + 50);
  quota::QuotaOutcome usageRead = outcome(direct);
  usageRead.usage = quota::ClaudeCodeUsageState{
    .source = "oauth_usage", .state = "ok", .status = 200, .message = std::nullopt, .attempted_at_ms = NOW + 50, .observed_at_ms = NOW + 50};
  usageRead.nowMs = NOW + 50;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, usageRead));
  quota::QuotaOutcome olderUsage = outcome(std::nullopt);
  olderUsage.usage = quota::ClaudeCodeUsageState{.source = "oauth_usage",
                                                 .state = "rate_limited",
                                                 .status = 429,
                                                 .message = "x",
                                                 .attempted_at_ms = NOW + 49,
                                                 .observed_at_ms = std::nullopt,
                                                 .retry_at_ms = NOW + 99};
  olderUsage.nowMs = NOW + 60;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, olderUsage));
  const auto headerObs = observed({{"anthropic-ratelimit-unified-5h-utilization", "0.5"}, {"anthropic-ratelimit-requests-remaining", "3"}}, NOW + 70);
  quota::QuotaOutcome fromHeaders = outcome(headerObs);
  fromHeaders.observation_source = "response_headers";
  fromHeaders.served = true;
  fromHeaders.nowMs = NOW + 70;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, fromHeaders));
  const auto older = observed({{"anthropic-ratelimit-unified-5h-utilization", "0.1"}, {"anthropic-ratelimit-unified-representative-claim", "five_hour"}}, NOW - 5000);
  quota::QuotaOutcome olderHeaders = outcome(older);
  olderHeaders.observation_source = "response_headers";
  olderHeaders.nowMs = NOW + 80;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, olderHeaders));
  STEP(s, quota::loadClaudeCodeQuotaState(s.kv));
  STEP(s, guardsAt(s.kv));
  STEP(s, quota::clearClaudeCodeQuotaGuards(s.kv));
  quota::QuotaOutcome nullServed = outcome(first);
  nullServed.served = true;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(nullptr, nullServed));
  STEP(s, quota::recordClaudeCodeQuotaOutcome(nullptr, outcome(std::nullopt)));
  STEP(s, quota::clearClaudeCodeQuotaGuards(nullptr));
  STEP(s, quota::loadClaudeCodeQuotaState(nullptr));
  co_await s.finish(__FILE__, __LINE__);
}

ASYNC_TEST("empty clear") {
  Scenario s("empty clear");
  STEP(s, quota::clearClaudeCodeQuotaGuards(s.kv));
  const rt::KvValue seeded(rt::jsonParse(R"({"windows":{},"updated_at_ms":1234,"blocked_until_ms":1700000060000,"pool_proofs":{"x":1}})"));
  auto seeding = s.kv->set(quota::CLAUDE_CODE_QUOTA_KV_KEY, seeded);
  co_await std::move(seeding);
  STEP(s, quota::clearClaudeCodeQuotaGuards(s.kv));
  co_await s.finish(__FILE__, __LINE__);
}

ASYNC_TEST("bound outcomes") {
  Scenario s("bound outcomes");
  const ClaudeCodeEnvLookup env = [](const std::string& name) -> std::optional<std::string> {
    if (name == "CLAUDE_CODE_OAUTH_TOKEN") return std::string("sk-ant-oat-fixture-first-account");
    return std::nullopt;
  };
  auto resolving = accounts::resolveClaudeCodeAccount(s.kv, env);
  const std::optional<accounts::ClaudeCodeAccountBinding> resolved = co_await std::move(resolving);
  s.step(ok(plain(resolved)), __FILE__, __LINE__);
  ASSERT(resolved.has_value());
  const accounts::ClaudeCodeAccountBinding binding = *resolved;
  s.step(ok(plain(quota::claudeCodeQuotaKey(binding))), __FILE__, __LINE__);
  s.step(ok(plain(quota::claudeCodeQuotaKey())), __FILE__, __LINE__);
  const auto limited = observed({{"anthropic-ratelimit-unified-5h-status", "rejected"}, {"anthropic-ratelimit-unified-5h-reset", "1700018000"}}, NOW);
  quota::QuotaOutcome exhausted = outcome(limited);
  exhausted.binding = binding;
  exhausted.model_id = "claude-opus";
  exhausted.exhaustion = quota::claudeCodeExhaustionFromResponse(429, limited);
  exhausted.response_status = 429;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, exhausted));
  quota::QuotaOutcome authInvalid = outcome(std::nullopt);
  authInvalid.binding = binding;
  authInvalid.model_id = "claude-sonnet";
  authInvalid.auth_invalid = true;
  authInvalid.response_observed_at_ms = NOW - 10;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, authInvalid));
  quota::QuotaOutcome pastReset = outcome(std::nullopt);
  pastReset.binding = binding;
  pastReset.model_id = "claude-haiku";
  pastReset.exhaustion = quota::ClaudeCodeQuotaExhaustion{.window = "7d", .reset_at_ms = NOW - 1};
  pastReset.nowMs = NOW + 1;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, pastReset));
  STEP(s, proofsFor(s.kv, binding));
  const auto explode = observed({{"anthropic-ratelimit-unified-representative-claim", "explode"}}, NOW + 2);
  quota::QuotaOutcome servedOpus = outcome(explode);
  servedOpus.binding = binding;
  servedOpus.model_id = "claude-opus";
  servedOpus.served = true;
  servedOpus.response_observed_at_ms = NOW + 2;
  servedOpus.nowMs = NOW + 3;
  servedOpus.observation_source = "response_headers";
  servedOpus.response_status = 200;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, servedOpus));
  STEP(s, quota::loadClaudeCodeQuotaState(s.kv, binding));
  STEP(s, quota::loadClaudeCodeQuotaState(s.kv));
  STEP(s, quota::clearClaudeCodeQuotaGuards(s.kv, binding));
  STEP(s, rawValue(s.kv, accounts::CLAUDE_CODE_ACCOUNTS_KEY));
  STEP(s, quota::loadClaudeCodeQuotaState(s.kv, binding));
  quota::QuotaOutcome stale = outcome(limited);
  stale.binding = binding;
  STEP(s, quota::recordClaudeCodeQuotaOutcome(s.kv, stale));
  STEP(s, quota::clearClaudeCodeQuotaGuards(s.kv, binding));
  auto renewing = accounts::resolveClaudeCodeAccount(s.kv, env);
  const std::optional<accounts::ClaudeCodeAccountBinding> renewed = co_await std::move(renewing);
  ASSERT(renewed.has_value());
  STEP(s, quota::loadClaudeCodeQuotaState(s.kv, *renewed));
  STEP(s, quota::clearClaudeCodeQuotaGuards(s.kv, *renewed));
  STEP(s, quota::loadClaudeCodeQuotaState(s.kv, *renewed));
  co_await s.finish(__FILE__, __LINE__);
}
