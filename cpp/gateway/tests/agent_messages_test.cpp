// Port of tests/agent-messages.test.ts, plus bounded validation cases.
// Links: tests/agent_messages_test.cpp
// src/agent_messages.cpp src/kv.cpp src/request.cpp src/utils.cpp
// src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/http.cpp src/auth/index.cpp
// src/auth/passport.cpp src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp
// src/auth/passport_sessions.cpp src/kernel/attestation.cpp src/auth/local_development_key.cpp src/auth/passkeys.cpp
// src/api_keys.cpp src/types.cpp src/paid_fallback/ledger_state.cpp src/kernel/policy_queue.cpp
// src/api_key_policy.cpp src/auth/local_admin.cpp src/kernel/quota_reservations.cpp src/authority/native_paid_contracts.cpp
// src/config.cpp src/authority/native_paid_policy.cpp src/kernel/quota_v2.cpp src/authority/native_kernel.cpp
// src/provider/surplus.cpp src/authority/native_paid.cpp src/analytics.cpp src/responses_stream.cpp
// src/storage/native_kv.cpp src/provider/anthropic_chat_responses.cpp src/provider/surplus_claude_chat_request.cpp src/provider/anthropic_response_wire.cpp
// src/provider/surplus_claude_request.cpp src/authority/native_kernel_contracts.cpp src/provider/anthropic_replay.cpp src/release.cpp
// src/paid_fallback/index.cpp src/paid_fallback/ledger_backfill.cpp src/provider/cerebras_rate_limits.cpp src/paid_fallback/ledger_admission.cpp
// src/auth/relay.cpp src/paid_fallback/ledger_settlement.cpp src/provider/metered.cpp src/paid_fallback/ledger_stats.cpp
// src/provider/anthropic_responses.cpp src/provider/anthropic_chat_stream.cpp src/models/codex_models_fetch.cpp src/codex/auth.cpp
// src/bounded_response_body.cpp src/codex/auth_refresh.cpp src/codex/dispatch.cpp src/models/codex_models.cpp
// src/models/codex_models_availability.cpp src/runtime_config.cpp src/authority/native_codex_contracts.cpp src/codex/native_auth.cpp
// src/provider/health.cpp src/defaults.cpp src/codex/external_auth.cpp src/provider/capacity_events.cpp
// src/provider/claude_code_accounts.cpp src/codex/routing_mutations.cpp src/authority/native_codex_capacity.cpp src/provider/capacity_contract.cpp
// src/codex/capacity_routing.cpp src/provider/claude_code_credentials.cpp src/authority/native_codex_mutations.cpp src/codex/rate_limit_429.cpp
// src/authority/native_codex_429.cpp src/authority/native_codex_evaluation.cpp src/codex/routing_state.cpp src/codex/banked_reset.cpp
// src/codex/native_auth_transport.cpp src/provider/capacity.cpp src/provider/claude_capacity_rollups.cpp src/cache/prompt_analytics_core.cpp
// src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/metered_quota.cpp src/cache/prompt_analytics_read.cpp
// src/provider/claude_capacity_history.cpp src/provider/capacity_rollups.cpp src/provider/claude_code_quota.cpp src/codex/banked_reset_provider.cpp
// src/runtime/websocket.cpp src/codex/reset_settings.cpp src/codex/experiment.cpp src/provider/claude_code_usage_schema.cpp
// src/provider/capacity_analytics.cpp src/codex/account_routing.cpp src/codex/routing_serial.cpp src/provider/selection.cpp
// src/authority/native_codex.cpp src/codex/routing_evaluation.cpp src/codex/overage_settings.cpp
#include "uos/agent_messages.hpp"
#include "uos/config.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt; namespace json = uos::json; namespace messages = uos::agent_messages; namespace auth = uos::auth::index;
  struct OwnedKv { rt::MemoryKv store{[] { return rt::dateNow(); }}; };
  class Iterator : public rt::KvListIterator {
  public:
    Iterator(std::vector<rt::KvEntry<>> rows, std::string cursor) : rows_(std::move(rows)), cursor_(std::move(cursor)) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override { if (index_ >= rows_.size()) return rt::fulfilledTask(std::optional<rt::KvEntry<>>()); return rt::fulfilledTask(std::optional(rows_[index_++])); }
    std::string cursor() const override { return cursor_; }
    rt::Task<void> return_() override { index_ = rows_.size(); return rt::fulfilledTask(); }
  private:
    std::vector<rt::KvEntry<>> rows_; std::string cursor_; std::size_t index_ = 0;
  };
  class MemoryKv : private OwnedKv, public rt::ForwardingKv {
  public:
    MemoryKv() : rt::ForwardingKv(OwnedKv::store) {}
    std::vector<rt::KvEntry<>> entries;
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override { const auto stamp = std::to_string(entries.size() + 1); entries.push_back({key, value, stamp}); return rt::fulfilledTask(rt::KvCommitResult{true, stamp}); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      std::vector<rt::KvEntry<>> rows;
      for (const auto& entry : entries) { bool matches = true; if (selector.prefix) { const auto& prefix = *selector.prefix; matches = prefix.size() <= entry.key.size() && std::equal(prefix.begin(), prefix.end(), entry.key.begin()); } if (matches && (!selector.start || rt::compareKvKeys(entry.key, *selector.start) >= 0)) rows.push_back(entry); }
      std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) { return rt::compareKvKeys(left.key, right.key) < 0; });
      const auto limit = options.limit ? static_cast<std::size_t>(*options.limit) : rows.size(); const auto cursor = rows.size() > limit ? "next" : ""; if (rows.size() > limit) rows.resize(limit);
      return std::make_unique<Iterator>(std::move(rows), cursor);
    }
  };
  messages::AgentMessagesDeps makeDeps(std::shared_ptr<MemoryKv> kv, double time = 1700000000000, std::string id = "msg-1") {
    messages::AgentMessagesDeps deps;
    deps.kv = kv; deps.now = [time] { return time; }; deps.uuid = [id] { return id; };
    deps.authenticateClient = [](rt::Request) { return rt::fulfilledTask<auth::AuthenticateClientResult>(auth::AuthenticateClientResultOk{std::string("ghs_test_token"), auth::ClientAuthMethodGithubToken{"acme", "demo", "state-1", "org"}}); }; return deps;
  }
  rt::Request jsonRequest(const json::value& value) { return rt::Request("https://ai.ubq.fi/uos/agent-messages", rt::RequestInit{.method = "POST", .headers = rt::Headers{{"Content-Type", "application/json"}}, .body = rt::BodyInit(rt::jsonStringify(value))}); }
}
ASYNC_TEST("agent-messages: post stores a message") {
  rt::env::ScopedEnvironment environment({}); auto kv = std::make_shared<MemoryKv>(); auto deps = makeDeps(kv);
  const json::object input{{"agent_id", "agent-1"}, {"body", "hello"}, {"channel", "general"}, {"kind", "note"}, {"metadata", json::object{{"ok", true}}}};
  AWAIT(response, messages::handleAgentMessagesPost(jsonRequest(input), deps)); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); EXPECT(!rt::hasOwn(payload.as_object(), "ok"));
  const json::object expected{{"id", "msg-1"}, {"owner", "acme"}, {"repo", "demo"}, {"state_id", "state-1"}, {"agent_id", "agent-1"}, {"channel", "general"}, {"kind", "note"}, {"body", "hello"}, {"metadata", json::object{{"ok", true}}}, {"created_at_ms", 1700000000000LL}};
  EXPECT(rt::deepEqual(*rt::get(payload, "message"), expected)); EXPECT_EQ(kv->entries.size(), std::size_t(1));
}
ASYNC_TEST("agent-messages: list returns messages and cursor info") {
  rt::env::ScopedEnvironment environment({}); auto kv = std::make_shared<MemoryKv>(); auto first = makeDeps(kv, 1000, "msg-1"), second = makeDeps(kv, 2000, "msg-2");
  AWAIT(a, messages::handleAgentMessagesPost(jsonRequest(json::object{{"agent_id", "agent-1"}, {"body", "one"}}), first)); AWAIT(b, messages::handleAgentMessagesPost(jsonRequest(json::object{{"agent_id", "agent-1"}, {"body", "two"}}), second));
  AWAIT(response, messages::handleAgentMessagesList(rt::Request("https://ai.ubq.fi/uos/agent-messages?limit=1"), first)); AWAIT(payload, response.json()); EXPECT_EQ(response.status(), 200); EXPECT(!rt::hasOwn(payload.as_object(), "ok")); const auto* list = rt::getArray(payload, "messages"); ASSERT(list); ASSERT_EQ(list->size(), std::size_t(1)); EXPECT_EQ(rt::getString((*list)[0], "id"), std::optional<std::string_view>("msg-1")); EXPECT_EQ(rt::getNumber(payload, "next_since"), std::optional(1000.0)); EXPECT_EQ(rt::getBool(payload, "has_more"), std::optional(true)); EXPECT_EQ(rt::getString(payload, "next_cursor"), std::optional<std::string_view>("next"));
}
ASYNC_TEST("agent-messages: list returns null next_since when empty") {
  rt::env::ScopedEnvironment environment({}); auto kv = std::make_shared<MemoryKv>(); auto deps = makeDeps(kv); AWAIT(response, messages::handleAgentMessagesList(rt::Request("https://ai.ubq.fi/uos/agent-messages"), deps)); AWAIT(payload, response.json()); EXPECT_EQ(response.status(), 200); EXPECT(rt::getArray(payload, "messages")->empty()); EXPECT(rt::get(payload, "next_since")->is_null());
}
ASYNC_TEST("agent-messages: validation preserves auth precedence, UTF-16 limits and array metadata") {
  rt::env::ScopedEnvironment environment({}); auto kv = std::make_shared<MemoryKv>(); auto deps = makeDeps(kv);
  const std::vector<json::value> invalid{nullptr, json::object(), json::object{{"agent_id", "a\nb"}, {"body", "x"}}, json::object{{"agent_id", "a"}, {"body", " "}}, json::object{{"agent_id", "a"}, {"body", "x"}, {"channel", ""}}, json::object{{"agent_id", "a"}, {"body", "x"}, {"kind", 1}}, json::object{{"agent_id", "a"}, {"body", "x"}, {"metadata", 1}}};
  for (const auto& input : invalid) { AWAIT(response, messages::handleAgentMessagesPost(jsonRequest(input), deps)); EXPECT_EQ(response.status(), 400); } EXPECT(kv->entries.empty());
  AWAIT(array, messages::handleAgentMessagesPost(jsonRequest(json::object{{"agent_id", " a "}, {"body", " x "}, {"metadata", json::array{1}}}), deps)); AWAIT(body, array.json()); EXPECT_EQ(array.status(), 200); EXPECT_EQ(rt::getString(*rt::get(body, "message"), "agent_id"), std::optional<std::string_view>("a")); EXPECT_EQ(rt::getString(*rt::get(body, "message"), "body"), std::optional<std::string_view>(" x "));
  deps.authenticateClient = [](rt::Request) { return rt::fulfilledTask<auth::AuthenticateClientResult>(auth::AuthenticateClientResultOk{std::nullopt, auth::ClientAuthMethodDisabled{}}); }; AWAIT(forbidden, messages::handleAgentMessagesPost(jsonRequest(nullptr), deps)); EXPECT_EQ(forbidden.status(), 403);
  deps.authenticateClient = [](rt::Request) { return rt::fulfilledTask<auth::AuthenticateClientResult>(auth::AuthenticateClientResultNotOk{rt::Response(nullptr, rt::ResponseInit{.status = 401})}); }; AWAIT(refused, messages::handleAgentMessagesList(rt::Request("https://ai.ubq.fi/uos/agent-messages"), deps)); EXPECT_EQ(refused.status(), 401);
}
