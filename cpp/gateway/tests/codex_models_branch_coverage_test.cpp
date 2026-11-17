// Port of tests/codex-models-branch-coverage.test.ts.
//
// JavaScript `undefined` members (`{ ...x, key: undefined }`) are absent members here: a
// json::value has no undefined. TS identity checks (`assert.equal` on objects) compare
// values: the C++ functions return copies.
#include "uos/models/codex_models.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::models::codex_models;

namespace {
  json::value parse(std::string_view text) { return rt::jsonParse(text); }

  json::value str(std::string_view text) { return json::value(json::string(text.data(), text.size())); }

  json::value arrayOf(std::vector<json::value> items) {
    json::array out;
    for (json::value& item : items) out.push_back(std::move(item));
    return out;
  }

  // { ...base, key: value }
  json::value with(const json::value& base, std::string_view key, json::value value) {
    json::object copy = rt::objectSpread(base.as_object());
    rt::objectDefine(copy, key, std::move(value));
    return copy;
  }

  // { ...base, key: undefined } and `delete base.key`: the member is absent.
  json::value without(const json::value& base, std::string_view key) {
    json::object copy = rt::objectSpread(base.as_object());
    rt::objectDelete(copy, key);
    return copy;
  }

  json::value validControls() {
    return parse(R"({
      "key": true,
      "implicit": true,
      "explicit_breakpoints": true,
      "modes": ["implicit"],
      "ttls": ["30m"],
      "legacy_retentions": ["24h"],
      "breakpoint_block_types": { "responses": ["input_text"], "chat_completions": ["text"] },
      "expected_usage_fields": ["cached_tokens", "cache_write_tokens"],
      "source": "catalog",
      "verified_at_ms": 1000
    })");
  }

  json::value validScope() {
    return parse(R"({"probe_profile": ")" + std::string(PROMPT_CACHE_SCOPE_PROBE_PROFILE) + R"(",
      "account_slots": "account_scoped",
      "token_refresh": "preserved",
      "conversation_id": "scoped",
      "effective_model": "gpt-5-codex",
      "reproducible_cycles": 3,
      "source": "live_probe",
      "verified_at_ms": 2000
    })");
  }

  json::value providerWithControls(json::value controls = validControls(), std::string_view id = CODEX_CHATGPT_PROMPT_CACHE_PROVIDER) {
    json::object provider;
    provider["id"] = str(id);
    provider["controls"] = std::move(controls);
    return provider;
  }

  json::value capabilitiesWith(json::value provider) {
    json::object capabilities;
    capabilities["version"] = 1;
    capabilities["providers"] = arrayOf({std::move(provider)});
    return capabilities;
  }

  json::value capabilitiesWithControls(json::value controls = validControls(), std::string_view id = CODEX_CHATGPT_PROMPT_CACHE_PROVIDER) {
    return capabilitiesWith(providerWithControls(std::move(controls), id));
  }

  json::value capabilitiesWithScope(json::value scope = validScope()) {
    json::object provider;
    provider["id"] = str(CODEX_CHATGPT_PROMPT_CACHE_PROVIDER);
    provider["controls"] = validControls();
    provider["scope"] = std::move(scope);
    return capabilitiesWith(provider);
  }

  /**
   * Snapshot fixture. `models` stays `unknown` on purpose: several cases below
   * store malformed entries to drive the runtime shape guards a stored record can
   * still contain.
   */
  CodexModelsSnapshot snapshotOf(json::value models) {
    CodexModelsSnapshot snapshot;
    snapshot["models"] = std::move(models);
    snapshot["source"] = "codex_cli";
    snapshot["updated_at_ms"] = 1;
    return snapshot;
  }

  // { slug, prompt_cache }
  json::value modelWithCache(std::string_view slug, json::value promptCache) {
    json::object model;
    model["slug"] = str(slug);
    model["prompt_cache"] = std::move(promptCache);
    return model;
  }

  bool isNull(const std::optional<PromptCacheCapabilities>& value) { return !value.has_value(); }

  bool isFalse(const std::optional<PromptCacheCapabilities>& value) { return value && std::holds_alternative<PromptCacheCapabilitiesUnsupported>(*value); }

  // `value !== null && value !== false`
  const PromptCacheCapabilitiesV1* versioned(const std::optional<PromptCacheCapabilities>& value) {
    return value ? std::get_if<PromptCacheCapabilitiesV1>(&*value) : nullptr;
  }

  const RuntimePromptCacheCapabilitiesV1* versioned(const std::optional<RuntimePromptCacheCapabilities>& value) {
    return value ? std::get_if<RuntimePromptCacheCapabilitiesV1>(&*value) : nullptr;
  }

  const json::object& modelAt(const CodexModelsSnapshot& snapshot, std::size_t index) { return snapshot.at("models").as_array().at(index).as_object(); }

  std::vector<std::string> providerIds(const std::vector<PromptCacheProvider>& providers) {
    std::vector<std::string> ids;
    for (const PromptCacheProvider& provider : providers) ids.push_back(provider.id);
    return ids;
  }

  /** Capabilities accepted today, so any mutation in a case below is the only change. */
  void assertValidBaseline() {
    const std::optional<PromptCacheCapabilities> normalized = normalizePromptCacheCapabilities(capabilitiesWithScope());
    ASSERT(versioned(normalized) != nullptr);
    ASSERT_EQ(versioned(normalized)->providers.size(), std::size_t{1});
  }
}

// ---------------------------------------------------------------------------
// normalizePromptCacheCapabilities: envelope and provider guards
// ---------------------------------------------------------------------------

TEST("prompt cache capabilities reject every malformed envelope") {
  assertValidBaseline();
  const std::vector<json::value> cases = {
    nullptr,
    "providers",
    parse(R"([{ "version": 1, "providers": [] }])"),
    with(capabilitiesWith(providerWithControls()), "version", 2),
    parse(R"({ "version": 1 })"),
    parse(R"({ "version": 1, "providers": [] })"),
    parse(R"({ "version": 1, "providers": "none" })"),
    with(capabilitiesWith(providerWithControls()), "extra", true),
  };

  for (const json::value& value : cases) {
    EXPECT(isNull(normalizePromptCacheCapabilities(value)));
  }
  EXPECT(isFalse(normalizePromptCacheCapabilities(json::value(false))));
}

TEST("prompt cache capabilities reject an unidentifiable or duplicate provider") {
  EXPECT(isNull(normalizePromptCacheCapabilities(capabilitiesWith(with(parse(R"({ "id": "   " })"), "controls", validControls())))));
  EXPECT(isNull(normalizePromptCacheCapabilities(capabilitiesWith(with(parse(R"({ "id": 7 })"), "controls", validControls())))));
  EXPECT(isNull(normalizePromptCacheCapabilities(capabilitiesWith(with(with(parse(R"({ "id": "a b" })"), "controls", validControls()), "scope", json::object())))));
  EXPECT(isNull(normalizePromptCacheCapabilities(parse(R"({ "version": 1, "providers": [{ "id": "dup" }, { "id": "dup" }] })"))));
  const std::optional<PromptCacheCapabilities> spaced = normalizePromptCacheCapabilities(parse(R"({ "version": 1, "providers": [{ "id": "  spaced  " }] })"));
  ASSERT(spaced.has_value());
  EXPECT(rt::deepEqual(toJson(*spaced), parse(R"({
    "version": 1,
    "providers": [{ "id": "spaced" }]
  })")));
}

TEST("a declared controls block that is not a plain object invalidates the provider") {
  for (const json::value& controls : std::vector<json::value>{"catalog", 7, nullptr, arrayOf({validControls()})}) {
    EXPECT(isNull(normalizePromptCacheCapabilities(capabilitiesWith(providerWithControls(controls, "codex_chatgpt")))));
  }
  EXPECT(isNull(normalizePromptCacheCapabilities(capabilitiesWith(providerWithControls(with(validControls(), "extra", 1), "codex_chatgpt")))));
}

TEST("controls require a known source, a safe timestamp and boolean switches") {
  const std::vector<json::value> invalid = {
    with(validControls(), "source", "bogus"),
    without(validControls(), "source"),
    with(validControls(), "verified_at_ms", -1),
    with(validControls(), "verified_at_ms", 1000.5),
    with(validControls(), "verified_at_ms", "1000"),
    without(validControls(), "verified_at_ms"),
    with(validControls(), "key", "true"),
    with(validControls(), "implicit", 1),
    with(validControls(), "explicit_breakpoints", nullptr),
  };

  for (const json::value& controls : invalid) {
    EXPECT(isNull(normalizePromptCacheCapabilities(capabilitiesWith(providerWithControls(controls, "codex_chatgpt")))));
  }
  const std::optional<PromptCacheCapabilities> accepted = normalizePromptCacheCapabilities(
    capabilitiesWith(providerWithControls(with(with(with(validControls(), "key", false), "implicit", false), "explicit_breakpoints", false), "codex_chatgpt")));
  ASSERT(versioned(accepted) != nullptr);
  const std::optional<PromptCacheControls>& controls = versioned(accepted)->providers.at(0).controls;
  ASSERT(controls.has_value());
  EXPECT_EQ(controls->key, std::optional<bool>(false));
  EXPECT_EQ(controls->implicit, std::optional<bool>(false));
  EXPECT_EQ(controls->explicit_breakpoints, std::optional<bool>(false));
}

TEST("an optional list control with a non-array or blank entry invalidates the whole record") {
  const std::vector<json::value> invalid = {
    with(validControls(), "modes", "implicit"),
    with(validControls(), "modes", parse(R"(["implicit", 5])")),
    with(validControls(), "modes", parse(R"(["implicit", "bogus"])")),
    with(validControls(), "ttls", "30m"),
    with(validControls(), "ttls", parse(R"(["30m", ""])")),
    with(validControls(), "ttls", parse(R"(["30m", "   "])")),
    with(validControls(), "legacy_retentions", "24h"),
    with(validControls(), "expected_usage_fields", "cached_tokens"),
    with(validControls(), "expected_usage_fields", parse(R"(["cached_tokens", "bogus"])")),
    with(validControls(), "breakpoint_block_types", "input_text"),
    with(validControls(), "breakpoint_block_types", parse(R"({ "responses": "input_text" })")),
    with(validControls(), "breakpoint_block_types", parse(R"({ "responses": ["input_text"], "chat_completions": "text" })")),
    with(validControls(), "breakpoint_block_types", parse(R"({ "responses": ["input_text"], "unknown": ["input_text"] })")),
  };

  for (const json::value& controls : invalid) {
    EXPECT(isNull(normalizePromptCacheCapabilities(capabilitiesWith(providerWithControls(controls, "codex_chatgpt")))));
  }
}

TEST("gateway-unsupported list values are dropped or invalidate an exhaustive declaration") {
  const std::optional<PromptCacheCapabilities> droppedTtls =
    normalizePromptCacheCapabilities(capabilitiesWith(providerWithControls(with(validControls(), "ttls", parse(R"(["30m", "2h"])")), "codex_chatgpt")));
  ASSERT(versioned(droppedTtls) != nullptr);
  EXPECT_EQ(versioned(droppedTtls)->providers.at(0).controls->ttls, std::optional<std::vector<std::string>>(std::vector<std::string>{"30m"}));

  const std::optional<PromptCacheCapabilities> droppedRetentions = normalizePromptCacheCapabilities(
    capabilitiesWith(providerWithControls(with(validControls(), "legacy_retentions", parse(R"(["24h", "forever"])")), "codex_chatgpt")));
  ASSERT(versioned(droppedRetentions) != nullptr);
  EXPECT_EQ(versioned(droppedRetentions)->providers.at(0).controls->legacy_retentions, std::optional<std::vector<std::string>>(std::vector<std::string>{"24h"}));

  const std::optional<PromptCacheCapabilities> droppedAll =
    normalizePromptCacheCapabilities(capabilitiesWith(providerWithControls(with(validControls(), "ttls", parse(R"(["2h"])")), "codex_chatgpt")));
  ASSERT(versioned(droppedAll) != nullptr);
  EXPECT_EQ(rt::hasOwn(toJson(*versioned(droppedAll)->providers.at(0).controls).as_object(), "ttls"), false);

  const std::optional<PromptCacheCapabilities> deduplicated = normalizePromptCacheCapabilities(
    capabilitiesWith(providerWithControls(with(validControls(), "modes", parse(R"(["implicit", "implicit", "explicit"])")), "codex_chatgpt")));
  ASSERT(versioned(deduplicated) != nullptr);
  EXPECT_EQ(versioned(deduplicated)->providers.at(0).controls->modes, std::optional<std::vector<std::string>>(std::vector<std::string>{"implicit", "explicit"}));

  // Every advertised block type is filtered away, so nothing supported is left to publish.
  for (const json::value& value : std::vector<json::value>{parse(R"({ "responses": ["bogus"] })"), parse(R"({ "chat_completions": ["bogus"] })"),
                                                           parse(R"({ "responses": [], "chat_completions": [] })")}) {
    EXPECT(isNull(normalizePromptCacheCapabilities(capabilitiesWith(providerWithControls(with(validControls(), "breakpoint_block_types", value), "codex_chatgpt")))));
  }

  const std::optional<PromptCacheCapabilities> partial = normalizePromptCacheCapabilities(capabilitiesWith(
    providerWithControls(with(validControls(), "breakpoint_block_types", parse(R"({ "responses": ["input_text", "bogus"] })")), "codex_chatgpt")));
  ASSERT(versioned(partial) != nullptr);
  EXPECT(rt::deepEqual(json::value_from(*versioned(partial)->providers.at(0).controls->breakpoint_block_types), parse(R"({ "responses": ["input_text"] })")));

  const std::optional<PromptCacheCapabilities> chatOnly = normalizePromptCacheCapabilities(capabilitiesWith(
    providerWithControls(with(validControls(), "breakpoint_block_types", parse(R"({ "chat_completions": ["text", "residual"] })")), "codex_chatgpt")));
  ASSERT(versioned(chatOnly) != nullptr);
  EXPECT(rt::deepEqual(json::value_from(*versioned(chatOnly)->providers.at(0).controls->breakpoint_block_types), parse(R"({ "chat_completions": ["text"] })")));
}

// ---------------------------------------------------------------------------
// normalizePromptCacheCapabilities: scope guards
// ---------------------------------------------------------------------------

TEST("scope evidence rejects unknown members, profiles and classifications") {
  const std::vector<json::value> invalid = {
    with(validScope(), "probe_profile", "responses_implicit_v0"),
    with(validScope(), "account_slots", "private"),
    with(validScope(), "token_refresh", "rotated"),
    with(validScope(), "conversation_id", "global"),
    with(validScope(), "source", "catalog"),
    with(validScope(), "reproducible_cycles", 2),
    with(validScope(), "reproducible_cycles", "3"),
    without(validScope(), "verified_at_ms"),
    with(validScope(), "effective_model", "   "),
    with(validScope(), "extra", true),
  };

  for (const json::value& scope : invalid) {
    EXPECT(isNull(normalizePromptCacheCapabilities(capabilitiesWithScope(scope))));
  }
  EXPECT(isNull(normalizePromptCacheCapabilities(capabilitiesWith(parse(R"({ "id": "codex_chatgpt", "scope": "scope" })")))));
  EXPECT(isNull(normalizePromptCacheCapabilities(capabilitiesWith(parse(R"({ "id": "codex_chatgpt", "scope": null })")))));
}

TEST("a scope without an effective model keeps the other verified fields") {
  const json::value scope = without(validScope(), "effective_model");
  const std::optional<PromptCacheCapabilities> normalized = normalizePromptCacheCapabilities(capabilitiesWithScope(scope));

  ASSERT(versioned(normalized) != nullptr);
  ASSERT(versioned(normalized)->providers.at(0).scope.has_value());
  EXPECT(rt::deepEqual(toJson(*versioned(normalized)->providers.at(0).scope), parse(R"({"probe_profile": ")" + std::string(PROMPT_CACHE_SCOPE_PROBE_PROFILE) + R"(",
    "account_slots": "account_scoped",
    "token_refresh": "preserved",
    "conversation_id": "scoped",
    "reproducible_cycles": 3,
    "source": "live_probe",
    "verified_at_ms": 2000
  })")));

  const std::optional<PromptCacheCapabilities> trailing = normalizePromptCacheCapabilities(capabilitiesWithScope(with(validScope(), "effective_model", "  gpt-5-codex  ")));
  ASSERT(versioned(trailing) != nullptr);
  ASSERT(versioned(trailing)->providers.at(0).scope.has_value());
  EXPECT_EQ(versioned(trailing)->providers.at(0).scope->effective_model, std::optional<std::string>("gpt-5-codex"));
}

// ---------------------------------------------------------------------------
// compactPromptCacheCapabilities
// ---------------------------------------------------------------------------

TEST("compaction keeps controls providers and drops probe-only scope") {
  const std::optional<RuntimePromptCacheCapabilities> unsupported = compactPromptCacheCapabilities(json::value(false));
  EXPECT(unsupported.has_value() && std::holds_alternative<RuntimePromptCacheCapabilitiesUnsupported>(*unsupported));
  EXPECT(!compactPromptCacheCapabilities(json::value(nullptr)).has_value());
  EXPECT(!compactPromptCacheCapabilities(json::value("bogus")).has_value());
  EXPECT(!compactPromptCacheCapabilities(capabilitiesWith(with(parse(R"({ "id": "noise" })"), "scope", validScope()))).has_value());

  const std::optional<RuntimePromptCacheCapabilities> compacted = compactPromptCacheCapabilities(capabilitiesWithScope());
  ASSERT(versioned(compacted) != nullptr);
  std::vector<std::string> ids;
  for (const RuntimePromptCacheCapabilitiesV1::Provider& provider : versioned(compacted)->providers) ids.push_back(provider.id);
  EXPECT_EQ(ids, std::vector<std::string>{std::string(CODEX_CHATGPT_PROMPT_CACHE_PROVIDER)});
  EXPECT_EQ(rt::hasOwn(toJson(*compacted).at("providers").as_array().at(0).as_object(), "scope"), false);
  const json::value controls = toJson(versioned(compacted)->providers.at(0).controls);
  EXPECT(rt::deepEqual(controls, controls));
}

// ---------------------------------------------------------------------------
// Snapshot lookup helpers
// ---------------------------------------------------------------------------

TEST("exact-slug lookup resolves every id spelling and rejects ambiguity") {
  const json::value bySlugModel = parse(R"({ "slug": "gpt-5-codex" })");
  const json::value byIdModel = parse(R"({ "id": "gpt-5-codex" })");
  const json::value byModelModel = parse(R"({ "model": "gpt-5-codex" })");
  const json::value byNameModel = parse(R"({ "name": "  gpt-5-codex  " })");
  const std::vector<std::pair<CodexModelsSnapshot, json::value>> fixtures = {
    {snapshotOf(arrayOf({bySlugModel})), bySlugModel},
    {snapshotOf(arrayOf({byIdModel})), byIdModel},
    {snapshotOf(arrayOf({byModelModel})), byModelModel},
    {snapshotOf(arrayOf({byNameModel})), byNameModel},
  };

  for (const auto& [snapshot, model] : fixtures) {
    const std::optional<json::object> exact = getUniqueCodexModelBySlug(snapshot, "gpt-5-codex");
    EXPECT(exact.has_value() && rt::deepEqual(*exact, model));
    const std::optional<json::object> padded = getUniqueCodexModelBySlug(snapshot, "  gpt-5-codex  ");
    EXPECT(padded.has_value() && rt::deepEqual(*padded, model));
  }

  EXPECT(!getUniqueCodexModelBySlug(snapshotOf(parse(R"([{ "slug": "gpt-5-codex" }, { "id": "gpt-5-codex" }])")), "gpt-5-codex").has_value());
  EXPECT(!getUniqueCodexModelBySlug(snapshotOf(parse(R"([{ "slug": "gpt-5-codex" }])")), "   ").has_value());
  EXPECT(!getUniqueCodexModelBySlug(snapshotOf(json::array()), "gpt-5-codex").has_value());
  EXPECT(!getUniqueCodexModelBySlug(snapshotOf(parse(R"(["not-a-record"])")), "gpt-5-codex").has_value());
  EXPECT(!getUniqueCodexModelBySlug(snapshotOf("none"), "gpt-5-codex").has_value());
  EXPECT(!getUniqueCodexModelBySlug(snapshotOf(parse(R"([{ "slug": "   " }])")), "gpt-5-codex").has_value());
}

TEST("provider lookup reports absence instead of guessing") {
  const CodexModelsSnapshot withCache = snapshotOf(arrayOf({modelWithCache("gpt-5-codex", capabilitiesWithScope())}));
  const std::optional<PromptCacheProvider> provider = getCodexModelPromptCacheProvider(withCache, "gpt-5-codex", CODEX_CHATGPT_PROMPT_CACHE_PROVIDER);
  json::object expected;
  expected["id"] = str(CODEX_CHATGPT_PROMPT_CACHE_PROVIDER);
  expected["controls"] = validControls();
  expected["scope"] = validScope();
  ASSERT(provider.has_value());
  EXPECT(rt::deepEqual(toJson(*provider), expected));

  EXPECT(!getCodexModelPromptCacheProvider(withCache, "missing", CODEX_CHATGPT_PROMPT_CACHE_PROVIDER).has_value());
  EXPECT(!getCodexModelPromptCacheProvider(withCache, "gpt-5-codex", "other_provider").has_value());
  EXPECT(!getCodexModelPromptCacheProvider(snapshotOf(parse(R"([{ "slug": "gpt-5-codex" }])")), "gpt-5-codex", CODEX_CHATGPT_PROMPT_CACHE_PROVIDER).has_value());
  EXPECT(!getCodexModelPromptCacheProvider(snapshotOf(parse(R"([{ "slug": "gpt-5-codex", "prompt_cache": false }])")), "gpt-5-codex",
                                           CODEX_CHATGPT_PROMPT_CACHE_PROVIDER)
            .has_value());
  EXPECT(!getCodexModelPromptCacheProvider(snapshotOf(parse(R"([{ "slug": "gpt-5-codex", "prompt_cache": { "version": 1, "providers": [] } }])")), "gpt-5-codex", "x")
            .has_value());
}

TEST("scope-experiment eligibility requires the exact plain-key control set") {
  const CodexModelsSnapshot eligible = snapshotOf(arrayOf({modelWithCache("gpt-5-codex", capabilitiesWithControls())}));
  EXPECT_EQ(isCodexModelPromptCacheScopeExperimentEligible(eligible, "gpt-5-codex"), true);

  const std::vector<json::value> cases = {
    with(validControls(), "key", false),
    with(validControls(), "implicit", false),
    with(validControls(), "expected_usage_fields", parse(R"(["cached_tokens"])")),
    with(validControls(), "expected_usage_fields", json::array()),
    // `expected_usage_fields: undefined`. The TS keeps an own member holding undefined,
    // which invalidates the whole controls record; here the member is absent, which
    // leaves valid controls without the field. Either way the model is not eligible.
    without(validControls(), "expected_usage_fields"),
  };
  for (const json::value& controls : cases) {
    const CodexModelsSnapshot snapshot = snapshotOf(arrayOf({modelWithCache("gpt-5-codex", capabilitiesWithControls(controls))}));
    EXPECT_EQ(isCodexModelPromptCacheScopeExperimentEligible(snapshot, "gpt-5-codex"), false);
  }

  EXPECT_EQ(isCodexModelPromptCacheScopeExperimentEligible(eligible, "unknown"), false);
  EXPECT_EQ(isCodexModelPromptCacheScopeExperimentEligible(snapshotOf(parse(R"([{ "slug": "gpt-5-codex" }])")), "gpt-5-codex"), false);
  // Explicit-only option modes describe the other wire shape and cannot disqualify this one.
  const CodexModelsSnapshot explicitOnly = snapshotOf(arrayOf({modelWithCache(
    "gpt-5-codex", capabilitiesWithControls(with(with(with(validControls(), "modes", parse(R"(["explicit"])")), "implicit", true), "key", true)))}));
  EXPECT_EQ(isCodexModelPromptCacheScopeExperimentEligible(explicitOnly, "gpt-5-codex"), true);
}

TEST("only a complete reproducible scope counts as concrete") {
  const json::value scope = capabilitiesWithScope();
  const std::optional<PromptCacheCapabilities> normalized = normalizePromptCacheCapabilities(scope);
  ASSERT(versioned(normalized) != nullptr);
  const std::optional<PromptCacheScope> hosted = versioned(normalized)->providers.at(0).scope;
  ASSERT(hosted.has_value());
  const auto changed = [&hosted](auto mutate) {
    PromptCacheScope copy = *hosted;
    mutate(copy);
    return copy;
  };
  EXPECT_EQ(isConcretePromptCacheScope(*hosted), true);
  EXPECT_EQ(isConcretePromptCacheScope(changed([](PromptCacheScope& s) { s.reproducible_cycles = 4; }), 4), true);
  EXPECT_EQ(isConcretePromptCacheScope(changed([](PromptCacheScope& s) { s.reproducible_cycles = 4; })), false);
  EXPECT_EQ(isConcretePromptCacheScope(changed([](PromptCacheScope& s) { s.account_slots = "unknown"; })), false);
  EXPECT_EQ(isConcretePromptCacheScope(changed([](PromptCacheScope& s) { s.token_refresh = "unknown"; })), false);
  EXPECT_EQ(isConcretePromptCacheScope(changed([](PromptCacheScope& s) { s.conversation_id = "unknown"; })), false);
  EXPECT_EQ(isConcretePromptCacheScope(changed([](PromptCacheScope& s) { s.effective_model = std::nullopt; })), false);
  EXPECT_EQ(isConcretePromptCacheScope(changed([](PromptCacheScope& s) { s.effective_model = "   "; })), false);
  EXPECT_EQ(isConcretePromptCacheScope(changed([](PromptCacheScope& s) { s.probe_profile = "other"; })), false);
  EXPECT_EQ(isConcretePromptCacheScope(changed([](PromptCacheScope& s) { s.source = "catalog"; })), false);
  EXPECT_EQ(isConcretePromptCacheScope(changed([](PromptCacheScope& s) { s.source = "live_probe"; })), true);
}

// ---------------------------------------------------------------------------
// withCodexModelPromptCacheScope
// ---------------------------------------------------------------------------

TEST("scope attachment requires a unique model, a present provider and a valid scope") {
  const json::value model = modelWithCache("gpt-5-codex", capabilitiesWithControls());
  const json::value other = modelWithCache("other", capabilitiesWithControls());
  const CodexModelsSnapshot snapshot = snapshotOf(arrayOf({model, other}));
  const CodexModelsSnapshot pristine = snapshot;
  const std::optional<CodexModelsSnapshot> attached =
    withCodexModelPromptCacheScope(snapshot, "gpt-5-codex", CODEX_CHATGPT_PROMPT_CACHE_PROVIDER, json::value_to<PromptCacheScope>(validScope()));

  ASSERT(attached.has_value());
  // "the source snapshot must not be mutated" (the TS compares the source member with a
  // fresh literal by identity; the C++ checks that the source still holds its value).
  EXPECT(rt::deepEqual(snapshot, pristine));
  EXPECT(rt::deepEqual(modelAt(snapshot, 0).at("prompt_cache"), capabilitiesWithControls()));
  const std::optional<PromptCacheCapabilities> attachedCache = normalizePromptCacheCapabilities(rt::get(modelAt(*attached, 0), "prompt_cache"));
  ASSERT(versioned(attachedCache) != nullptr);
  ASSERT(versioned(attachedCache)->providers.at(0).scope.has_value());
  EXPECT_EQ(versioned(attachedCache)->providers.at(0).scope->effective_model, std::optional<std::string>("gpt-5-codex"));
  EXPECT(rt::deepEqual(modelAt(*attached, 1).at("prompt_cache"), other.at("prompt_cache")));
  EXPECT(rt::deepEqual(modelAt(*attached, 1), other));  // unrelated providers stay catalog-owned

  EXPECT(!withCodexModelPromptCacheScope(snapshot, "missing", CODEX_CHATGPT_PROMPT_CACHE_PROVIDER, json::value_to<PromptCacheScope>(validScope())).has_value());
  EXPECT(!withCodexModelPromptCacheScope(snapshotOf(parse(R"([{ "slug": "gpt-5-codex" }])")), "gpt-5-codex", CODEX_CHATGPT_PROMPT_CACHE_PROVIDER,
                                         json::value_to<PromptCacheScope>(validScope()))
            .has_value());
  EXPECT(!withCodexModelPromptCacheScope(snapshotOf(parse(R"([{ "slug": "gpt-5-codex", "prompt_cache": false }])")), "gpt-5-codex",
                                         CODEX_CHATGPT_PROMPT_CACHE_PROVIDER, json::value_to<PromptCacheScope>(validScope()))
            .has_value());
  EXPECT(!withCodexModelPromptCacheScope(snapshot, "gpt-5-codex", "absent_provider", json::value_to<PromptCacheScope>(validScope())).has_value());
  EXPECT(!withCodexModelPromptCacheScope(snapshot, "gpt-5-codex", CODEX_CHATGPT_PROMPT_CACHE_PROVIDER,
                                         json::value_to<PromptCacheScope>(with(validScope(), "reproducible_cycles", 1)))
            .has_value());
}

// ---------------------------------------------------------------------------
// mergePromptCacheCapabilities
// ---------------------------------------------------------------------------

TEST("prompt cache merge precedence covers false, absent and invalid sides") {
  EXPECT(isFalse(mergePromptCacheCapabilities(capabilitiesWithScope(), json::value(false))));
  EXPECT(isNull(mergePromptCacheCapabilities(json::value(nullptr), json::value(nullptr))));
  EXPECT(isNull(mergePromptCacheCapabilities(json::value("bogus"), json::value("bogus"))));

  const json::value next = capabilitiesWithControls(validControls());
  const std::optional<PromptCacheCapabilities> normalizedNext = normalizePromptCacheCapabilities(next);
  ASSERT(versioned(normalizedNext) != nullptr);
  const std::optional<PromptCacheControls> nextControls = versioned(normalizedNext)->providers.at(0).controls;
  ASSERT(nextControls.has_value());

  const std::optional<PromptCacheCapabilities> fromNull = mergePromptCacheCapabilities(json::value(nullptr), next);
  EXPECT(fromNull.has_value() && rt::deepEqual(toJson(*fromNull), toJson(*normalizedNext)));
  const std::optional<PromptCacheCapabilities> fromFalse = mergePromptCacheCapabilities(json::value(false), next);
  EXPECT(fromFalse.has_value() && rt::deepEqual(toJson(*fromFalse), toJson(*normalizedNext)));

  // The incoming catalog carries no scope, so the live-probe scope survives.
  const std::optional<PromptCacheCapabilities> mergedWithScope = mergePromptCacheCapabilities(capabilitiesWithScope(), next);
  ASSERT(versioned(mergedWithScope) != nullptr);
  EXPECT(rt::deepEqual(toJson(*versioned(mergedWithScope)->providers.at(0).controls), toJson(*nextControls)));
  ASSERT(versioned(mergedWithScope)->providers.at(0).scope.has_value());
  EXPECT_EQ(versioned(mergedWithScope)->providers.at(0).scope->effective_model, std::optional<std::string>("gpt-5-codex"));
  EXPECT_EQ(versioned(mergedWithScope)->providers.size(), std::size_t{1});
}

TEST("prompt cache merge keeps control-only and scope-only providers from both sides") {
  const json::value previousOnly = capabilitiesWith(with(parse(R"({ "id": "retired_provider" })"), "scope", validScope()));
  const json::value nextOnly = capabilitiesWith(with(parse(R"({ "id": "fresh_provider" })"), "controls", validControls()));
  const std::optional<PromptCacheCapabilities> merged = mergePromptCacheCapabilities(previousOnly, nextOnly);

  ASSERT(versioned(merged) != nullptr);
  EXPECT_EQ(providerIds(versioned(merged)->providers), (std::vector<std::string>{"fresh_provider", "retired_provider"}));

  const json::value previousControls = capabilitiesWith(providerWithControls(with(with(validControls(), "key", true), "implicit", true), "codex_chatgpt"));
  const json::value nextControls = capabilitiesWith(providerWithControls(with(with(validControls(), "source", "live_probe"), "key", false), "codex_chatgpt"));
  const std::optional<PromptCacheCapabilities> overridden = mergePromptCacheCapabilities(previousControls, nextControls);
  ASSERT(versioned(overridden) != nullptr);
  ASSERT(versioned(overridden)->providers.at(0).controls.has_value());
  EXPECT_EQ(versioned(overridden)->providers.at(0).controls->key, std::optional<bool>(false));
  EXPECT_EQ(versioned(overridden)->providers.at(0).controls->implicit, std::optional<bool>(true));
  EXPECT_EQ(versioned(overridden)->providers.at(0).controls->source, "live_probe");

  const json::value previousScope = capabilitiesWith(with(providerWithControls(validControls(), "codex_chatgpt"), "scope", validScope()));
  const json::value nextScope = capabilitiesWith(with(parse(R"({ "id": "codex_chatgpt" })"), "scope", validScope()));
  const std::optional<PromptCacheCapabilities> scopeKept = mergePromptCacheCapabilities(previousScope, nextScope);
  ASSERT(versioned(scopeKept) != nullptr);
  ASSERT(versioned(scopeKept)->providers.at(0).controls.has_value());
  EXPECT_EQ(versioned(scopeKept)->providers.at(0).controls->source, "catalog");
  EXPECT(versioned(scopeKept)->providers.at(0).scope.has_value());
}

// ---------------------------------------------------------------------------
// mergeCodexModelPromptCacheCapabilities
// ---------------------------------------------------------------------------

TEST("model-level cache merge preserves evidence per matching slug only") {
  const CodexModelsSnapshot previous = snapshotOf(arrayOf({
    modelWithCache("gpt-5-codex", capabilitiesWithScope()),
    modelWithCache("retired", capabilitiesWithScope()),
    "not-a-record",
    parse(R"({ "slug": "   " })"),
    with(parse(R"({ "id": "by-id" })"), "prompt_cache", capabilitiesWithScope()),
  }));
  const CodexModelsSnapshot next =
    snapshotOf(parse(R"([{ "slug": "gpt-5-codex" }, { "slug": "new-model" }, { "slug": "retired" }, { "id": "by-id" }, "not-a-record"])"));

  const CodexModelsSnapshot merged = mergeCodexModelPromptCacheCapabilities(next, previous);

  EXPECT(!rt::deepEqual(merged, next));
  const std::optional<PromptCacheCapabilities> codexCache = normalizePromptCacheCapabilities(rt::get(modelAt(merged, 0), "prompt_cache"));
  ASSERT(versioned(codexCache) != nullptr);
  EXPECT(versioned(codexCache)->providers.at(0).scope.has_value());
  EXPECT_EQ(rt::hasOwn(modelAt(merged, 1), "prompt_cache"), false);
  EXPECT(rt::truthy(rt::get(modelAt(merged, 2), "prompt_cache")));
  EXPECT(rt::truthy(rt::get(modelAt(merged, 3), "prompt_cache")));
  EXPECT(rt::deepEqual(merged.at("models").as_array().at(4), json::value("not-a-record")));
}

TEST("model-level cache merge returns the same snapshot when nothing can be added") {
  const CodexModelsSnapshot next = snapshotOf(parse(R"([{ "slug": "gpt-5-codex" }])"));

  EXPECT(rt::deepEqual(mergeCodexModelPromptCacheCapabilities(next, std::nullopt), next));  // previous: null
  EXPECT(rt::deepEqual(mergeCodexModelPromptCacheCapabilities(next, std::nullopt), next));  // previous: undefined
  EXPECT(rt::deepEqual(mergeCodexModelPromptCacheCapabilities(next, snapshotOf(json::array())), next));
  EXPECT(rt::deepEqual(mergeCodexModelPromptCacheCapabilities(next, snapshotOf("none")), next));
  // The previous snapshot advertises a different model, so no evidence may travel.
  EXPECT(rt::deepEqual(mergeCodexModelPromptCacheCapabilities(next, snapshotOf(arrayOf({modelWithCache("other", capabilitiesWithScope())}))), next));
  // Cached evidence exists but the incoming entry carries its own answer.
  const CodexModelsSnapshot ownAnswer = mergeCodexModelPromptCacheCapabilities(snapshotOf(parse(R"([{ "slug": "gpt-5-codex", "prompt_cache": false }])")),
                                                                               snapshotOf(arrayOf({modelWithCache("gpt-5-codex", capabilitiesWithScope())})));
  EXPECT(rt::deepEqual(modelAt(ownAnswer, 0).at("prompt_cache"), json::value(false)));
  // A model without any slug cannot inherit evidence.
  EXPECT(rt::deepEqual(mergeCodexModelPromptCacheCapabilities(snapshotOf(parse("[{}]")), snapshotOf(parse(R"([{ "slug": "gpt-5-codex" }])"))).at("models"),
                       parse("[{}]")));
}

// ---------------------------------------------------------------------------
// Version parsing and default model selection
// ---------------------------------------------------------------------------

TEST("client version parsing and comparison reject malformed versions") {
  EXPECT_EQ(compareCodexClientVersions("1.2.3", "1.2.4"), -1.0);
  EXPECT_EQ(compareCodexClientVersions("1.3.0", "1.2.9"), 1.0);
  EXPECT_EQ(compareCodexClientVersions(" 1.2.3 ", "1.2.3"), 0.0);
  EXPECT_EQ(compareCodexClientVersions("1.2", "1.2.3"), std::nullopt);
  EXPECT_EQ(compareCodexClientVersions("1.2.3", "latest"), std::nullopt);
  EXPECT_EQ(compareCodexClientVersions("99999999999999999999.0.0", "1.0.0"), std::nullopt);
  EXPECT_EQ(compareCodexClientVersions("1.2.3-rc.4", "1.2.3"), std::nullopt);
}

TEST("the default model comes from the first entry with a usable identifier") {
  EXPECT_EQ(getCodexModelsSnapshotDefaultModel(std::nullopt), std::nullopt);
  EXPECT_EQ(getCodexModelsSnapshotDefaultModel(snapshotOf(json::array())), std::nullopt);
  EXPECT_EQ(getCodexModelsSnapshotDefaultModel(snapshotOf("none")), std::nullopt);
  EXPECT_EQ(getCodexModelsSnapshotDefaultModel(snapshotOf(parse(R"(["nope", {}, { "slug": "   " }])"))), std::nullopt);
  EXPECT_EQ(getCodexModelsSnapshotDefaultModel(snapshotOf(parse(R"([{}, { "id": "second" }])"))), std::optional<std::string>("second"));
  EXPECT_EQ(getCodexModelsSnapshotDefaultModel(snapshotOf(parse(R"([{ "model": "third" }])"))), std::optional<std::string>("third"));
  EXPECT_EQ(getCodexModelsSnapshotDefaultModel(snapshotOf(parse(R"([{ "name": "  fourth  " }])"))), std::optional<std::string>("fourth"));
}

// ---------------------------------------------------------------------------
// normalizeCodexModelsPayload
// ---------------------------------------------------------------------------

TEST("payload envelopes accept a bare array and a data wrapper") {
  const std::optional<CodexModelsSnapshot> fromArray = normalizeCodexModelsPayload(parse(R"([{ "slug": "gpt-5-codex" }])"));
  ASSERT(fromArray.has_value());
  EXPECT_EQ(rt::getString(*fromArray, "source"), std::optional<std::string_view>("codex_cli"));
  EXPECT(rt::get(*fromArray, "client_version") == nullptr);
  EXPECT(rt::getNumber(*fromArray, "updated_at_ms").value_or(0) > 0);

  const std::optional<CodexModelsSnapshot> fromData =
    normalizeCodexModelsPayload(parse(R"({ "data": [{ "slug": "gpt-5-codex" }], "source": "upstream", "client_version": "1.2.3", "updated_at_ms": 7 })"));
  ASSERT(fromData.has_value());
  EXPECT_EQ(rt::getString(*fromData, "source"), std::optional<std::string_view>("upstream"));
  EXPECT_EQ(rt::getString(*fromData, "client_version"), std::optional<std::string_view>("1.2.3"));
  EXPECT_EQ(rt::getNumber(*fromData, "updated_at_ms"), std::optional<double>(7));
}

TEST("payload normalization refuses envelopes without a model list") {
  for (const json::value& value : std::vector<json::value>{nullptr, "nope", 42, parse("[null]"), json::object(), parse(R"({ "models": "none" })"),
                                                           parse(R"({ "data": "none" })")}) {
    EXPECT(!normalizeCodexModelsPayload(value).has_value());
  }
  // The payload loop keeps the stored spelling: only an absent slug is skipped.
  const std::optional<CodexModelsSnapshot> blank = normalizeCodexModelsPayload(parse(R"([{ "slug": "   " }])"));
  EXPECT(blank.has_value() && rt::deepEqual(blank->at("models"), parse(R"([{ "slug": "   " }])")));
  const std::optional<CodexModelsSnapshot> duplicate = normalizeCodexModelsPayload(parse(R"([{ "slug": "dup" }, { "slug": "dup" }])"));
  EXPECT(duplicate.has_value() && rt::deepEqual(duplicate->at("models"), parse(R"([{ "slug": "dup" }])")));
  EXPECT(!normalizeCodexModelsPayload(parse(R"(["not-a-record"])")).has_value());
}

TEST("payload normalization keeps the first entry per slug and drops unusable ones") {
  const std::optional<CodexModelsSnapshot> snapshot = normalizeCodexModelsPayload(parse(R"([
    { "slug": "gpt-5-codex", "display_name": "Codex" },
    { "slug": "gpt-5-codex", "display_name": "Duplicate" },
    { "id": "by-id", "description": "By id" },
    { "model": "by-model" },
    { "name": "by-name" },
    { "visibility": "hide", "supported_in_api": false, "slug": "hidden" },
    "not-a-record"
  ])"));

  ASSERT(snapshot.has_value());
  std::vector<std::string> slugs;
  for (const json::value& model : snapshot->at("models").as_array()) slugs.push_back(std::string(*rt::getString(model, "slug")));
  EXPECT_EQ(slugs, (std::vector<std::string>{"gpt-5-codex", "by-id", "by-model", "by-name"}));
  EXPECT_EQ(rt::getString(modelAt(*snapshot, 0), "display_name"), std::optional<std::string_view>("Codex"));
  EXPECT_EQ(rt::getString(modelAt(*snapshot, 1), "description"), std::optional<std::string_view>("By id"));
  EXPECT(rt::get(modelAt(*snapshot, 1), "display_name") == nullptr);
}

TEST("overrides win only when they carry a value") {
  const json::value payload = parse(R"({ "models": [{ "slug": "gpt-5-codex" }], "source": "upstream", "client_version": "9.9.9", "updated_at_ms": 5 })");

  const std::optional<CodexModelsSnapshot> untouched =
    normalizeCodexModelsPayload(payload, CodexModelsPayloadOverrides{.source = "", .clientVersion = "", .updatedAtMs = std::nullopt});
  ASSERT(untouched.has_value());
  EXPECT_EQ(rt::getString(*untouched, "source"), std::optional<std::string_view>("upstream"));
  EXPECT_EQ(rt::getString(*untouched, "client_version"), std::optional<std::string_view>("9.9.9"));
  EXPECT_EQ(rt::getNumber(*untouched, "updated_at_ms"), std::optional<double>(5));

  const std::optional<CodexModelsSnapshot> overridden =
    normalizeCodexModelsPayload(payload, CodexModelsPayloadOverrides{.source = "chatgpt_codex", .clientVersion = "1.0.0", .updatedAtMs = 11.9});
  ASSERT(overridden.has_value());
  EXPECT_EQ(rt::getString(*overridden, "source"), std::optional<std::string_view>("chatgpt_codex"));
  EXPECT_EQ(rt::getString(*overridden, "client_version"), std::optional<std::string_view>("1.0.0"));
  EXPECT_EQ(rt::getNumber(*overridden, "updated_at_ms"), std::optional<double>(11));

  const std::optional<CodexModelsSnapshot> noTimestamp = normalizeCodexModelsPayload(parse(R"({ "models": [{ "slug": "gpt-5-codex" }] })"));
  ASSERT(noTimestamp.has_value());
  EXPECT(rt::getNumber(*noTimestamp, "updated_at_ms").value_or(0) > 0);
}

TEST("model entries normalize context windows, reasoning levels and cache evidence") {
  const json::value first = with(parse(R"({
    "slug": "gpt-5-codex",
    "supported_in_api": true,
    "visibility": "list",
    "context_window": 400000,
    "max_context_window": 400000.9,
    "auto_compact_token_limit": null,
    "effective_context_window_percent": -5,
    "default_reasoning_level": null,
    "supported_reasoning_levels": [null, "low", { "effort": null }, { "effort": "high" }, { "effort": "ultra" }, 42, { "effort": "ultra", "wire_effort": "max" }]
  })"),
                                 "prompt_cache", capabilitiesWithScope());
  const std::optional<CodexModelsSnapshot> snapshot = normalizeCodexModelsPayload(arrayOf({
    first,
    parse(R"({
      "slug": "flat",
      "context_window": null,
      "default_reasoning_level": "medium",
      "supported_reasoning_levels": "none"
    })"),
    parse(R"({
      "slug": "no-slug-fields",
      "context_window": "400000",
      "default_reasoning_level": "bogus",
      "supported_reasoning_levels": []
    })"),
    parse(R"({ "slug": "blank-level", "default_reasoning_level": "   " })"),
  }));

  ASSERT(snapshot.has_value());
  const json::object& codex = modelAt(*snapshot, 0);
  EXPECT_EQ(rt::getNumber(codex, "context_window"), std::optional<double>(400000));
  EXPECT_EQ(rt::getNumber(codex, "max_context_window"), std::optional<double>(400000));
  EXPECT(rt::get(codex, "auto_compact_token_limit") != nullptr && rt::get(codex, "auto_compact_token_limit")->is_null());
  EXPECT_EQ(rt::getBool(codex, "supported_in_api"), std::optional<bool>(true));
  EXPECT_EQ(rt::getString(codex, "visibility"), std::optional<std::string_view>("list"));
  EXPECT_EQ(rt::hasOwn(codex, "effective_context_window_percent"), false);
  EXPECT_EQ(rt::getString(codex, "default_reasoning_level"), std::optional<std::string_view>("none"));
  EXPECT(rt::deepEqual(codex.at("supported_reasoning_levels"), parse(R"(["none", "low", "none", "high", "ultra", "ultra"])")));
  EXPECT(rt::deepEqual(codex.at("reasoning_effort_wire_map"), parse(R"({ "ultra": "max" })")));
  EXPECT(rt::truthy(rt::get(codex, "prompt_cache")));

  const json::object& flat = modelAt(*snapshot, 1);
  EXPECT(rt::get(flat, "context_window") != nullptr && rt::get(flat, "context_window")->is_null());
  EXPECT_EQ(rt::hasOwn(flat, "max_context_window"), false);
  EXPECT_EQ(rt::getString(flat, "default_reasoning_level"), std::optional<std::string_view>("medium"));
  EXPECT_EQ(rt::hasOwn(flat, "supported_reasoning_levels"), false);

  const json::object& sparse = modelAt(*snapshot, 2);
  EXPECT_EQ(rt::hasOwn(sparse, "context_window"), false);
  // Tiers are preserved verbatim: no allowlist rejects an unknown advertised value.
  EXPECT_EQ(rt::getString(sparse, "default_reasoning_level"), std::optional<std::string_view>("bogus"));
  EXPECT_EQ(rt::hasOwn(sparse, "reasoning_effort_wire_map"), false);

  const json::object& blankLevel = modelAt(*snapshot, 3);
  EXPECT_EQ(rt::hasOwn(blankLevel, "default_reasoning_level"), false);
}

TEST("a hidden model is dropped unless the API still serves it") {
  const std::optional<CodexModelsSnapshot> snapshot = normalizeCodexModelsPayload(parse(R"([
    { "slug": "hidden", "visibility": " HIDE ", "supported_in_api": false },
    { "slug": "hidden-but-served", "visibility": "hide", "supported_in_api": true },
    { "slug": "visible", "visibility": "list" }
  ])"));

  ASSERT(snapshot.has_value());
  std::vector<std::string> slugs;
  for (const json::value& model : snapshot->at("models").as_array()) slugs.push_back(std::string(*rt::getString(model, "slug")));
  EXPECT_EQ(slugs, (std::vector<std::string>{"hidden-but-served", "visible"}));
}

TEST("a minimal controls record keeps only its required evidence") {
  const json::value minimal = parse(R"({ "source": "live_probe", "verified_at_ms": 3 })");
  const std::optional<PromptCacheCapabilities> normalized = normalizePromptCacheCapabilities(capabilitiesWith(providerWithControls(minimal, "codex_chatgpt")));

  ASSERT(versioned(normalized) != nullptr);
  ASSERT(versioned(normalized)->providers.at(0).controls.has_value());
  const json::value controls = toJson(*versioned(normalized)->providers.at(0).controls);
  EXPECT(rt::deepEqual(controls, minimal));
  for (const char* key : {"key", "implicit", "explicit_breakpoints", "modes", "ttls", "legacy_retentions", "breakpoint_block_types", "expected_usage_fields"}) {
    EXPECT_EQ(rt::hasOwn(controls.as_object(), key), false);
  }

  const std::optional<RuntimePromptCacheCapabilities> compacted = compactPromptCacheCapabilities(capabilitiesWith(providerWithControls(minimal, "codex_chatgpt")));
  ASSERT(versioned(compacted) != nullptr);
  EXPECT(rt::deepEqual(toJson(versioned(compacted)->providers.at(0).controls), minimal));
}

TEST("a provider entry must be a plain object with only id, controls and scope") {
  EXPECT(isNull(normalizePromptCacheCapabilities(parse(R"({ "version": 1, "providers": ["codex_chatgpt"] })"))));
  EXPECT(isNull(normalizePromptCacheCapabilities(parse(R"({ "version": 1, "providers": [null] })"))));
  EXPECT(isNull(normalizePromptCacheCapabilities(parse(R"({ "version": 1, "providers": [[{ "id": "codex_chatgpt" }]] })"))));
  EXPECT(isNull(normalizePromptCacheCapabilities(parse(R"({ "version": 1, "providers": [{ "id": "codex_chatgpt", "extra": true }] })"))));
  EXPECT(isNull(normalizePromptCacheCapabilities(parse(R"({ "version": 1, "providers": [{ "id": "codex_chatgpt", "controls": null }] })"))));
  EXPECT(isNull(normalizePromptCacheCapabilities(parse(R"({ "version": 1, "providers": [{ "id": "codex_chatgpt", "scope": [] }] })"))));

  const std::optional<PromptCacheCapabilities> identityOnly = normalizePromptCacheCapabilities(parse(R"({ "version": 1, "providers": [{ "id": "codex_chatgpt" }] })"));
  ASSERT(versioned(identityOnly) != nullptr);
  EXPECT(rt::deepEqual(toJson(*identityOnly), parse(R"({ "version": 1, "providers": [{ "id": "codex_chatgpt" }] })")));
}

TEST("scope attachment leaves sibling providers untouched") {
  const json::value sibling = with(parse(R"({ "id": "second_provider" })"), "controls", validControls());
  json::object promptCache;
  promptCache["version"] = 1;
  promptCache["providers"] = arrayOf({providerWithControls(validControls(), CODEX_CHATGPT_PROMPT_CACHE_PROVIDER), sibling});
  const json::value model = modelWithCache("gpt-5-codex", promptCache);
  const std::optional<CodexModelsSnapshot> attached =
    withCodexModelPromptCacheScope(snapshotOf(arrayOf({model})), "gpt-5-codex", CODEX_CHATGPT_PROMPT_CACHE_PROVIDER, json::value_to<PromptCacheScope>(validScope()));

  ASSERT(attached.has_value());
  const std::optional<PromptCacheCapabilities> cache = normalizePromptCacheCapabilities(rt::get(modelAt(*attached, 0), "prompt_cache"));
  ASSERT(versioned(cache) != nullptr);
  EXPECT_EQ(providerIds(versioned(cache)->providers), (std::vector<std::string>{std::string(CODEX_CHATGPT_PROMPT_CACHE_PROVIDER), "second_provider"}));
  EXPECT(versioned(cache)->providers.at(0).scope.has_value());
  EXPECT(rt::deepEqual(toJson(versioned(cache)->providers.at(1)), sibling));
}

TEST("a profile-only provider merge keeps the provider identity") {
  const std::optional<PromptCacheCapabilities> merged =
    mergePromptCacheCapabilities(capabilitiesWith(parse(R"({ "id": "codex_chatgpt" })")), capabilitiesWith(parse(R"({ "id": "codex_chatgpt" })")));

  ASSERT(versioned(merged) != nullptr);
  EXPECT(rt::deepEqual(toJson(*merged), parse(R"({ "version": 1, "providers": [{ "id": "codex_chatgpt" }] })")));

  // A provider that only carries scope accepts controls from the catalog side.
  const std::optional<PromptCacheCapabilities> withControls =
    mergePromptCacheCapabilities(capabilitiesWith(with(parse(R"({ "id": "codex_chatgpt" })"), "scope", validScope())), capabilitiesWithControls());
  ASSERT(versioned(withControls) != nullptr);
  EXPECT(versioned(withControls)->providers.at(0).controls.has_value());
  ASSERT(versioned(withControls)->providers.at(0).scope.has_value());
  EXPECT_EQ(versioned(withControls)->providers.at(0).scope->account_slots, "account_scoped");
}
