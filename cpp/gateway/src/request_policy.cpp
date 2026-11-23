// Port of src/request-policy.ts.
// Model metadata, prompt-cache, reasoning and warning policy for upstream requests, extracted from src/openai.ts.
#include "uos/request_policy.hpp"

#include "uos/http.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/provider/surplus_claude_request.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <tuple>
#include <utility>

namespace uos::request_policy {
  namespace {
    using defaults::ReasoningEffort;
    using models::metadata::ModelMetadataHint;

    /**
     * What the Cerebras route declares about its own models, per id. The two ids
     * differ in both context window and reasoning contract, so a single shared
     * hint cannot describe them.
     *
     * Facts below are from Cerebras' own catalog and reasoning pages (verified
     * 2026-09-26):
     * - Context window: 65,536 free-trial / 131,072 paid, for BOTH ids. Shared
     *   Inference bills these ids at paid tiers, so 131,072 is the window a request
     *   can actually use. Cerebras reports no context length over `/v1/models`.
     * - `gpt-oss-120b` accepts low/medium/high, defaults to `medium`, and cannot
     *   disable reasoning.
     * - `qwen-3.8-27b` additionally accepts `none` and defaults to `high`.
     *
     * One shared list would advertise a tier a model rejects (a promise the gateway
     * cannot keep) or hide `none` from a client that needs it.
     */
    constexpr double CEREBRAS_CONTEXT_WINDOW_TOKENS = 131'072;

    json::array stringArray(std::initializer_list<std::string_view> values) {
      json::array array;
      for (const std::string_view value : values) array.emplace_back(json::string(value));
      return array;
    }

    // `const CEREBRAS_PROVIDER_HINTS: Record<string, ModelMetadataHint>`, built from provider/cerebras.ts's ids:
    // constructed on first use.
    const rt::OrderedMap<std::string, ModelMetadataHint>& CEREBRAS_PROVIDER_HINTS() {
      static const rt::OrderedMap<std::string, ModelMetadataHint> hints{
        {std::string(provider::cerebras::CEREBRAS_GPT_OSS_120B_MODEL),
         ModelMetadataHint{
           .context_window_tokens = CEREBRAS_CONTEXT_WINDOW_TOKENS,
           .max_context_window_tokens = CEREBRAS_CONTEXT_WINDOW_TOKENS,
           .supported_reasoning_levels = stringArray({"low", "medium", "high"}),
           .default_reasoning_effort = json::value("medium"),
         }},
        {std::string(provider::cerebras::CEREBRAS_QWEN_3_8_27B_MODEL),
         ModelMetadataHint{
           .context_window_tokens = CEREBRAS_CONTEXT_WINDOW_TOKENS,
           .max_context_window_tokens = CEREBRAS_CONTEXT_WINDOW_TOKENS,
           .supported_reasoning_levels = stringArray({"none", "low", "medium", "high"}),
           .default_reasoning_effort = json::value("high"),
         }},
      };
      return hints;
    }

    // The own property names of Object.prototype in Bun. A plain object read (`HINTS[model]`) of one of them
    // yields the inherited function (or Object.prototype itself for `__proto__`), not undefined.
    bool isObjectPrototypeName(std::string_view key) {
      static constexpr std::string_view NAMES[]{
        "toString",         "toLocaleString",   "valueOf",          "hasOwnProperty", "propertyIsEnumerable", "isPrototypeOf",
        "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__", "__proto__",          "constructor",
      };
      return std::find(std::begin(NAMES), std::end(NAMES), key) != std::end(NAMES);
    }
  }

  /**
   * The hint for a Cerebras id. An unrecognized id falls back to the gpt-oss
   * contract, which is the route's original single-model declaration.
   */
  ModelMetadataHint cerebrasProviderHint(const std::string& model) {
    if (const ModelMetadataHint* hint = CEREBRAS_PROVIDER_HINTS().get(model)) return *hint;
    // The plain-object lookup resolves an Object.prototype name to the inherited value, which is not nullish:
    // every hint member reads undefined on it.
    if (isObjectPrototypeName(model)) return ModelMetadataHint{};
    return *CEREBRAS_PROVIDER_HINTS().get(provider::cerebras::CEREBRAS_GPT_OSS_120B_MODEL);
  }

  const ModelMetadataHint& LITHOS_PROVIDER_HINT() {
    static const ModelMetadataHint hint = [] {
      json::array levels;
      for (const std::string_view level : provider::lithos::LITHOS_REASONING_LEVELS) levels.emplace_back(json::string(level));
      return ModelMetadataHint{
        .context_window_tokens = provider::lithos::LITHOS_CONTEXT_WINDOW_TOKENS,
        .max_context_window_tokens = provider::lithos::LITHOS_CONTEXT_WINDOW_TOKENS,
        .effective_context_window_percent = provider::lithos::LITHOS_EFFECTIVE_CONTEXT_WINDOW_PERCENT,
        .supported_reasoning_levels = std::move(levels),
        .default_reasoning_effort = json::value(json::string(provider::lithos::LITHOS_DEFAULT_REASONING_EFFORT)),
      };
    }();
    return hint;
  }

  bool isTemporaryFreeSurplusModel(const std::string& model) { return model == TEMPORARY_FREE_SURPLUS_MODEL; }

  bool isAdditionalTrustedCodexModel(const std::string& model) {
    const std::string trimmed = rt::trim(model);
    return std::find(ADDITIONAL_TRUSTED_CODEX_MODEL_IDS.begin(), ADDITIONAL_TRUSTED_CODEX_MODEL_IDS.end(), std::string_view(trimmed)) !=
           ADDITIONAL_TRUSTED_CODEX_MODEL_IDS.end();
  }

  std::optional<std::string> modelIdFromSnapshotRecord(const json::object& model) {
    std::optional<std::string> id = utils::getString(rt::get(model, "slug"));
    if (!id) id = utils::getString(rt::get(model, "id"));
    if (!id) id = utils::getString(rt::get(model, "model"));
    if (!id) id = utils::getString(rt::get(model, "name"));
    if (!id) return std::nullopt;
    std::string trimmed = rt::trim(*id);
    if (trimmed.empty()) return std::nullopt;
    return trimmed;
  }

  namespace {
    // `snapshot: CodexModelsSnapshot | null`; `Record<string, unknown> | null`.
    std::optional<json::object> findSnapshotModelRecord(const std::optional<models::codex_models::CodexModelsSnapshot>& snapshot, const std::string& model) {
      const std::string target = rt::trim(model);
      if (target.empty()) return std::nullopt;
      if (!snapshot) return std::nullopt;
      const json::array* models = rt::getArray(*snapshot, "models");
      if (!models) return std::nullopt;
      for (const json::value& entry : *models) {
        // An array entry passes isRecord but has none of the id members, so only objects can match.
        if (!utils::isRecord(entry) || !entry.is_object()) continue;
        if (modelIdFromSnapshotRecord(entry.get_object()) == std::optional<std::string>(target)) return entry.get_object();
      }
      return std::nullopt;
    }

    // `value: unknown` (nullptr is undefined).
    std::optional<ReasoningEffort> normalizeSnapshotReasoningEffort(const json::value* value) {
      if (value && value->is_null()) return ReasoningEffort("none");
      return defaults::normalizeReasoningEffort(value);
    }

    std::vector<ReasoningEffort> extractSnapshotReasoningLevels(const std::optional<json::object>& model) {
      const json::array* raw = model ? rt::getArray(*model, "supported_reasoning_levels") : nullptr;
      std::vector<ReasoningEffort> levels;
      if (raw) {
        for (const json::value& entry : *raw) {
          std::optional<ReasoningEffort> level;
          if (entry.is_null() || entry.is_string()) level = normalizeSnapshotReasoningEffort(&entry);
          else if (utils::isRecord(entry)) level = normalizeSnapshotReasoningEffort(rt::get(entry, "effort"));
          if (!level || level->empty()) continue;
          if (std::find(levels.begin(), levels.end(), *level) == levels.end()) levels.push_back(std::move(*level));
        }
      }
      return levels;
    }

    rt::OrderedMap<ReasoningEffort, ReasoningEffort> extractSnapshotReasoningEffortWireMap(const std::optional<json::object>& model) {
      const json::value* raw = model ? rt::get(*model, "reasoning_effort_wire_map") : nullptr;
      rt::OrderedMap<ReasoningEffort, ReasoningEffort> wireEfforts;
      if (!utils::isRecord(raw)) return wireEfforts;
      // Object.entries: an array (isRecord accepts it) enumerates its indexes.
      std::vector<std::pair<std::string, const json::value*>> entries;
      if (raw->is_array()) {
        const json::array& array = raw->get_array();
        for (std::size_t index = 0; index < array.size(); index += 1) entries.emplace_back(rt::numberToString(static_cast<double>(index)), &array[index]);
      } else {
        for (const json::key_value_pair* entry : rt::objectEntries(raw->get_object())) entries.emplace_back(std::string(entry->key()), &entry->value());
      }
      for (const auto& [effort, wireEffort] : entries) {
        const json::value key = json::value(json::string(effort));
        std::optional<ReasoningEffort> normalizedEffort = defaults::normalizeReasoningEffort(key);
        std::optional<ReasoningEffort> normalizedWireEffort = defaults::normalizeReasoningEffort(wireEffort);
        if (!normalizedEffort || !normalizedWireEffort) continue;
        wireEfforts.set(std::move(*normalizedEffort), std::move(*normalizedWireEffort));
      }
      return wireEfforts;
    }
  }

  CodexModelReasoning getCodexModelReasoning(const std::optional<json::object>& record) {
    std::optional<ReasoningEffort> defaultLevel = normalizeSnapshotReasoningEffort(record ? rt::get(*record, "default_reasoning_level") : nullptr);
    const std::vector<ReasoningEffort> catalogLevels = extractSnapshotReasoningLevels(record);
    // Advertised tiers are preserved verbatim; `none` is never added, because the
    // upstream rejects an effort its catalog does not list.
    std::vector<ReasoningEffort> levels = catalogLevels;
    if (defaultLevel && !defaultLevel->empty() && std::find(levels.begin(), levels.end(), *defaultLevel) == levels.end()) levels.push_back(*defaultLevel);
    return CodexModelReasoning{
      .levels = std::move(levels),
      .defaultLevel = std::move(defaultLevel),
      .wireEfforts = extractSnapshotReasoningEffortWireMap(record),
    };
  }

  namespace {
    rt::Task<std::optional<provider::claude_code_models::ClaudeCodeModel>> findClaudeCodeModel(std::string model) {
      if (!provider::surplus_claude_request::isSurplusClaudeModel(std::string_view(model))) co_return std::nullopt;
      rt::Promise<std::optional<provider::selection::ProviderSelection>> selectionLoading(provider::selection::loadProviderSelectionCached());
      const std::optional<provider::selection::ProviderSelection> selection = co_await selectionLoading.wait();
      if (!provider::selection::isProviderEnabled("claude_code", selection)) co_return std::nullopt;
      rt::Promise<std::optional<provider::claude_code_models::ClaudeCodeModelsSnapshot>> snapshotLoading(provider::claude_code_models::fetchClaudeCodeModels());
      const std::optional<provider::claude_code_models::ClaudeCodeModelsSnapshot> snapshot = co_await snapshotLoading.wait();
      if (!snapshot) co_return std::nullopt;
      for (const provider::claude_code_models::ClaudeCodeModel& candidate : snapshot->models) {
        if (candidate.id == model) co_return candidate;
      }
      co_return std::nullopt;
    }

    json::array stringValues(const std::vector<std::string>& values) {
      json::array array;
      for (const std::string& value : values) array.emplace_back(json::string(value));
      return array;
    }
  }

  rt::Task<CodexModelMetadata> getCodexModelMetadata(std::string model, std::string route) {
    if (isTemporaryFreeSurplusModel(model)) {
      // This non-Codex routing record deliberately omits reasoning capability
      // claims. GLM preserves each caller-selected effort; no Surplus catalog
      // metadata currently authorizes the gateway to advertise a fixed list.
      json::object record;
      rt::objectDefine(record, "slug", json::string(TEMPORARY_FREE_SURPLUS_MODEL));
      CodexModelReasoning reasoning = getCodexModelReasoning(record);
      co_return CodexModelMetadata{
        .snapshot = std::nullopt,
        .record = std::move(record),
        .reasoning = std::move(reasoning),
        .supportedEndpoints = std::vector<std::string>{"openai", "openai-response"},
      };
    }
    rt::Promise<std::optional<models::codex_models::CodexModelsSnapshot>> snapshotLoading(models::codex_models_fetch::loadCodexModelsSnapshot());
    std::optional<models::codex_models::CodexModelsSnapshot> snapshot = co_await snapshotLoading.wait();
    std::optional<json::object> record = findSnapshotModelRecord(snapshot, model);
    if (record) {
      CodexModelReasoning reasoning = getCodexModelReasoning(record);
      co_return CodexModelMetadata{.snapshot = std::move(snapshot), .record = std::move(record), .reasoning = std::move(reasoning), .supportedEndpoints = std::nullopt};
    }
    rt::Promise<std::optional<provider::claude_code_models::ClaudeCodeModel>> claudeLookup(findClaudeCodeModel(model));
    const std::optional<provider::claude_code_models::ClaudeCodeModel> claudeModel = co_await claudeLookup.wait();
    if (claudeModel) {
      // The native subscription adapter serves both wires. A same-id paid row
      // cannot remove that route, but only this credential's observed IDs add it.
      json::object subscriptionRecord;
      rt::objectDefine(subscriptionRecord, "slug", json::string(model));
      rt::objectDefine(subscriptionRecord, "supported_reasoning_levels", stringValues(provider::claude_code_models::claudeCodeReasoningLevels(*claudeModel)));
      if (claudeModel->supports_disabled_thinking == true) rt::objectDefine(subscriptionRecord, "default_reasoning_level", json::string("none"));
      CodexModelReasoning reasoning = getCodexModelReasoning(subscriptionRecord);
      co_return CodexModelMetadata{
        .snapshot = std::move(snapshot),
        .record = std::move(subscriptionRecord),
        .reasoning = std::move(reasoning),
        .supportedEndpoints = std::vector<std::string>{"openai", "openai-response"},
      };
    }
    rt::Promise<std::optional<provider::metered::MeteredModelsSnapshot>> meteredLoading(provider::metered::fetchMeteredModels());
    rt::Promise<std::optional<provider::surplus::SurplusModelsSnapshot>> surplusLoading(provider::surplus::fetchSurplusModels());
    auto both = rt::all(meteredLoading, surplusLoading);
    const auto loaded = co_await both.wait();
    const std::optional<provider::metered::MeteredModelsSnapshot>& metered = std::get<0>(loaded);
    const std::optional<provider::surplus::SurplusModelsSnapshot>& surplus = std::get<1>(loaded);
    const provider::metered::MeteredModel* meteredRecord = nullptr;
    if (metered) {
      for (const provider::metered::MeteredModel& candidate : metered->models) {
        if (candidate.id == model) {
          meteredRecord = &candidate;
          break;
        }
      }
    }
    const provider::surplus::SurplusModel* surplusRecord = nullptr;
    if (surplus) {
      for (const provider::surplus::SurplusModel& candidate : surplus->models) {
        if (candidate.id == model) {
          surplusRecord = &candidate;
          break;
        }
      }
    }
    const std::string endpointType = route == "responses" ? "openai-response" : "openai";
    const auto servesEndpoint = [&](const std::vector<std::string>& endpoints) {
      return std::find(endpoints.begin(), endpoints.end(), endpointType) != endpoints.end();
    };
    // `[meteredRecord, surplusRecord].find(...)`: which of the two records (by position) serves the route.
    const bool routeIsMetered = meteredRecord && servesEndpoint(meteredRecord->supported_endpoint_types);
    const bool routeIsSurplus = !routeIsMetered && surplusRecord && servesEndpoint(surplusRecord->supported_endpoint_types);
    // `routeRecord ?? meteredRecord ?? surplusRecord`
    const std::vector<std::string>* paidEndpoints = nullptr;
    std::string paidId;
    if (routeIsMetered || (!routeIsSurplus && meteredRecord)) {
      paidEndpoints = &meteredRecord->supported_endpoint_types;
      paidId = meteredRecord->id;
    } else if (surplusRecord) {
      paidEndpoints = &surplusRecord->supported_endpoint_types;
      paidId = surplusRecord->id;
    }
    if (paidEndpoints) {
      // `routeRecord === surplusRecord && surplusRecord`: the route record is the Surplus one.
      const std::string routeProvider = routeIsSurplus ? "surplus" : "metered";
      std::optional<double> routeUpdatedAtMs;
      if (routeProvider == "surplus") {
        if (surplus) routeUpdatedAtMs = surplus->updated_at_ms;
      } else if (metered) {
        routeUpdatedAtMs = metered->updated_at_ms;
      }
      models::codex_models::CodexModelsSnapshot routeSnapshot;
      if (snapshot) {
        routeSnapshot = std::move(*snapshot);
      } else {
        rt::objectDefine(routeSnapshot, "models", json::array());
        rt::objectDefine(routeSnapshot, "source", json::string(routeProvider));
        rt::objectDefine(routeSnapshot, "updated_at_ms", rt::makeNumber(routeUpdatedAtMs ? *routeUpdatedAtMs : rt::dateNow()));
      }
      json::object paidRecord;
      rt::objectDefine(paidRecord, "slug", json::string(paidId));
      rt::objectDefine(paidRecord, "supported_reasoning_levels", stringArray({"none"}));
      rt::objectDefine(paidRecord, "default_reasoning_level", json::string("none"));
      json::object paidReasoningRecord;
      rt::objectDefine(paidReasoningRecord, "supported_reasoning_levels", stringArray({"none"}));
      rt::objectDefine(paidReasoningRecord, "default_reasoning_level", json::string("none"));
      CodexModelReasoning reasoning = getCodexModelReasoning(paidReasoningRecord);
      std::vector<std::string> supportedEndpoints = *paidEndpoints;
      co_return CodexModelMetadata{
        .snapshot = std::move(routeSnapshot),
        .record = std::move(paidRecord),
        .reasoning = std::move(reasoning),
        .supportedEndpoints = std::move(supportedEndpoints),
      };
    }
    CodexModelReasoning reasoning = getCodexModelReasoning(std::nullopt);
    co_return CodexModelMetadata{.snapshot = std::move(snapshot), .record = std::nullopt, .reasoning = std::move(reasoning), .supportedEndpoints = std::nullopt};
  }

  namespace {
    http::OpenaiErrorOptions paramOption(const std::string& param) {
      http::OpenaiErrorOptions options;
      options.param = std::optional<std::string>(param);
      return options;
    }

    // `snapshotModels?.length` read as a condition: the length of an array or string, an object's own
    // `length` member, nothing for the other values.
    bool lengthIsTruthy(const json::value* value) {
      if (!value) return false;
      if (value->is_array()) return !value->get_array().empty();
      if (value->is_string()) return rt::length(value->get_string()) > 0;
      if (value->is_object()) return rt::truthy(rt::get(value->get_object(), "length"));
      return false;
    }
  }

  std::optional<rt::Response> validateCodexModelAvailable(const std::string& model, const std::string& route, const CodexModelMetadata& metadata) {
    if (metadata.supportedEndpoints) {
      const std::string endpoint = route == "responses" ? "openai-response" : "openai";
      if (std::find(metadata.supportedEndpoints->begin(), metadata.supportedEndpoints->end(), endpoint) == metadata.supportedEndpoints->end()) {
        return http::openaiError(404, "The model '" + model + "' does not support " + route + ". Use /v1/models for supported models.", "model_not_found",
                                 paramOption("model"));
      }
    }
    // The snapshot is read back from KV, so keep the runtime guard on `models`
    // even though the declared type always presents it.
    const json::value* snapshotModels = metadata.snapshot ? rt::get(*metadata.snapshot, "models") : nullptr;
    if (!lengthIsTruthy(snapshotModels) || metadata.record) return std::nullopt;
    // An owner-authorized id stays servable while the published catalog lags; it
    // is dispatched to the Codex transport under the requested id, and no catalog
    // record is invented for it.
    if (isAdditionalTrustedCodexModel(model)) return std::nullopt;
    return http::openaiError(404, "The model '" + model + "' does not exist or is not available through this gateway. Use /v1/models for supported models.",
                             "model_not_found", paramOption("model"));
  }

  namespace {
    std::optional<std::string> promptCacheControlParam(const json::object& rawRecord) {
      for (const std::string_view key : {"prompt_cache_key", "prompt_cache_options", "prompt_cache_retention"}) {
        if (rt::hasOwn(rawRecord, key)) return std::string(key);
      }
      return std::nullopt;
    }

    bool hasExplicitPromptCacheBreakpoint(const json::value* value) {
      return utils::isRecord(value) && !value->is_array() && rt::getString(*value, "mode") == std::optional<std::string_view>("explicit");
    }

    struct ExplicitPromptCacheBreakpoint {
      std::string param;
      std::optional<std::string> blockType;
    };

    void collectExplicitPromptCacheBreakpoint(const json::object& value, const std::string& param, std::vector<ExplicitPromptCacheBreakpoint>& breakpoints) {
      if (!hasExplicitPromptCacheBreakpoint(rt::get(value, "prompt_cache_breakpoint"))) return;
      breakpoints.push_back({.param = param, .blockType = utils::getString(rt::get(value, "type"))});
    }

    std::string indexText(std::size_t index) { return rt::numberToString(static_cast<double>(index)); }

    void collectIndexedExplicitPromptCacheBreakpoints(const json::array& values, const std::string& paramPrefix, std::vector<ExplicitPromptCacheBreakpoint>& breakpoints) {
      for (std::size_t index = 0; index < values.size(); index += 1) {
        const json::value& value = values[index];
        if (!utils::isRecord(value) || value.is_array()) continue;
        collectExplicitPromptCacheBreakpoint(value.get_object(), paramPrefix + "[" + indexText(index) + "].prompt_cache_breakpoint", breakpoints);
      }
    }

    // `inputParam: "input" | "messages"`.
    std::vector<ExplicitPromptCacheBreakpoint> findExplicitPromptCacheBreakpoints(const json::value* rawInput, const std::string& inputParam) {
      if (!rawInput || !rawInput->is_array()) return {};

      std::vector<ExplicitPromptCacheBreakpoint> breakpoints;

      const json::array& items = rawInput->get_array();
      for (std::size_t index = 0; index < items.size(); index += 1) {
        const json::value& itemValue = items[index];
        if (!utils::isRecord(itemValue) || itemValue.is_array()) continue;
        const json::object& item = itemValue.get_object();
        const std::string itemParam = inputParam + "[" + indexText(index) + "]";
        if (inputParam == "input") {
          collectExplicitPromptCacheBreakpoint(item, itemParam + ".prompt_cache_breakpoint", breakpoints);
        }
        if (const json::array* content = rt::getArray(item, "content")) {
          collectIndexedExplicitPromptCacheBreakpoints(*content, itemParam + ".content", breakpoints);
        }
        const json::array* output = rt::getArray(item, "output");
        if (inputParam != "input" || rt::getString(item, "type") != std::optional<std::string_view>("function_call_output") || !output) continue;
        collectIndexedExplicitPromptCacheBreakpoints(*output, itemParam + ".output", breakpoints);
      }
      return breakpoints;
    }

    std::optional<models::codex_models::PromptCacheControls> activePromptCacheControls(const CodexModelMetadata& metadata) {
      const std::optional<models::codex_models::PromptCacheCapabilities> capabilities =
        models::codex_models::normalizePromptCacheCapabilities(metadata.record ? rt::get(*metadata.record, "prompt_cache") : nullptr);
      if (!capabilities || std::holds_alternative<models::codex_models::PromptCacheCapabilitiesUnsupported>(*capabilities)) return std::nullopt;
      for (const models::codex_models::PromptCacheProvider& provider : std::get<models::codex_models::PromptCacheCapabilitiesV1>(*capabilities).providers) {
        if (provider.id == models::codex_models::CODEX_CHATGPT_PROMPT_CACHE_PROVIDER) return provider.controls;
      }
      return std::nullopt;
    }

    struct RequestedPromptCacheMode {
      std::string value;  // "implicit" | "explicit"
      std::string param;  // "prompt_cache_options" | "prompt_cache_options.mode"
    };

    std::optional<RequestedPromptCacheMode> requestedPromptCacheMode(const json::object& rawRecord) {
      if (!rt::hasOwn(rawRecord, "prompt_cache_options")) return std::nullopt;
      const json::value* options = rt::get(rawRecord, "prompt_cache_options");
      if (!utils::isRecord(options) || options->is_array()) return std::nullopt;
      const std::optional<std::string_view> mode = rt::getString(*options, "mode");
      if (mode == std::optional<std::string_view>("explicit")) return RequestedPromptCacheMode{.value = "explicit", .param = "prompt_cache_options.mode"};
      return RequestedPromptCacheMode{
        .value = "implicit",
        .param = mode == std::optional<std::string_view>("implicit") ? "prompt_cache_options.mode" : "prompt_cache_options",
      };
    }

    std::optional<std::string> requestedPromptCacheTtl(const json::object& rawRecord) {
      const json::value* options = rt::get(rawRecord, "prompt_cache_options");
      if (!utils::isRecord(options) || options->is_array()) return std::nullopt;
      return utils::getString(rt::get(*options, "ttl"));
    }

    rt::Response knownUnsupportedPromptCacheUseError(const std::string& model, const std::string& param) {
      return http::openaiError(400, "Prompt cache control '" + param + "' is not supported for model '" + model + "'.", "invalid_request_error", paramOption(param));
    }

    bool includes(const std::vector<std::string>& values, const std::string& value) { return std::find(values.begin(), values.end(), value) != values.end(); }

    // `value: "implicit" | "explicit"`.
    bool promptCacheControlModeIsKnownUnsupported(const models::codex_models::PromptCacheControls& controls, const std::string& value) {
      return controls.modes.has_value() && !includes(*controls.modes, value);
    }

    std::optional<rt::Response> validatePromptCacheControlKey(const std::string& model, const models::codex_models::PromptCacheControls& controls,
                                                              const json::object& rawRecord) {
      if (rt::hasOwn(rawRecord, "prompt_cache_key") && controls.key == false) {
        return knownUnsupportedPromptCacheUseError(model, "prompt_cache_key");
      }
      return std::nullopt;
    }

    std::optional<rt::Response> validatePromptCacheControlOptions(const std::string& model, const models::codex_models::PromptCacheControls& controls,
                                                                  const json::object& rawRecord) {
      const std::optional<std::string> ttl = requestedPromptCacheTtl(rawRecord);
      if (ttl && controls.ttls && !includes(*controls.ttls, *ttl)) {
        return knownUnsupportedPromptCacheUseError(model, "prompt_cache_options.ttl");
      }

      const std::optional<std::string> retention = utils::getString(rt::get(rawRecord, "prompt_cache_retention"));
      if (retention && controls.legacy_retentions && !includes(*controls.legacy_retentions, *retention)) {
        return knownUnsupportedPromptCacheUseError(model, "prompt_cache_retention");
      }
      return std::nullopt;
    }

    std::optional<rt::Response> validatePromptCacheControlBreakpoints(const std::string& model, const models::codex_models::PromptCacheControls& controls,
                                                                      const std::string& inputParam,
                                                                      const std::vector<ExplicitPromptCacheBreakpoint>& breakpoints) {
      for (const ExplicitPromptCacheBreakpoint& breakpoint : breakpoints) {
        if (controls.explicit_breakpoints == false || promptCacheControlModeIsKnownUnsupported(controls, "explicit")) {
          return knownUnsupportedPromptCacheUseError(model, breakpoint.param);
        }
        const bool responsesEndpoint = inputParam == "input";
        const std::optional<std::vector<std::string>>* supportedBlockTypes = nullptr;
        if (controls.breakpoint_block_types) {
          supportedBlockTypes = responsesEndpoint ? &controls.breakpoint_block_types->responses : &controls.breakpoint_block_types->chat_completions;
        }
        if (supportedBlockTypes && supportedBlockTypes->has_value() && (!breakpoint.blockType || !includes(**supportedBlockTypes, *breakpoint.blockType))) {
          return knownUnsupportedPromptCacheUseError(model, breakpoint.param);
        }
      }
      return std::nullopt;
    }
  }

  std::optional<rt::Response> validateKnownUnsupportedPromptCacheUse(const std::string& model, const CodexModelMetadata& metadata, const json::object& rawRecord,
                                                                     const std::vector<::uos::types::ResponseInputItem>& input, const std::string& inputParam) {
    const std::vector<ExplicitPromptCacheBreakpoint> breakpoints = openai_telemetry::countExplicitPromptCacheBreakpoints(input) > 0
                                                                     ? findExplicitPromptCacheBreakpoints(rt::get(rawRecord, inputParam), inputParam)
                                                                     : std::vector<ExplicitPromptCacheBreakpoint>{};

    if (metadata.record && rt::getBool(*metadata.record, "prompt_cache") == std::optional<bool>(false)) {
      std::optional<std::string> param = promptCacheControlParam(rawRecord);
      if (!param && !breakpoints.empty()) param = breakpoints[0].param;
      if (!param || param->empty()) return std::nullopt;
      return http::openaiError(400, "Prompt caching is not supported for model '" + model + "'.", "invalid_request_error", paramOption(*param));
    }

    // A missing capability envelope, another provider's record, or an omitted
    // control field is unknown—not an unsupported upstream feature. Preserve
    // standard OpenAI controls in each of those cases for forward compatibility.
    const std::optional<models::codex_models::PromptCacheControls> controls = activePromptCacheControls(metadata);
    if (!controls) return std::nullopt;

    std::optional<rt::Response> keyError = validatePromptCacheControlKey(model, *controls, rawRecord);
    if (keyError) return keyError;

    const std::optional<RequestedPromptCacheMode> mode = requestedPromptCacheMode(rawRecord);
    if (mode && mode->value == "implicit" && (controls->implicit == false || promptCacheControlModeIsKnownUnsupported(*controls, "implicit"))) {
      return knownUnsupportedPromptCacheUseError(model, mode->param);
    }

    std::optional<rt::Response> optionsError = validatePromptCacheControlOptions(model, *controls, rawRecord);
    if (optionsError) return optionsError;

    std::optional<rt::Response> breakpointsError = validatePromptCacheControlBreakpoints(model, *controls, inputParam, breakpoints);
    if (breakpointsError) return breakpointsError;

    if (mode && mode->value == "explicit" && (controls->explicit_breakpoints == false || promptCacheControlModeIsKnownUnsupported(*controls, "explicit"))) {
      return knownUnsupportedPromptCacheUseError(model, mode->param);
    }

    return std::nullopt;
  }

  defaults::ReasoningEffort resolveDefaultReasoningLabel([[maybe_unused]] const CodexModelReasoning& _modelReasoning, const defaults::ReasoningEffort& defaultEffort) {
    return defaultEffort;
  }

  defaults::ReasoningEffort resolveReasoningLabelFromEffort(const std::optional<defaults::ReasoningEffort>& effort, const defaults::ReasoningEffort& defaultLabel) {
    if (!effort) return defaultLabel;
    return *effort;
  }

  defaults::ReasoningEffort resolveReasoningLabelFromParam(const std::optional<json::object>& reasoning, const defaults::ReasoningEffort& defaultLabel) {
    if (!reasoning) return defaultLabel;
    if (rt::hasOwn(*reasoning, "effort")) {
      std::optional<ReasoningEffort> effort = defaults::normalizeReasoningEffort(rt::get(*reasoning, "effort"));
      if (effort && !effort->empty()) return *effort;
    }
    return defaultLabel;
  }

  namespace {
    std::optional<ReasoningEffort> extractReasoningParamEffort(const std::optional<json::object>& reasoning) {
      if (!reasoning) return std::nullopt;
      if (!rt::hasOwn(*reasoning, "effort")) return std::nullopt;
      return defaults::normalizeReasoningEffort(rt::get(*reasoning, "effort"));
    }
  }

  defaults::ReasoningEffort reasoningEffortForCodexRequest(const defaults::ReasoningEffort& effort, const CodexModelReasoning& modelReasoning) {
    if (effort == "none") return "none";
    // Codex CLI's advanced `ultra` preset is client-side orchestration and
    // always uses `max` on the upstream wire, even for an older catalog that
    // has not yet published its wire map.
    if (effort == "ultra") return "max";
    if (const ReasoningEffort* wire = modelReasoning.wireEfforts.get(effort)) return *wire;
    return effort;
  }

  std::optional<json::object> normalizeReasoningParamForCodex(const std::optional<json::object>& reasoning, const CodexModelReasoning& modelReasoning) {
    if (!reasoning) return std::nullopt;
    const std::optional<ReasoningEffort> effort = extractReasoningParamEffort(reasoning);
    if (!effort) return reasoning;
    json::object normalized = rt::objectSpread(*reasoning);
    rt::objectDefine(normalized, "effort", json::string(reasoningEffortForCodexRequest(*effort, modelReasoning)));
    return normalized;
  }

  std::vector<std::string> buildIgnoredWarnings(const json::object& record, const rt::OrderedSet<std::string>& usedKeys) {
    std::vector<std::string> warnings;
    for (const std::string& key : rt::objectKeys(record)) {
      if (usedKeys.has(key)) continue;
      const std::string* known = WARNING_KEY_MAP.get(key);
      std::string mapped = known ? *known : key + "_ignored";
      if (std::find(warnings.begin(), warnings.end(), mapped) == warnings.end()) warnings.push_back(std::move(mapped));
    }
    return warnings;
  }

  std::vector<std::string> responseWarnings(const rt::Response& response) {
    const std::string header = response.headers().get(UOS_WARNING_HEADER).value_or("");
    std::vector<std::string> warnings;
    for (const std::string& value : rt::split(header, ",")) {
      std::string trimmed = rt::trim(value);
      if (!trimmed.empty()) warnings.push_back(std::move(trimmed));
    }
    return warnings;
  }

  namespace {
    json::value normalizeCodexToolChoice(const json::value& value) {
      if (!utils::isRecord(value)) return value;
      if (utils::getString(rt::get(value, "type")) != std::optional<std::string>("function")) return value;

      json::object normalized = rt::objectSpread(value.get_object());
      const std::optional<std::string> topLevelName = utils::getString(rt::get(normalized, "name"));
      const json::value* function = rt::get(value, "function");
      const json::value* fn = utils::isRecord(function) ? function : nullptr;
      if (!fn && !rt::truthy(topLevelName)) return value;

      if (!rt::truthy(topLevelName)) {
        const std::optional<std::string> functionName = utils::getString(fn ? rt::get(*fn, "name") : nullptr);
        if (!rt::truthy(functionName)) return value;
        rt::objectSet(normalized, "name", json::string(*functionName));
      }

      rt::objectDelete(normalized, "function");
      return normalized;
    }

    json::value normalizeCodexTools(const json::value& value) {
      if (!value.is_array()) return value;
      json::array mapped;
      for (const json::value& tool : value.get_array()) {
        mapped.push_back([&]() -> json::value {
          if (!utils::isRecord(tool)) return tool;
          if (utils::getString(rt::get(tool, "type")) != std::optional<std::string>("function")) return tool;
          const json::value* function = rt::get(tool, "function");
          const json::value* nestedFunction = utils::isRecord(function) ? function : nullptr;
          if (!nestedFunction) return tool;

          json::object normalized = rt::objectSpread(tool.get_object());
          const std::optional<std::string> topLevelName = utils::getString(rt::get(normalized, "name"));
          const std::optional<std::string> nestedName = utils::getString(rt::get(*nestedFunction, "name"));
          if (!rt::truthy(topLevelName) && !rt::truthy(nestedName)) return tool;

          if (!rt::truthy(topLevelName)) {
            rt::objectSet(normalized, "name", json::string(*nestedName));
          }
          // Object.entries: an array (isRecord accepts it) enumerates its indexes.
          std::vector<std::pair<std::string, json::value>> entries;
          if (nestedFunction->is_array()) {
            const json::array& array = nestedFunction->get_array();
            for (std::size_t index = 0; index < array.size(); index += 1) entries.emplace_back(indexText(index), array[index]);
          } else {
            for (const json::key_value_pair* entry : rt::objectEntries(nestedFunction->get_object())) entries.emplace_back(std::string(entry->key()), entry->value());
          }
          for (auto& [key, nestedValue] : entries) {
            // `key in normalized` also sees the names Object.prototype provides.
            if (rt::hasOwn(normalized, key) || isObjectPrototypeName(key)) continue;
            rt::objectSet(normalized, key, std::move(nestedValue));
          }
          rt::objectDelete(normalized, "function");
          return normalized;
        }());
      }
      return mapped;
    }

    json::value normalizePassthroughForCodex(const PassthroughToolSchemaKey& key, const json::value& value) {
      if (key == "tools") return normalizeCodexTools(value);
      if (key == "tool_choice") return normalizeCodexToolChoice(value);
      return value;
    }
  }

  void applyPassthroughToCodexRequest(json::object& codexBody, const json::object& rawRecord, const std::vector<PassthroughToolSchemaKey>& keys) {
    for (const PassthroughToolSchemaKey& key : keys) {
      if (rt::hasOwn(rawRecord, key)) {
        json::value normalized = normalizePassthroughForCodex(key, *rt::get(rawRecord, key));
        rt::objectSet(codexBody, key, std::move(normalized));
      }
    }
  }

  rt::Response withUosWarning(rt::Response response, const std::vector<std::string>& warnings) {
    std::vector<std::string> combined = responseWarnings(response);
    combined.insert(combined.end(), warnings.begin(), warnings.end());
    std::vector<std::string> merged;
    for (const std::string& warning : combined) {
      if (std::find(merged.begin(), merged.end(), warning) == merged.end()) merged.push_back(warning);
    }
    if (merged.empty()) return response;
    rt::Headers headers(response.headers());
    headers.set(UOS_WARNING_HEADER, rt::join(merged, ", "));
    return rt::Response(rt::bodyInit(response.body()), rt::ResponseInit{
                                                         .status = static_cast<double>(response.status()),
                                                         .statusText = response.statusText(),
                                                         .headers = std::move(headers),
                                                       });
  }

  ParseReasoningEffortFieldResult parseReasoningEffortField(const json::value* value, const std::string& fieldName) {
    if (!value || value->is_null()) return ParseReasoningEffortFieldOk{.value = std::nullopt};
    if (!value->is_string()) {
      return ParseFieldNotOk{.message = fieldName + " must be a string"};
    }
    std::optional<ReasoningEffort> normalized = defaults::normalizeReasoningEffort(*value);
    if (!normalized || normalized->empty()) return ParseFieldNotOk{.message = fieldName + " must be a non-empty string"};
    return ParseReasoningEffortFieldOk{.value = std::move(normalized)};
  }

  ParseReasoningEffortFieldResult parseReasoningEffortField(const json::value& value, const std::string& fieldName) { return parseReasoningEffortField(&value, fieldName); }

  namespace {
    std::optional<ParseFieldNotOk> normalizeReasoningSummaryField(json::object& record) {
      if (!rt::hasOwn(record, "summary")) return std::nullopt;
      const json::value* summary = rt::get(record, "summary");
      if (summary->is_null()) rt::objectDelete(record, "summary");
      else if (!summary->is_string()) {
        return ParseFieldNotOk{.message = "reasoning.summary must be a string"};
      }
      return std::nullopt;
    }

    std::optional<ParseFieldNotOk> normalizeReasoningGenerateSummaryField(json::object& record) {
      if (!rt::hasOwn(record, "generate_summary")) return std::nullopt;
      const json::value* generateSummary = rt::get(record, "generate_summary");
      if (generateSummary->is_null()) rt::objectDelete(record, "generate_summary");
      else if (!generateSummary->is_string()) {
        return ParseFieldNotOk{.message = "reasoning.generate_summary must be a string"};
      }
      return std::nullopt;
    }
  }

  ParseReasoningParamResult parseReasoningParam(const json::value* value) {
    if (!value || value->is_null()) return ParseReasoningParamOk{.value = std::nullopt};
    if (!utils::isRecord(value) || value->is_array()) return ParseFieldNotOk{.message = "reasoning must be an object"};
    json::object normalized = rt::objectSpread(value->get_object());
    if (rt::hasOwn(normalized, "effort")) {
      ParseReasoningEffortFieldResult effort = parseReasoningEffortField(rt::get(normalized, "effort"), "reasoning.effort");
      if (const ParseFieldNotOk* failure = std::get_if<ParseFieldNotOk>(&effort)) return *failure;
      const ParseReasoningEffortFieldOk& parsed = std::get<ParseReasoningEffortFieldOk>(effort);
      if (!parsed.value) rt::objectDelete(normalized, "effort");
      else rt::objectSet(normalized, "effort", json::string(*parsed.value));
    }
    if (std::optional<ParseFieldNotOk> summaryError = normalizeReasoningSummaryField(normalized)) return *summaryError;
    if (std::optional<ParseFieldNotOk> generateSummaryError = normalizeReasoningGenerateSummaryField(normalized)) return *generateSummaryError;

    return ParseReasoningParamOk{.value = !normalized.empty() ? std::optional<json::object>(std::move(normalized)) : std::nullopt};
  }

  ParseReasoningParamResult parseReasoningParam(const json::value& value) { return parseReasoningParam(&value); }

  ParseStreamFieldResult parseStreamField(const json::value* value) {
    if (!value || (value->is_bool() && !value->get_bool())) return ParseStreamFieldOk{.value = false};
    if (value->is_bool() && value->get_bool()) return ParseStreamFieldOk{.value = true};
    return ParseFieldNotOk{.message = "stream must be a boolean"};
  }

  ParseStreamFieldResult parseStreamField(const json::value& value) { return parseStreamField(&value); }

  ParseChatStreamOptionsResult parseChatStreamOptions(const json::value* value) {
    if (!value) return ParseChatStreamOptionsOk{.includeUsage = false};
    if (!utils::isRecord(value) || value->is_array()) {
      return ParseFieldNotOk{.message = "stream_options must be an object"};
    }
    const json::value* includeUsage = rt::get(*value, "include_usage");
    if (includeUsage && !includeUsage->is_bool()) {
      return ParseFieldNotOk{.message = "stream_options.include_usage must be a boolean"};
    }
    return ParseChatStreamOptionsOk{.includeUsage = includeUsage && includeUsage->get_bool()};
  }

  ParseChatStreamOptionsResult parseChatStreamOptions(const json::value& value) { return parseChatStreamOptions(&value); }

  ParseMaxCompletionTokensFieldResult parseMaxCompletionTokensField(const json::value* value) {
    if (!value) return ParseMaxCompletionTokensFieldOk{.value = std::nullopt};
    const std::optional<double> number = rt::getNumber(value);
    if (!number || !rt::Number::isSafeInteger(*number) || *number <= 0) {
      return ParseFieldNotOk{.message = "max_completion_tokens must be a positive integer"};
    }
    return ParseMaxCompletionTokensFieldOk{.value = number};
  }

  ParseMaxCompletionTokensFieldResult parseMaxCompletionTokensField(const json::value& value) { return parseMaxCompletionTokensField(&value); }
}
