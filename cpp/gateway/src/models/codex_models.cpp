#include "uos/models/codex_models.hpp"

#include "uos/defaults.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace uos::models::codex_models {
  namespace {
    using defaults::ReasoningEffort;
    using rt::record::describe;
    using rt::record::Reader;
    using rt::record::Writer;

    // `new Set([...])` of string literals; `has` is true only for an equal string.
    using StringSet = std::span<const std::string_view>;

    bool setHas(StringSet set, std::string_view value) {
      for (std::string_view entry : set)
        if (entry == value) return true;
      return false;
    }

    bool setHas(StringSet set, const json::value* value) {
      const std::optional<std::string_view> text = rt::getString(value);
      return text && setHas(set, *text);
    }

    constexpr std::array<std::string_view, 3> PROMPT_CACHE_CONTROL_SOURCES = {"catalog", "live_probe", "inferred"};
    constexpr std::array<std::string_view, 1> PROMPT_CACHE_SCOPE_SOURCES = {"live_probe"};
    constexpr std::array<std::string_view, 2> PROMPT_CACHE_MODES = {"implicit", "explicit"};
    constexpr std::array<std::string_view, 2> PROMPT_CACHE_USAGE_FIELDS = {"cached_tokens", "cache_write_tokens"};
    // These are the public controls that the gateway accepts in openai.ts. Cache
    // capability metadata is a gateway-compatibility projection, so it must not
    // publish a value that the public request validator will reject.
    constexpr std::array<std::string_view, 1> PROMPT_CACHE_TTLS = {"30m"};
    constexpr std::array<std::string_view, 2> PROMPT_CACHE_LEGACY_RETENTIONS = {"in_memory", "24h"};
    // The official Chat contract permits additional marked blocks, but the
    // Chat-to-Responses adapter currently has no lossless upstream representation
    // for them. Keep published/enforced capabilities aligned with the subset this
    // gateway can preserve rather than advertising a marker it will fail closed.
    constexpr std::array<std::string_view, 3> PROMPT_CACHE_RESPONSE_BREAKPOINT_BLOCK_TYPES = {"input_text", "input_image", "input_file"};
    constexpr std::array<std::string_view, 3> PROMPT_CACHE_CHAT_BREAKPOINT_BLOCK_TYPES = {"text", "image_url", "file"};
    constexpr std::array<std::string_view, 3> PROMPT_CACHE_ACCOUNT_SLOTS = {"shared", "account_scoped", "unknown"};
    constexpr std::array<std::string_view, 3> PROMPT_CACHE_TOKEN_REFRESH = {"preserved", "changed", "unknown"};
    constexpr std::array<std::string_view, 3> PROMPT_CACHE_CONVERSATION_ID = {"independent", "scoped", "unknown"};

    bool isObjectRecord(const json::value* value) { return utils::isRecord(value) && !value->is_array(); }

    bool hasOnlyKeys(const json::object& value, std::span<const std::string_view> allowed) {
      for (const std::string& key : rt::objectKeys(value))
        if (!setHas(allowed, key)) return false;
      return true;
    }

    bool hasOwn(const json::object& value, std::string_view key) { return rt::hasOwn(value, key); }

    std::optional<std::string> normalizePromptCacheString(const json::value* value) {
      const std::optional<std::string> text = utils::getString(value);
      if (!text) return std::nullopt;
      std::string normalized = rt::trim(*text);
      if (normalized.empty()) return std::nullopt;
      return normalized;
    }

    std::optional<std::vector<std::string>> normalizePromptCacheStringList(const json::value* value) {
      if (!value || !value->is_array()) return std::nullopt;
      std::vector<std::string> normalized;
      std::unordered_set<std::string> seen;
      for (const json::value& item : value->get_array()) {
        std::optional<std::string> entry = normalizePromptCacheString(&item);
        if (!entry) return std::nullopt;
        if (!seen.contains(*entry)) {
          normalized.push_back(*entry);
          seen.insert(std::move(*entry));
        }
      }
      return normalized;
    }

    /**
     * Drop catalog values the public gateway cannot accept while retaining the
     * rest of otherwise valid capability evidence. An empty result is either
     * omitted as an unknown scalar control or invalidates an exhaustive block-type
     * declaration, depending on the caller's compatibility semantics.
     */
    std::optional<std::vector<std::string>> normalizeGatewayPromptCacheStringList(const json::value* value, StringSet allowed) {
      const std::optional<std::vector<std::string>> normalized = normalizePromptCacheStringList(value);
      if (!normalized) return std::nullopt;
      std::vector<std::string> filtered;
      for (const std::string& entry : *normalized)
        if (setHas(allowed, entry)) filtered.push_back(entry);
      return filtered;
    }

    std::optional<std::vector<std::string>> normalizePromptCacheEnumList(const json::value* value, StringSet allowed) {
      if (!value || !value->is_array()) return std::nullopt;
      std::vector<std::string> normalized;
      std::unordered_set<std::string> seen;
      for (const json::value& item : value->get_array()) {
        if (!item.is_string() || !setHas(allowed, &item)) return std::nullopt;
        std::string entry(item.get_string());
        if (!seen.contains(entry)) {
          normalized.push_back(entry);
          seen.insert(std::move(entry));
        }
      }
      return normalized;
    }

    std::optional<double> normalizePromptCacheTimestamp(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      if (number && rt::Number::isSafeInteger(*number) && *number >= 0) return number;
      return std::nullopt;
    }

    std::optional<double> normalizePromptCacheCycles(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      if (number && rt::Number::isSafeInteger(*number) && *number >= 0) return number;
      return std::nullopt;
    }

    std::optional<PromptCacheControls::BreakpointBlockTypes> normalizePromptCacheBreakpointBlockTypes(const json::value* value) {
      static constexpr std::array<std::string_view, 2> KEYS = {"responses", "chat_completions"};
      if (!isObjectRecord(value) || !hasOnlyKeys(value->get_object(), KEYS)) return std::nullopt;
      const json::object& record = value->get_object();
      const std::optional<std::vector<std::string>> responses =
        hasOwn(record, "responses") ? normalizeGatewayPromptCacheStringList(rt::get(record, "responses"), PROMPT_CACHE_RESPONSE_BREAKPOINT_BLOCK_TYPES) : std::nullopt;
      const std::optional<std::vector<std::string>> chatCompletions =
        hasOwn(record, "chat_completions") ? normalizeGatewayPromptCacheStringList(rt::get(record, "chat_completions"), PROMPT_CACHE_CHAT_BREAKPOINT_BLOCK_TYPES) : std::nullopt;
      if ((hasOwn(record, "responses") && !responses) || (hasOwn(record, "chat_completions") && !chatCompletions)) {
        return std::nullopt;
      }
      const std::optional<std::vector<std::string>> supportedResponses = responses && !responses->empty() ? responses : std::nullopt;
      const std::optional<std::vector<std::string>> supportedChatCompletions = chatCompletions && !chatCompletions->empty() ? chatCompletions : std::nullopt;
      if (!supportedResponses && !supportedChatCompletions) return std::nullopt;
      return PromptCacheControls::BreakpointBlockTypes{.responses = supportedResponses, .chat_completions = supportedChatCompletions};
    }

    constexpr std::array<std::string_view, 10> PROMPT_CACHE_CONTROLS_KEYS = {
      "key",
      "implicit",
      "explicit_breakpoints",
      "modes",
      "ttls",
      "legacy_retentions",
      "breakpoint_block_types",
      "expected_usage_fields",
      "source",
      "verified_at_ms",
    };
    constexpr std::array<std::string_view, 3> PROMPT_CACHE_CONTROLS_BOOLEAN_KEYS = {"key", "implicit", "explicit_breakpoints"};

    /** An optional control field: absent, or present with the normalizer's verdict. */
    template <class T> struct PromptCacheControlField {
      bool present;
      std::optional<T> value;
    };

    struct PromptCacheControlFields {
      PromptCacheControlField<std::vector<PromptCacheMode>> modes;
      PromptCacheControlField<std::vector<std::string>> ttls;
      PromptCacheControlField<std::vector<std::string>> legacyRetentions;
      PromptCacheControlField<PromptCacheControls::BreakpointBlockTypes> breakpointBlockTypes;
      PromptCacheControlField<std::vector<PromptCacheUsageField>> expectedUsageFields;
    };

    template <class T, class Normalize>
    PromptCacheControlField<T> readPromptCacheControlField(const json::object& value, std::string_view key, Normalize normalize) {
      if (!hasOwn(value, key)) return {false, std::nullopt};
      return {true, normalize(rt::get(value, key))};
    }

    /** Presence was validated as a boolean, so absence is the only non-boolean case left. */
    std::optional<bool> readPromptCacheControlBoolean(const json::object& value, std::string_view key) { return rt::getBool(value, key); }

    PromptCacheControls buildPromptCacheControls(const json::object& value, const PromptCacheControlFields& fields, double verifiedAtMs) {
      const std::optional<bool> key = readPromptCacheControlBoolean(value, "key");
      const std::optional<bool> implicit = readPromptCacheControlBoolean(value, "implicit");
      const std::optional<bool> explicitBreakpoints = readPromptCacheControlBoolean(value, "explicit_breakpoints");
      return {
        .key = key,
        .implicit = implicit,
        .explicit_breakpoints = explicitBreakpoints,
        .modes = fields.modes.value,
        .ttls = fields.ttls.value && !fields.ttls.value->empty() ? fields.ttls.value : std::nullopt,
        .legacy_retentions = fields.legacyRetentions.value && !fields.legacyRetentions.value->empty() ? fields.legacyRetentions.value : std::nullopt,
        .breakpoint_block_types = fields.breakpointBlockTypes.value,
        .expected_usage_fields = fields.expectedUsageFields.value,
        .source = std::string(*rt::getString(value, "source")),
        .verified_at_ms = verifiedAtMs,
      };
    }

    std::optional<PromptCacheControls> normalizePromptCacheControls(const json::value* value) {
      if (!isObjectRecord(value) || !hasOnlyKeys(value->get_object(), PROMPT_CACHE_CONTROLS_KEYS)) return std::nullopt;
      const json::object& record = value->get_object();
      if (!setHas(PROMPT_CACHE_CONTROL_SOURCES, rt::get(record, "source"))) return std::nullopt;
      const std::optional<double> verifiedAtMs = normalizePromptCacheTimestamp(rt::get(record, "verified_at_ms"));
      if (!verifiedAtMs) return std::nullopt;
      for (std::string_view key : PROMPT_CACHE_CONTROLS_BOOLEAN_KEYS)
        if (hasOwn(record, key) && !rt::get(record, key)->is_bool()) return std::nullopt;

      const PromptCacheControlFields fields{
        .modes = readPromptCacheControlField<std::vector<PromptCacheMode>>(
          record, "modes", [](const json::value* raw) { return normalizePromptCacheEnumList(raw, PROMPT_CACHE_MODES); }),
        .ttls = readPromptCacheControlField<std::vector<std::string>>(
          record, "ttls", [](const json::value* raw) { return normalizeGatewayPromptCacheStringList(raw, PROMPT_CACHE_TTLS); }),
        .legacyRetentions = readPromptCacheControlField<std::vector<std::string>>(
          record, "legacy_retentions", [](const json::value* raw) { return normalizeGatewayPromptCacheStringList(raw, PROMPT_CACHE_LEGACY_RETENTIONS); }),
        .breakpointBlockTypes = readPromptCacheControlField<PromptCacheControls::BreakpointBlockTypes>(record, "breakpoint_block_types",
                                                                                                     normalizePromptCacheBreakpointBlockTypes),
        .expectedUsageFields = readPromptCacheControlField<std::vector<PromptCacheUsageField>>(
          record, "expected_usage_fields", [](const json::value* raw) { return normalizePromptCacheEnumList(raw, PROMPT_CACHE_USAGE_FIELDS); }),
      };
      // A declared field that fails its own normalizer invalidates the whole record,
      // exactly as an exhaustive block-type declaration does.
      if ((fields.modes.present && !fields.modes.value) || (fields.ttls.present && !fields.ttls.value) ||
          (fields.legacyRetentions.present && !fields.legacyRetentions.value) || (fields.breakpointBlockTypes.present && !fields.breakpointBlockTypes.value) ||
          (fields.expectedUsageFields.present && !fields.expectedUsageFields.value))
        return std::nullopt;
      return buildPromptCacheControls(record, fields, *verifiedAtMs);
    }

    std::optional<PromptCacheScope> normalizePromptCacheScope(const json::value* value) {
      static constexpr std::array<std::string_view, 8> KEYS = {
        "probe_profile", "account_slots", "token_refresh", "conversation_id", "effective_model", "reproducible_cycles", "source", "verified_at_ms",
      };
      if (!isObjectRecord(value) || !hasOnlyKeys(value->get_object(), KEYS)) return std::nullopt;
      const json::object& record = value->get_object();
      const std::optional<std::string_view> probeProfile = rt::getString(record, "probe_profile");
      if (!probeProfile || *probeProfile != PROMPT_CACHE_SCOPE_PROBE_PROFILE) return std::nullopt;
      if (!setHas(PROMPT_CACHE_ACCOUNT_SLOTS, rt::get(record, "account_slots"))) return std::nullopt;
      if (!setHas(PROMPT_CACHE_TOKEN_REFRESH, rt::get(record, "token_refresh"))) return std::nullopt;
      if (!setHas(PROMPT_CACHE_CONVERSATION_ID, rt::get(record, "conversation_id"))) return std::nullopt;
      if (!setHas(PROMPT_CACHE_SCOPE_SOURCES, rt::get(record, "source"))) return std::nullopt;
      const std::optional<double> reproducibleCycles = normalizePromptCacheCycles(rt::get(record, "reproducible_cycles"));
      const std::optional<double> verifiedAtMs = normalizePromptCacheTimestamp(rt::get(record, "verified_at_ms"));
      if (!reproducibleCycles || !verifiedAtMs) return std::nullopt;
      // Scope is only published after the bounded live probe has reproduced the
      // complete classification. An unverified "shared" result is no safer than
      // an unverified "account_scoped" result, and callers should treat omitted
      // scope as unknown rather than consuming an early classification.
      if (*reproducibleCycles < 3) return std::nullopt;
      const std::optional<std::string> effectiveModel =
        hasOwn(record, "effective_model") ? normalizePromptCacheString(rt::get(record, "effective_model")) : std::nullopt;
      if (hasOwn(record, "effective_model") && !effectiveModel) return std::nullopt;
      return PromptCacheScope{
        .probe_profile = std::string(PROMPT_CACHE_SCOPE_PROBE_PROFILE),
        .account_slots = std::string(*rt::getString(record, "account_slots")),
        .token_refresh = std::string(*rt::getString(record, "token_refresh")),
        .conversation_id = std::string(*rt::getString(record, "conversation_id")),
        .effective_model = effectiveModel,
        .reproducible_cycles = *reproducibleCycles,
        .source = std::string(*rt::getString(record, "source")),
        .verified_at_ms = *verifiedAtMs,
      };
    }

    std::optional<PromptCacheProvider> normalizePromptCacheProvider(const json::value* value) {
      static constexpr std::array<std::string_view, 3> KEYS = {"id", "controls", "scope"};
      if (!isObjectRecord(value) || !hasOnlyKeys(value->get_object(), KEYS)) return std::nullopt;
      const json::object& record = value->get_object();
      std::optional<std::string> id = normalizePromptCacheString(rt::get(record, "id"));
      if (!id) return std::nullopt;
      std::optional<PromptCacheControls> controls = hasOwn(record, "controls") ? normalizePromptCacheControls(rt::get(record, "controls")) : std::nullopt;
      std::optional<PromptCacheScope> scope = hasOwn(record, "scope") ? normalizePromptCacheScope(rt::get(record, "scope")) : std::nullopt;
      if ((hasOwn(record, "controls") && !controls) || (hasOwn(record, "scope") && !scope)) return std::nullopt;
      return PromptCacheProvider{.id = std::move(*id), .controls = std::move(controls), .scope = std::move(scope)};
    }
  }

  std::optional<PromptCacheCapabilities> normalizePromptCacheCapabilities(const json::value* value) {
    if (value && value->is_bool() && !value->get_bool()) return PromptCacheCapabilitiesUnsupported{};
    static constexpr std::array<std::string_view, 2> KEYS = {"version", "providers"};
    if (!isObjectRecord(value) || !hasOnlyKeys(value->get_object(), KEYS) || rt::getNumber(value->get_object(), "version") != 1.0) return std::nullopt;
    const json::array* rawProviders = rt::getArray(value->get_object(), "providers");
    if (!rawProviders || rawProviders->empty()) return std::nullopt;
    std::vector<PromptCacheProvider> providers;
    std::unordered_set<std::string> seen;
    for (const json::value& rawProvider : *rawProviders) {
      std::optional<PromptCacheProvider> provider = normalizePromptCacheProvider(&rawProvider);
      if (!provider || seen.contains(provider->id)) return std::nullopt;
      seen.insert(provider->id);
      providers.push_back(std::move(*provider));
    }
    return PromptCacheCapabilitiesV1{.version = 1, .providers = std::move(providers)};
  }

  std::optional<PromptCacheCapabilities> normalizePromptCacheCapabilities(const json::value& value) { return normalizePromptCacheCapabilities(&value); }

  std::optional<RuntimePromptCacheCapabilities> compactPromptCacheCapabilities(const json::value* value) {
    const std::optional<PromptCacheCapabilities> normalized = normalizePromptCacheCapabilities(value);
    if (!normalized) return std::nullopt;
    const PromptCacheCapabilitiesV1* versioned = std::get_if<PromptCacheCapabilitiesV1>(&*normalized);
    if (!versioned) return RuntimePromptCacheCapabilitiesUnsupported{};
    std::vector<RuntimePromptCacheCapabilitiesV1::Provider> providers;
    for (const PromptCacheProvider& provider : versioned->providers) {
      if (provider.controls) providers.push_back({.id = provider.id, .controls = *provider.controls});
    }
    if (providers.empty()) return std::nullopt;
    return RuntimePromptCacheCapabilitiesV1{.version = 1, .providers = std::move(providers)};
  }

  std::optional<RuntimePromptCacheCapabilities> compactPromptCacheCapabilities(const json::value& value) { return compactPromptCacheCapabilities(&value); }

  namespace {
    std::optional<std::string> modelSlug(const json::object& model) {
      std::optional<std::string> slug = utils::getString(rt::get(model, "slug"));
      if (!slug) slug = utils::getString(rt::get(model, "id"));
      if (!slug) slug = utils::getString(rt::get(model, "model"));
      if (!slug) slug = utils::getString(rt::get(model, "name"));
      if (!slug) return std::nullopt;
      std::string trimmed = rt::trim(*slug);
      if (trimmed.empty()) return std::nullopt;
      return trimmed;
    }

    bool isCodexModelWithSlug(const json::value& value, std::string_view slug) {
      if (!isObjectRecord(&value)) return false;
      const std::optional<std::string> candidate = modelSlug(value.get_object());
      return candidate && *candidate == slug;
    }

    const PromptCacheCapabilitiesV1* versionedCapabilities(const std::optional<PromptCacheCapabilities>& capabilities) {
      return capabilities ? std::get_if<PromptCacheCapabilitiesV1>(&*capabilities) : nullptr;
    }
  }

  std::optional<json::object> getUniqueCodexModelBySlug(const CodexModelsSnapshot& snapshot, std::string_view slug) {
    const std::string target = rt::trim(slug);
    const json::array* models = rt::getArray(snapshot, "models");
    if (target.empty() || !models) return std::nullopt;
    std::vector<const json::object*> matches;
    for (const json::value& model : *models)
      if (isCodexModelWithSlug(model, target)) matches.push_back(&model.get_object());
    if (matches.size() != 1) return std::nullopt;
    return *matches.at(0);
  }

  std::optional<PromptCacheProvider> getCodexModelPromptCacheProvider(const CodexModelsSnapshot& snapshot, std::string_view slug, std::string_view providerId) {
    const std::optional<json::object> model = getUniqueCodexModelBySlug(snapshot, slug);
    if (!model) return std::nullopt;
    const std::optional<PromptCacheCapabilities> promptCache = normalizePromptCacheCapabilities(rt::get(*model, "prompt_cache"));
    const PromptCacheCapabilitiesV1* versioned = versionedCapabilities(promptCache);
    if (!versioned) return std::nullopt;
    for (const PromptCacheProvider& provider : versioned->providers)
      if (provider.id == providerId) return provider;
    return std::nullopt;
  }

  bool isCodexModelPromptCacheScopeExperimentEligible(const CodexModelsSnapshot& snapshot, std::string_view slug) {
    const std::optional<PromptCacheProvider> provider = getCodexModelPromptCacheProvider(snapshot, slug, CODEX_CHATGPT_PROMPT_CACHE_PROVIDER);
    const std::optional<PromptCacheControls> controls = provider ? provider->controls : std::nullopt;
    const std::optional<std::vector<PromptCacheUsageField>> expectedUsageFields = controls ? controls->expected_usage_fields : std::nullopt;
    return controls && controls->key == true && controls->implicit != false &&
           // The fixed profile uses only prompt_cache_key. `modes` describes the
           // optional prompt_cache_options field, so an explicit-only options
           // declaration cannot disqualify this distinct request shape.
           expectedUsageFields && rt::includes(*expectedUsageFields, "cached_tokens") && rt::includes(*expectedUsageFields, "cache_write_tokens");
  }

  bool isConcretePromptCacheScope(const PromptCacheScope& scope, double reproducibleCycles) {
    // `probe_profile` and `source` are single literals at the type level, but this
    // predicate also guards admin-supplied scope records, whose runtime values can
    // still disagree with the declared type.
    const std::string& probeProfile = scope.probe_profile;
    const std::string& source = scope.source;
    return probeProfile == PROMPT_CACHE_SCOPE_PROBE_PROFILE && scope.account_slots != "unknown" && scope.token_refresh != "unknown" &&
           scope.conversation_id != "unknown" && scope.reproducible_cycles == reproducibleCycles && source == "live_probe" && scope.effective_model &&
           !rt::trim(*scope.effective_model).empty();
  }

  std::optional<CodexModelsSnapshot> withCodexModelPromptCacheScope(const CodexModelsSnapshot& snapshot, std::string_view slug, std::string_view providerId,
                                                                    const PromptCacheScope& scope) {
    const std::string target = rt::trim(slug);
    const std::optional<json::object> model = getUniqueCodexModelBySlug(snapshot, target);
    const json::value scopeValue = toJson(scope);
    const std::optional<PromptCacheScope> normalizedScope = normalizePromptCacheScope(&scopeValue);
    if (!model || !normalizedScope) return std::nullopt;
    const std::optional<PromptCacheCapabilities> promptCache = normalizePromptCacheCapabilities(rt::get(*model, "prompt_cache"));
    const PromptCacheCapabilitiesV1* versioned = versionedCapabilities(promptCache);
    if (!versioned) return std::nullopt;
    bool hasProvider = false;
    for (const PromptCacheProvider& provider : versioned->providers)
      if (provider.id == providerId) hasProvider = true;
    if (!hasProvider) return std::nullopt;

    json::array providers;
    for (const PromptCacheProvider& provider : versioned->providers) {
      if (provider.id != providerId) {
        providers.push_back(toJson(provider));
        continue;
      }
      PromptCacheProvider withScope = provider;
      withScope.scope = normalizedScope;
      providers.push_back(toJson(withScope));
    }
    json::object nextRaw;
    rt::objectDefine(nextRaw, "version", rt::makeNumber(1.0));
    rt::objectDefine(nextRaw, "providers", json::value(std::move(providers)));
    const json::value nextRawValue(std::move(nextRaw));
    const std::optional<PromptCacheCapabilities> nextPromptCache = normalizePromptCacheCapabilities(&nextRawValue);
    if (!versionedCapabilities(nextPromptCache)) return std::nullopt;

    // getUniqueCodexModelBySlug found the model, so `models` is an array here.
    const json::array& models = *rt::getArray(snapshot, "models");
    bool hasCandidate = false;
    for (const json::value& candidate : models)
      if (isCodexModelWithSlug(candidate, target)) hasCandidate = true;
    if (!hasCandidate) return std::nullopt;
    const json::value nextPromptCacheValue = toJson(*nextPromptCache);
    json::array nextModels;
    for (const json::value& candidate : models) {
      if (!isCodexModelWithSlug(candidate, target)) {
        nextModels.push_back(candidate);
        continue;
      }
      json::object entry = rt::objectSpread(candidate.get_object());
      rt::objectDefine(entry, "prompt_cache", nextPromptCacheValue);
      nextModels.emplace_back(std::move(entry));
    }
    json::object next = rt::objectSpread(snapshot);
    rt::objectDefine(next, "models", json::value(std::move(nextModels)));
    return next;
  }

  namespace {
    PromptCacheProvider mergePromptCacheProvider(const PromptCacheProvider* previous, const PromptCacheProvider& next) {
      std::optional<PromptCacheControls> controls = next.controls ? next.controls : previous ? previous->controls : std::nullopt;
      std::optional<PromptCacheScope> scope = next.scope ? next.scope : previous ? previous->scope : std::nullopt;
      return {.id = next.id, .controls = std::move(controls), .scope = std::move(scope)};
    }
  }

  std::optional<PromptCacheCapabilities> mergePromptCacheCapabilities(const json::value* previousRaw, const json::value* nextRaw) {
    std::optional<PromptCacheCapabilities> previous = normalizePromptCacheCapabilities(previousRaw);
    std::optional<PromptCacheCapabilities> next = normalizePromptCacheCapabilities(nextRaw);
    if (next && std::holds_alternative<PromptCacheCapabilitiesUnsupported>(*next)) return PromptCacheCapabilitiesUnsupported{};
    if (!next) return previous;
    const PromptCacheCapabilitiesV1* previousVersioned = versionedCapabilities(previous);
    if (!previousVersioned) return next;
    const PromptCacheCapabilitiesV1& nextVersioned = std::get<PromptCacheCapabilitiesV1>(*next);

    std::unordered_map<std::string, const PromptCacheProvider*> previousById;
    for (const PromptCacheProvider& provider : previousVersioned->providers) previousById.insert_or_assign(provider.id, &provider);
    std::vector<PromptCacheProvider> providers;
    for (const PromptCacheProvider& provider : nextVersioned.providers) {
      const auto found = previousById.find(provider.id);
      providers.push_back(mergePromptCacheProvider(found == previousById.end() ? nullptr : found->second, provider));
    }
    std::unordered_set<std::string> nextIds;
    for (const PromptCacheProvider& provider : nextVersioned.providers) nextIds.insert(provider.id);
    for (const PromptCacheProvider& provider : previousVersioned->providers) {
      if (!nextIds.contains(provider.id)) providers.push_back(provider);
    }
    return PromptCacheCapabilitiesV1{.version = 1, .providers = std::move(providers)};
  }

  std::optional<PromptCacheCapabilities> mergePromptCacheCapabilities(const json::value& previousRaw, const json::value& nextRaw) {
    return mergePromptCacheCapabilities(&previousRaw, &nextRaw);
  }

  namespace {
    using PreviousBySlug = std::unordered_map<std::string, const json::object*>;

    // Returns std::nullopt for "the same entry" when the merge adds nothing, so callers
    // can detect changes (the TS compares the returned reference).
    std::optional<json::object> mergeCodexModelPromptCache(const json::value& value, const PreviousBySlug& previousBySlug) {
      if (!isObjectRecord(&value)) return std::nullopt;
      const json::object& record = value.get_object();
      const std::optional<std::string> slug = modelSlug(record);
      const json::object* prior = nullptr;
      if (slug) {
        const auto found = previousBySlug.find(*slug);
        if (found != previousBySlug.end()) prior = found->second;
      }
      const std::optional<PromptCacheCapabilities> promptCache = mergePromptCacheCapabilities(prior ? rt::get(*prior, "prompt_cache") : nullptr, rt::get(record, "prompt_cache"));
      if (!promptCache) return std::nullopt;
      json::object merged = rt::objectSpread(record);
      rt::objectDefine(merged, "prompt_cache", toJson(*promptCache));
      return merged;
    }

    // JavaScriptCore's TypeError for `next.models.map(...)` when `next.models` is not an array.
    [[noreturn]] void throwModelsMapTypeError(const json::value* models) {
      if (!models) throw rt::TypeError("undefined is not an object (evaluating 'next.models.map')");
      if (models->is_null()) throw rt::TypeError("null is not an object (evaluating 'next.models.map')");
      throw rt::TypeError(
        "next.models.map is not a function. (In 'next.models.map((value) => mergeCodexModelPromptCache(value, previousBySlug))', 'next.models.map' is undefined)");
    }
  }

  CodexModelsSnapshot mergeCodexModelPromptCacheCapabilities(const CodexModelsSnapshot& next, const std::optional<CodexModelsSnapshot>& previous) {
    // `models` is required by the snapshot type, but the previous snapshot is read
    // back from a stored record, so the shape is validated rather than trusted.
    const json::array* previousModels = previous ? rt::getArray(*previous, "models") : nullptr;
    if (!previousModels || previousModels->empty()) return next;
    PreviousBySlug previousBySlug;
    for (const json::value& value : *previousModels) {
      if (!isObjectRecord(&value)) continue;
      const std::optional<std::string> slug = modelSlug(value.get_object());
      if (slug && !previousBySlug.contains(*slug)) previousBySlug.emplace(*slug, &value.get_object());
    }

    const json::value* nextModelsValue = rt::get(next, "models");
    const json::array* nextModels = nextModelsValue ? nextModelsValue->if_array() : nullptr;
    if (!nextModels) throwModelsMapTypeError(nextModelsValue);
    json::array models;
    bool changed = false;
    for (const json::value& value : *nextModels) {
      std::optional<json::object> merged = mergeCodexModelPromptCache(value, previousBySlug);
      if (!merged) {
        models.push_back(value);
        continue;
      }
      changed = true;
      models.emplace_back(std::move(*merged));
    }
    if (!changed) return next;
    json::object result = rt::objectSpread(next);
    rt::objectDefine(result, "models", json::value(std::move(models)));
    return result;
  }

  std::optional<std::array<double, 3>> parseCodexClientVersion(std::string_view value) {
    static const rt::Regex VERSION("^(\\d+)\\.(\\d+)\\.(\\d+)$", "");
    const std::string trimmed = rt::trim(value);
    const std::optional<rt::RegexMatch> match = VERSION.exec(trimmed);
    if (!match) return std::nullopt;
    std::array<double, 3> parsed{};
    for (std::size_t index = 0; index < parsed.size(); ++index) parsed[index] = rt::stringToNumber(*(*match)[index + 1]);
    for (const double part : parsed)
      if (!rt::Number::isSafeInteger(part)) return std::nullopt;
    return parsed;
  }

  std::optional<double> compareCodexClientVersions(std::string_view left, std::string_view right) {
    const std::optional<std::array<double, 3>> leftParts = parseCodexClientVersion(left);
    const std::optional<std::array<double, 3>> rightParts = parseCodexClientVersion(right);
    if (!leftParts || !rightParts) return std::nullopt;
    for (std::size_t index = 0; index < leftParts->size(); ++index) {
      if ((*leftParts)[index] != (*rightParts)[index]) return (*leftParts)[index] - (*rightParts)[index];
    }
    return 0.0;
  }

  std::optional<std::string> getCodexModelsSnapshotDefaultModel(const std::optional<CodexModelsSnapshot>& snapshot) {
    const json::array* models = snapshot ? rt::getArray(*snapshot, "models") : nullptr;
    if (!models) return std::nullopt;
    for (const json::value& model : *models) {
      if (!utils::isRecord(model)) continue;
      std::optional<std::string> id = utils::getString(rt::get(model, "slug"));
      if (!id) id = utils::getString(rt::get(model, "id"));
      if (!id) id = utils::getString(rt::get(model, "model"));
      if (!id) id = utils::getString(rt::get(model, "name"));
      if (!id) continue;
      std::string trimmed = rt::trim(*id);
      if (!trimmed.empty()) return trimmed;
    }
    return std::nullopt;
  }

  namespace {
    bool isHiddenCodexModel(const json::value& value) {
      const std::optional<std::string> visibility = utils::getString(rt::get(value, "visibility"));
      if (!visibility || rt::toLowerCase(rt::trim(*visibility)) != "hide") return false;
      const json::value* supportedInApi = rt::get(value, "supported_in_api");
      return !(supportedInApi && supportedInApi->is_bool() && supportedInApi->get_bool());
    }

    std::optional<double> normalizeNonNegativeInteger(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      if (!number || !rt::Number::isFinite(*number)) return std::nullopt;
      const double normalized = rt::Math::trunc(*number);
      if (normalized >= 0) return normalized;
      return std::nullopt;
    }

    std::optional<ReasoningEffort> reasoningLevelEffort(const json::value* value) {
      if (value && value->is_null()) return ReasoningEffort("none");
      if (value && value->is_string()) return defaults::normalizeReasoningEffort(value);
      if (!utils::isRecord(value)) return std::nullopt;
      const json::value* effort = rt::get(*value, "effort");
      if (effort && effort->is_null()) return ReasoningEffort("none");
      return defaults::normalizeReasoningEffort(effort);
    }

    json::object deriveReasoningEffortWireMap(const json::array& levels) {
      rt::OrderedMap<ReasoningEffort, ReasoningEffort> wireMap;

      for (const json::value& level : levels) {
        const std::optional<ReasoningEffort> effort = reasoningLevelEffort(&level);
        if (!effort) continue;
        const std::optional<ReasoningEffort> explicitWireEffort = utils::isRecord(level) ? defaults::normalizeReasoningEffort(rt::get(level, "wire_effort")) : std::nullopt;
        const ReasoningEffort wireEffort = *effort == "ultra" ? ReasoningEffort("max") : explicitWireEffort.value_or(*effort);
        if (wireEffort != *effort) wireMap.set(*effort, wireEffort);
      }
      std::vector<std::pair<std::string, json::value>> entries;
      for (const auto& [effort, wireEffort] : wireMap) entries.emplace_back(effort, json::value(json::string(wireEffort)));
      return rt::objectFromEntries(entries);
    }

    constexpr std::string_view CODEX_MODELS_DEFAULT_SOURCE = "codex_cli";
    constexpr std::array<std::string_view, 4> CODEX_MODEL_CONTEXT_WINDOW_KEYS = {"context_window", "max_context_window", "auto_compact_token_limit",
                                                                                 "effective_context_window_percent"};

    std::optional<double> normalizeCodexModelsUpdatedAtMs(std::optional<double> value) {
      if (value && rt::Number::isFinite(*value)) return rt::Math::trunc(*value);
      return std::nullopt;
    }

    std::optional<double> normalizeCodexModelsUpdatedAtMs(const json::value* value) { return normalizeCodexModelsUpdatedAtMs(rt::getNumber(value)); }

    struct CodexModelsPayloadEnvelope {
      const json::array* modelsRaw;  // unknown: the model list, or null
      std::string source;
      std::optional<std::string> clientVersion;
      std::optional<double> updatedAtMs;
    };

    CodexModelsPayloadEnvelope normalizeCodexModelsEnvelope(const json::value* value) {
      if (value && value->is_array()) {
        return {.modelsRaw = &value->get_array(), .source = std::string(CODEX_MODELS_DEFAULT_SOURCE), .clientVersion = std::nullopt, .updatedAtMs = std::nullopt};
      }
      if (!utils::isRecord(value)) {
        return {.modelsRaw = nullptr, .source = std::string(CODEX_MODELS_DEFAULT_SOURCE), .clientVersion = std::nullopt, .updatedAtMs = std::nullopt};
      }
      const json::object& record = value->get_object();
      const json::array* modelsRaw = nullptr;
      if (const json::array* models = rt::getArray(record, "models")) modelsRaw = models;
      else if (const json::array* data = rt::getArray(record, "data")) modelsRaw = data;
      std::optional<std::string> clientVersion = utils::getString(rt::get(record, "client_version"));
      if (!clientVersion) clientVersion = utils::getString(rt::get(record, "clientVersion"));
      return {
        .modelsRaw = modelsRaw,
        .source = utils::getString(rt::get(record, "source")).value_or(std::string(CODEX_MODELS_DEFAULT_SOURCE)),
        .clientVersion = std::move(clientVersion),
        .updatedAtMs = normalizeCodexModelsUpdatedAtMs(rt::get(record, "updated_at_ms")),
      };
    }

    /** Copies the context-window fields in list order, so catalog key order is stable. */
    void applyCodexModelContextWindowFields(const json::object& item, json::object& normalized) {
      for (const std::string_view key : CODEX_MODEL_CONTEXT_WINDOW_KEYS) {
        const json::value* raw = rt::get(item, key);
        if (raw && raw->is_null()) {
          rt::objectSet(normalized, key, nullptr);
        } else {
          const std::optional<double> count = normalizeNonNegativeInteger(raw);
          if (count) rt::objectSet(normalized, key, rt::makeNumber(*count));
        }
      }
    }

    void applyCodexModelReasoningFields(const json::object& item, json::object& normalized) {
      const json::value* defaultLevel = rt::get(item, "default_reasoning_level");
      const std::optional<ReasoningEffort> defaultReasoning =
        defaultLevel && defaultLevel->is_null() ? std::optional<ReasoningEffort>("none") : defaults::normalizeReasoningEffort(defaultLevel);
      if (defaultReasoning) rt::objectSet(normalized, "default_reasoning_level", json::string(*defaultReasoning));
      // Every non-empty advertised tier is preserved verbatim, and no tier is added:
      // the upstream rejects values it does not support (`gpt-6-astra` answers a
      // `reasoning.effort: "none"` request with a 400 naming low..max), so advertising
      // a tier the catalog omits is a claim the endpoint will refuse. A null entry in
      // the upstream list still means `none`, which is the upstream's own value.
      const json::array* rawLevels = rt::getArray(item, "supported_reasoning_levels");
      if (!rawLevels) return;
      json::array levels;
      for (const json::value& rawLevel : *rawLevels) {
        if (const std::optional<ReasoningEffort> level = reasoningLevelEffort(&rawLevel)) levels.emplace_back(json::string(*level));
      }
      if (!levels.empty()) rt::objectSet(normalized, "supported_reasoning_levels", json::value(std::move(levels)));
      json::object wireMap = deriveReasoningEffortWireMap(*rawLevels);
      if (!wireMap.empty()) rt::objectSet(normalized, "reasoning_effort_wire_map", json::value(std::move(wireMap)));
    }

    json::object normalizeCodexModelEntry(const json::object& item, const std::string& slug) {
      json::object normalized;
      rt::objectDefine(normalized, "slug", json::string(slug));
      std::optional<std::string> displayName = utils::getString(rt::get(item, "display_name"));
      if (!displayName) displayName = utils::getString(rt::get(item, "displayName"));
      if (!displayName) displayName = utils::getString(rt::get(item, "name"));
      if (displayName && !displayName->empty()) rt::objectSet(normalized, "display_name", json::string(*displayName));
      const std::optional<std::string> description = utils::getString(rt::get(item, "description"));
      if (description && !description->empty()) rt::objectSet(normalized, "description", json::string(*description));
      const std::optional<std::string> visibility = utils::getString(rt::get(item, "visibility"));
      if (visibility && !visibility->empty()) rt::objectSet(normalized, "visibility", json::string(*visibility));
      if (const std::optional<bool> supportedInApi = rt::getBool(item, "supported_in_api")) rt::objectSet(normalized, "supported_in_api", *supportedInApi);
      applyCodexModelContextWindowFields(item, normalized);
      applyCodexModelReasoningFields(item, normalized);
      const std::optional<PromptCacheCapabilities> promptCache = normalizePromptCacheCapabilities(rt::get(item, "prompt_cache"));
      if (promptCache) rt::objectSet(normalized, "prompt_cache", toJson(*promptCache));
      return normalized;
    }
  }

  std::optional<CodexModelsSnapshot> normalizeCodexModelsPayload(const json::value* value, const CodexModelsPayloadOverrides& overrides) {
    const CodexModelsPayloadEnvelope envelope = normalizeCodexModelsEnvelope(value);
    const json::array* modelsRaw = envelope.modelsRaw;
    if (!modelsRaw) return std::nullopt;

    json::array models;
    std::unordered_set<std::string> seen;
    for (const json::value& item : *modelsRaw) {
      if (!utils::isRecord(item) || isHiddenCodexModel(item)) continue;
      std::optional<std::string> slug = utils::getString(rt::get(item, "slug"));
      if (!slug) slug = utils::getString(rt::get(item, "id"));
      if (!slug) slug = utils::getString(rt::get(item, "model"));
      if (!slug) slug = utils::getString(rt::get(item, "name"));
      if (!slug || slug->empty() || seen.contains(*slug)) continue;
      models.emplace_back(normalizeCodexModelEntry(item.get_object(), *slug));
      seen.insert(*slug);
    }
    if (models.empty()) return std::nullopt;

    std::optional<double> updatedAtMs = normalizeCodexModelsUpdatedAtMs(overrides.updatedAtMs);
    if (!updatedAtMs) updatedAtMs = envelope.updatedAtMs;
    // Only a non-empty override replaces the payload's own field, matching the
    // original truthiness rule; `??` would let an empty string win.
    std::string source = envelope.source;
    if (overrides.source && !overrides.source->empty()) source = *overrides.source;
    std::optional<std::string> clientVersion = envelope.clientVersion;
    if (overrides.clientVersion && !overrides.clientVersion->empty()) clientVersion = *overrides.clientVersion;
    json::object snapshot;
    rt::objectDefine(snapshot, "models", json::value(std::move(models)));
    rt::objectDefine(snapshot, "source", json::string(source));
    rt::objectDefine(snapshot, "updated_at_ms", rt::makeNumber(updatedAtMs ? *updatedAtMs : rt::dateNow()));
    // `client_version: clientVersion ?? undefined`: an undefined member is absent.
    if (clientVersion) rt::objectDefine(snapshot, "client_version", json::string(*clientVersion));
    return snapshot;
  }

  std::optional<CodexModelsSnapshot> normalizeCodexModelsPayload(const json::value* value) { return normalizeCodexModelsPayload(value, CodexModelsPayloadOverrides{}); }

  std::optional<CodexModelsSnapshot> normalizeCodexModelsPayload(const json::value& value) { return normalizeCodexModelsPayload(&value, CodexModelsPayloadOverrides{}); }

  std::optional<CodexModelsSnapshot> normalizeCodexModelsPayload(const json::value& value, const CodexModelsPayloadOverrides& overrides) {
    return normalizeCodexModelsPayload(&value, overrides);
  }

  json::value toJson(const PromptCacheControls& value) { return json::value_from(value); }

  json::value toJson(const PromptCacheScope& value) { return json::value_from(value); }

  json::value toJson(const PromptCacheProvider& value) { return json::value_from(value); }

  json::value toJson(const PromptCacheCapabilities& value) {
    return std::visit([](const auto& arm) { return json::value_from(arm); }, value);
  }

  json::value toJson(const RuntimePromptCacheCapabilities& value) {
    return std::visit([](const auto& arm) { return json::value_from(arm); }, value);
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheControls::BreakpointBlockTypes& value) {
    out = Writer().orUndefined("responses", value.responses).orUndefined("chat_completions", value.chat_completions).done();
  }

  PromptCacheControls::BreakpointBlockTypes tag_invoke(const json::value_to_tag<PromptCacheControls::BreakpointBlockTypes>&, const json::value& value) {
    const Reader in(value, "PromptCacheControls.breakpoint_block_types");
    return {
      .responses = in.orUndefined<std::vector<std::string>>("responses"),
      .chat_completions = in.orUndefined<std::vector<std::string>>("chat_completions"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheControls& value) {
    out = Writer()
      .orUndefined("key", value.key)
      .orUndefined("implicit", value.implicit)
      .orUndefined("explicit_breakpoints", value.explicit_breakpoints)
      .orUndefined("modes", value.modes)
      .orUndefined("ttls", value.ttls)
      .orUndefined("legacy_retentions", value.legacy_retentions)
      .orUndefined("breakpoint_block_types", value.breakpoint_block_types)
      .orUndefined("expected_usage_fields", value.expected_usage_fields)
      .required("source", value.source)
      .required("verified_at_ms", value.verified_at_ms)
      .done();
  }

  PromptCacheControls tag_invoke(const json::value_to_tag<PromptCacheControls>&, const json::value& value) {
    const Reader in(value, "PromptCacheControls");
    return {
      .key = in.orUndefined<bool>("key"),
      .implicit = in.orUndefined<bool>("implicit"),
      .explicit_breakpoints = in.orUndefined<bool>("explicit_breakpoints"),
      .modes = in.orUndefined<std::vector<PromptCacheMode>>("modes"),
      .ttls = in.orUndefined<std::vector<std::string>>("ttls"),
      .legacy_retentions = in.orUndefined<std::vector<std::string>>("legacy_retentions"),
      .breakpoint_block_types = in.orUndefined<PromptCacheControls::BreakpointBlockTypes>("breakpoint_block_types"),
      .expected_usage_fields = in.orUndefined<std::vector<PromptCacheUsageField>>("expected_usage_fields"),
      .source = in.required<std::string>("source"),
      .verified_at_ms = in.required<double>("verified_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheScope& value) {
    out = Writer()
      .required("probe_profile", value.probe_profile)
      .required("account_slots", value.account_slots)
      .required("token_refresh", value.token_refresh)
      .required("conversation_id", value.conversation_id)
      .orUndefined("effective_model", value.effective_model)
      .required("reproducible_cycles", value.reproducible_cycles)
      .required("source", value.source)
      .required("verified_at_ms", value.verified_at_ms)
      .done();
  }

  PromptCacheScope tag_invoke(const json::value_to_tag<PromptCacheScope>&, const json::value& value) {
    const Reader in(value, "PromptCacheScope");
    return {
      .probe_profile = in.required<std::string>("probe_profile"),
      .account_slots = in.required<std::string>("account_slots"),
      .token_refresh = in.required<std::string>("token_refresh"),
      .conversation_id = in.required<std::string>("conversation_id"),
      .effective_model = in.orUndefined<std::string>("effective_model"),
      .reproducible_cycles = in.required<double>("reproducible_cycles"),
      .source = in.required<std::string>("source"),
      .verified_at_ms = in.required<double>("verified_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheProvider& value) {
    out = Writer().required("id", value.id).orUndefined("controls", value.controls).orUndefined("scope", value.scope).done();
  }

  PromptCacheProvider tag_invoke(const json::value_to_tag<PromptCacheProvider>&, const json::value& value) {
    const Reader in(value, "PromptCacheProvider");
    return {
      .id = in.required<std::string>("id"),
      .controls = in.orUndefined<PromptCacheControls>("controls"),
      .scope = in.orUndefined<PromptCacheScope>("scope"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheCapabilitiesUnsupported&) { out = false; }

  PromptCacheCapabilitiesUnsupported tag_invoke(const json::value_to_tag<PromptCacheCapabilitiesUnsupported>&, const json::value& value) {
    if (value.is_bool() && !value.get_bool()) return {};
    throw rt::TypeError("PromptCacheCapabilities is " + describe(&value) + ", not false");
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheCapabilitiesV1& value) {
    out = Writer().required("version", value.version).required("providers", value.providers).done();
  }

  PromptCacheCapabilitiesV1 tag_invoke(const json::value_to_tag<PromptCacheCapabilitiesV1>&, const json::value& value) {
    const Reader in(value, "PromptCacheCapabilities");
    return {.version = in.required<double>("version"), .providers = in.required<std::vector<PromptCacheProvider>>("providers")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const RuntimePromptCacheCapabilitiesV1::Provider& value) {
    out = Writer().required("id", value.id).required("controls", value.controls).done();
  }

  RuntimePromptCacheCapabilitiesV1::Provider tag_invoke(const json::value_to_tag<RuntimePromptCacheCapabilitiesV1::Provider>&, const json::value& value) {
    const Reader in(value, "RuntimePromptCacheCapabilities.providers[]");
    return {.id = in.required<std::string>("id"), .controls = in.required<PromptCacheControls>("controls")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const RuntimePromptCacheCapabilitiesV1& value) {
    out = Writer().required("version", value.version).required("providers", value.providers).done();
  }

  RuntimePromptCacheCapabilitiesV1 tag_invoke(const json::value_to_tag<RuntimePromptCacheCapabilitiesV1>&, const json::value& value) {
    const Reader in(value, "RuntimePromptCacheCapabilities");
    return {.version = in.required<double>("version"), .providers = in.required<std::vector<RuntimePromptCacheCapabilitiesV1::Provider>>("providers")};
  }
}
