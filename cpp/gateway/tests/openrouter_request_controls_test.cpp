// Links: tests/openrouter_request_controls_test.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter.cpp src/provider/selection.cpp src/models/openrouter_models.cpp
//        src/kv.cpp src/openai_schema.cpp src/utils.cpp src/http.cpp
//        src/storage/kv.cpp src/auth/relay.cpp src/storage/sqlite_readonly.cpp src/config.cpp
//        src/provider/cerebras_rate_limits.cpp src/release.cpp src/storage/native_kv.cpp
// Direct cache-control contracts from tests/public-provider-cache-controls.test.ts; public route integration awaits the handlers.
#include "uos/provider/openrouter_request_controls.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/provider/selection.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/openai_schema.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt; namespace json = uos::json; namespace controls = uos::provider::openrouter_request_controls;
  namespace selection = uos::provider::selection; namespace models = uos::models::openrouter_models;
  const std::string CLAUDE_MODEL = "~anthropic/claude-opus-latest";
  struct Scope {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w05c2"}, {"OPENROUTER_API_KEY", "fixture-key"}}};
    std::shared_ptr<rt::MemoryKv> kv = std::make_shared<rt::MemoryKv>(rt::dateNow);
    Scope() { uos::kv::setKvForTest(kv); selection::resetProviderSelectionCacheForTest(); models::resetOpenRouterModelsCacheForTest(); }
    ~Scope() { uos::kv::setKvForTest(nullptr); selection::resetProviderSelectionCacheForTest(); models::setOpenRouterModelsFetchForTest({}); models::resetOpenRouterModelsCacheForTest(); }
  };
}

ASYNC_TEST("Claude native cache extension stays separate from official schemas and checks enabled served models") {
  Scope isolation;
  const auto& chat = uos::openai_schema::CHAT_COMPLETIONS_REQUEST_KEYS; const auto& responses = uos::openai_schema::RESPONSES_REQUEST_KEYS;
  EXPECT(std::find(chat.begin(), chat.end(), "cache_control") == chat.end()); EXPECT(std::find(responses.begin(), responses.end(), "cache_control") == responses.end());
  models::setOpenRouterModelsFetchForTest(rt::Fetch([](rt::RequestInfo, rt::RequestInit) {
    return rt::Response::json(json::object{{"data", json::array{json::object{{"id", CLAUDE_MODEL}}, json::object{{"id", "vendor/alpha"}}}}});
  }));
  const json::object valid{{"model", CLAUDE_MODEL}, {"cache_control", json::object{{"type", "ephemeral"}}}};
  auto allowing = controls::openRouterPromptCacheRequestExtensions(valid); const auto allowed = co_await std::move(allowing);
  EXPECT(allowed); EXPECT_EQ(allowed->size(), 1U); EXPECT(allowed->has("cache_control"));
  for (const auto& name : std::vector<std::string>{"vendor/alpha", "gpt-5-fixture", "claude-opus-5.5", "~anthropic/claude-unlisted", ""}) {
    auto body = valid; body["model"] = name; auto refusing = controls::openRouterPromptCacheRequestExtensions(body); const auto result = co_await std::move(refusing); EXPECT(!result);
  }
  auto missing = valid; rt::objectDelete(missing, "cache_control"); auto absent = controls::openRouterPromptCacheRequestExtensions(missing); const auto noExtension = co_await std::move(absent); EXPECT(!noExtension);
  const std::vector<std::string> codexOnly{"codex"}; auto saving = selection::storeProviderSelection(isolation.kv, codexOnly); const auto selected = co_await std::move(saving); EXPECT(selected);
  auto disabled = controls::openRouterPromptCacheRequestExtensions(valid); const auto noProvider = co_await std::move(disabled); EXPECT(!noProvider);
}

ASYNC_TEST("Claude cache control validation rejects every malformed field with exact OpenAI error parameter") {
  const rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w05c2"}}};
  const std::vector<std::pair<json::value, std::pair<std::string, std::string>>> cases{
    {nullptr, {"cache_control", "cache_control must be an object"}}, {json::array{}, {"cache_control", "cache_control must be an object"}},
    {json::object{}, {"cache_control.type", "cache_control.type must be ephemeral"}}, {json::object{{"type", "permanent"}}, {"cache_control.type", "cache_control.type must be ephemeral"}},
    {json::object{{"type", "ephemeral"}, {"ttl", "24h"}}, {"cache_control.ttl", "cache_control.ttl must be 5m or 1h"}},
    {json::object{{"type", "ephemeral"}, {"ttl", nullptr}}, {"cache_control.ttl", "cache_control.ttl must be 5m or 1h"}},
    {json::object{{"type", "ephemeral"}, {"unknown", true}}, {"cache_control.unknown", "Unknown cache control field: unknown"}}};
  for (const auto& [control, expected] : cases) {
    const json::object body{{"cache_control", control}}; const auto response = controls::validateOpenRouterPromptCacheRequestControl(body); EXPECT(response); EXPECT_EQ(response->status(), 400);
    auto reading = response->json(); const auto payload = co_await std::move(reading); const auto* error = rt::get(payload, "error");
    EXPECT_EQ(rt::getString(rt::get(error, "param")), std::optional<std::string_view>(expected.first)); EXPECT_EQ(rt::getString(rt::get(error, "message")), std::optional<std::string_view>(expected.second));
    EXPECT_EQ(rt::getString(rt::get(error, "type")), std::optional<std::string_view>("invalid_request_error"));
  }
  const json::object missing{}; const auto response = controls::validateOpenRouterPromptCacheRequestControl(missing); EXPECT(response); EXPECT_EQ(response->status(), 400);
}

TEST("Claude cache control validation accepts the native ephemeral policies and follows JavaScript key order") {
  for (const auto& control : std::vector<json::value>{json::object{{"type", "ephemeral"}}, json::object{{"type", "ephemeral"}, {"ttl", "5m"}}, json::object{{"type", "ephemeral"}, {"ttl", "1h"}}, json::object{{"", true}, {"unknown", true}, {"type", "ephemeral"}}}) {
    const json::object body{{"cache_control", control}}; EXPECT(!controls::validateOpenRouterPromptCacheRequestControl(body));
  }
  EXPECT_EQ(controls::OPENROUTER_CLAUDE_REQUEST_EXTENSION_KEYS.size(), 1U); EXPECT(controls::OPENROUTER_CLAUDE_REQUEST_EXTENSION_KEYS.has("cache_control"));
}
