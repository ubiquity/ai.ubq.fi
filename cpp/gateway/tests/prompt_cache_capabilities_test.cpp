// Port of tests/prompt-cache-capabilities.test.ts.
#include "uos/models/codex_models.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::models::codex_models;

namespace {
  json::value parse(std::string_view text) { return rt::jsonParse(text); }

  const std::string PROMPT_CACHE_EVIDENCE = R"({
    "version": 1,
    "providers": [
      {
        "id": "codex_chatgpt",
        "controls": {
          "key": true,
          "implicit": true,
          "explicit_breakpoints": true,
          "modes": ["implicit", "explicit"],
          "ttls": ["30m"],
          "legacy_retentions": ["24h"],
          "breakpoint_block_types": {
            "responses": ["input_text", "input_image", "input_file"],
            "chat_completions": ["text", "image_url", "file"]
          },
          "expected_usage_fields": ["cached_tokens", "cache_write_tokens"],
          "source": "catalog",
          "verified_at_ms": 1000
        }
      },
      {
        "id": "metered",
        "controls": {
          "key": false,
          "source": "inferred",
          "verified_at_ms": 1001
        },
        "scope": {
          "probe_profile": "responses_implicit_input_text_keyed_cycle_isolated_v5",
          "account_slots": "unknown",
          "token_refresh": "unknown",
          "conversation_id": "independent",
          "effective_model": "gpt-cache-fixture",
          "reproducible_cycles": 3,
          "source": "live_probe",
          "verified_at_ms": 1002
        }
      }
    ]
  })";

  const json::value promptCacheEvidence = parse(PROMPT_CACHE_EVIDENCE);

  const json::value* modelMember(const CodexModelsSnapshot& snapshot, std::size_t index, std::string_view key) {
    const json::value* model = rt::get(snapshot.at("models").as_array(), index);
    return model ? rt::get(*model, key) : nullptr;
  }

  bool isNull(const std::optional<PromptCacheCapabilities>& value) { return !value.has_value(); }

  // { ...base, key: value }
  json::value with(const json::value& base, std::string_view key, json::value value) {
    json::object copy = rt::objectSpread(base.as_object());
    rt::objectDefine(copy, key, std::move(value));
    return copy;
  }

  // { ...base, key: undefined }: a json::value has no undefined, so the member is absent.
  json::value without(const json::value& base, std::string_view key) {
    json::object copy = rt::objectSpread(base.as_object());
    rt::objectDelete(copy, key);
    return copy;
  }

  // { version: 1, providers: [{ id: "codex_chatgpt", scope }] }
  json::value codexScopeCapabilities(const json::value& scope) {
    json::object provider;
    provider["id"] = "codex_chatgpt";
    provider["scope"] = scope;
    json::object capabilities;
    capabilities["version"] = 1;
    capabilities["providers"] = json::array{provider};
    return capabilities;
  }
}

TEST("Codex model normalization retains only versioned prompt-cache metadata") {
  const json::value payload = parse(R"({
    "source": "catalog-fixture",
    "updated_at_ms": 1100,
    "models": [
      { "slug": "gpt-cache-fixture", "prompt_cache": )" + PROMPT_CACHE_EVIDENCE + R"( },
      { "slug": "gpt-cache-unsupported", "prompt_cache": false },
      {
        "slug": "gpt-cache-invalid",
        "prompt_cache": {
          "version": 1,
          "providers": [
            {
              "id": "codex_chatgpt",
              "controls": { "source": "catalog", "verified_at_ms": 1003, "modes": ["keyed"] }
            }
          ]
        }
      }
    ]
  })");
  const std::optional<CodexModelsSnapshot> snapshot = normalizeCodexModelsPayload(payload);

  ASSERT(snapshot.has_value());
  const json::value* first = modelMember(*snapshot, 0, "prompt_cache");
  ASSERT(first != nullptr);
  EXPECT(rt::deepEqual(*first, promptCacheEvidence));
  const json::value* second = modelMember(*snapshot, 1, "prompt_cache");
  EXPECT(second != nullptr && second->is_bool() && !second->get_bool());
  EXPECT_EQ(rt::hasOwn(snapshot->at("models").as_array().at(2).as_object(), "prompt_cache"), false);
}

TEST("prompt-cache metadata rejects unknown shape and duplicate provider identities") {
  EXPECT(isNull(normalizePromptCacheCapabilities(parse(R"({ "version": 2, "providers": [] })"))));
  EXPECT(isNull(normalizePromptCacheCapabilities(parse(R"({ "version": 1, "providers": [] })"))));
  EXPECT(isNull(normalizePromptCacheCapabilities(parse(R"({
    "version": 1,
    "providers": [{ "id": "codex_chatgpt" }, { "id": "codex_chatgpt" }]
  })"))));
  EXPECT(isNull(normalizePromptCacheCapabilities(parse(R"({
    "version": 1,
    "providers": [{ "id": "codex_chatgpt", "unknown": true }]
  })"))));
}

TEST("prompt-cache metadata exposes only gateway-supported cache controls") {
  const std::optional<PromptCacheCapabilities> normalized = normalizePromptCacheCapabilities(parse(R"({
    "version": 1,
    "providers": [
      {
        "id": "codex_chatgpt",
        "controls": {
          "ttls": ["5m", "30m", "1h"],
          "legacy_retentions": ["24h", "unsupported"],
          "breakpoint_block_types": {
            "responses": ["input_text", "input_audio"],
            "chat_completions": ["text", "input_audio", "refusal"]
          },
          "source": "catalog",
          "verified_at_ms": 1010
        }
      }
    ]
  })"));

  ASSERT(normalized.has_value());
  EXPECT(rt::deepEqual(toJson(*normalized), parse(R"({
    "version": 1,
    "providers": [
      {
        "id": "codex_chatgpt",
        "controls": {
          "ttls": ["30m"],
          "legacy_retentions": ["24h"],
          "breakpoint_block_types": {
            "responses": ["input_text"],
            "chat_completions": ["text"]
          },
          "source": "catalog",
          "verified_at_ms": 1010
        }
      }
    ]
  })")));
}

TEST("prompt-cache scope requires three reproducible cycles before publication") {
  const json::value earlyScopeScope = parse(R"({
    "probe_profile": "responses_implicit_input_text_keyed_cycle_isolated_v5",
    "account_slots": "shared",
    "token_refresh": "preserved",
    "conversation_id": "independent",
    "reproducible_cycles": 2,
    "source": "live_probe",
    "verified_at_ms": 1100
  })");
  const json::value earlyScope = codexScopeCapabilities(earlyScopeScope);
  EXPECT(isNull(normalizePromptCacheCapabilities(earlyScope)));

  const json::value earlyUnknown =
    codexScopeCapabilities(with(with(with(earlyScopeScope, "account_slots", "unknown"), "token_refresh", "unknown"), "conversation_id", "unknown"));
  EXPECT(isNull(normalizePromptCacheCapabilities(earlyUnknown)));

  const json::value verifiedScopeScope = with(earlyScopeScope, "reproducible_cycles", 3);
  const json::value verifiedScope = codexScopeCapabilities(verifiedScopeScope);
  const std::optional<PromptCacheCapabilities> verified = normalizePromptCacheCapabilities(verifiedScope);
  ASSERT(verified.has_value());
  EXPECT(rt::deepEqual(toJson(*verified), verifiedScope));

  // `probe_profile: undefined`
  const json::value missingProfile = codexScopeCapabilities(without(verifiedScopeScope, "probe_profile"));
  EXPECT(isNull(normalizePromptCacheCapabilities(missingProfile)));

  const json::value wrongProfile = codexScopeCapabilities(with(verifiedScopeScope, "probe_profile", "responses_explicit_input_text_keyed_30m"));
  EXPECT(isNull(normalizePromptCacheCapabilities(wrongProfile)));

  const json::value priorExperimentDefinition = codexScopeCapabilities(with(verifiedScopeScope, "probe_profile", "responses_implicit_input_text_keyed"));
  EXPECT(isNull(normalizePromptCacheCapabilities(priorExperimentDefinition)));
}

TEST("catalog prompt-cache merges retain same-slug provider evidence without cross-provider collapse") {
  const std::optional<CodexModelsSnapshot> previous = normalizeCodexModelsPayload(parse(R"({
    "source": "probe",
    "updated_at_ms": 1200,
    "models": [{ "slug": "gpt-cache-fixture", "prompt_cache": )" + PROMPT_CACHE_EVIDENCE + R"( }]
  })"));
  const std::optional<CodexModelsSnapshot> next = normalizeCodexModelsPayload(parse(R"({
    "source": "catalog",
    "updated_at_ms": 1300,
    "models": [
      {
        "slug": "gpt-cache-fixture",
        "prompt_cache": {
          "version": 1,
          "providers": [
            {
              "id": "codex_chatgpt",
              "controls": { "implicit": false, "source": "catalog", "verified_at_ms": 1300 }
            }
          ]
        }
      }
    ]
  })"));
  ASSERT(previous.has_value());
  ASSERT(next.has_value());

  const CodexModelsSnapshot merged = mergeCodexModelPromptCacheCapabilities(*next, previous);
  const json::value* promptCache = modelMember(merged, 0, "prompt_cache");
  ASSERT(promptCache != nullptr);
  json::object expected = parse(R"({
    "version": 1,
    "providers": [
      {
        "id": "codex_chatgpt",
        "controls": { "implicit": false, "source": "catalog", "verified_at_ms": 1300 }
      }
    ]
  })").as_object();
  expected.at("providers").as_array().push_back(promptCacheEvidence.at("providers").as_array().at(1));
  EXPECT(rt::deepEqual(*promptCache, expected));
}
