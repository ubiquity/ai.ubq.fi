#include "uos/runtime_config.hpp"
#include "uos/rt/memory.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"

#include <exception>
#include <unordered_set>
#include <utility>

namespace uos::runtime_config {
  namespace {
    constexpr rt::StoredJsonViewOptions STORED_JSON_VIEW_OPTIONS{
      .undefinedMember = rt::StoredJsonViewOptions::UndefinedView::emptyObject,
      .undefinedElement = rt::StoredJsonViewOptions::UndefinedView::emptyObject,
      .hole = rt::StoredJsonViewOptions::UndefinedView::emptyObject,
      .topLevelUndefined = rt::StoredJsonViewOptions::UndefinedView::emptyObject,
      .bigint = rt::StoredJsonViewOptions::BigIntView::emptyObject,
      .kvU64Members = false,
      .typedArrayMembers = false,
    };
    using defaults::ReasoningEffort;
    using models::codex_models::CodexModelsSnapshot;
    using models::codex_models::RuntimePromptCacheCapabilities;
    using rt::record::Reader;
    using rt::record::Writer;

    // `Readonly<{ value: RuntimeConfigV2; expires_at_ms: number }>`
    struct CachedRuntimeConfig {
      RuntimeConfigV2 value;
      double expires_at_ms;
    };

    std::optional<CachedRuntimeConfig>& cachedRuntimeConfig() {
      static std::optional<CachedRuntimeConfig> value;
      return value;
    }

    void setCachedRuntimeConfig(CachedRuntimeConfig cached) {
      const auto& config = cached.value;
      const auto size = sizeof(CachedRuntimeConfig) + rt::memory::bytes(toJson(config));
      cachedRuntimeConfig() = std::move(cached);
      rt::memory::setCache(rt::memory::Cache::runtime_config, 1, size);
    }

    std::optional<rt::Promise<std::optional<RuntimeConfigV2>>>& runtimeConfigLoadInFlight() {
      static std::optional<rt::Promise<std::optional<RuntimeConfigV2>>> value;
      return value;
    }

    double serializedSize(const json::value& value) { return static_cast<double>(rt::utf8ByteLength(rt::jsonStringify(value))); }

    // `value: unknown`: nullptr is undefined.
    std::optional<ReasoningEffort> reasoningLevel(const json::value* value) {
      if (value != nullptr && value->is_null()) return ReasoningEffort("none");
      if (value != nullptr && value->is_string()) return defaults::normalizeReasoningEffort(*value);
      if (!utils::isRecord(value)) return std::nullopt;
      const json::value* effort = rt::get(*value, "effort");
      if (effort != nullptr && effort->is_null()) return ReasoningEffort("none");
      return defaults::normalizeReasoningEffort(effort);
    }

    std::vector<ReasoningEffort> compactReasoningLevels(const json::value& model) {
      // Verbatim advertised tiers only: a null entry is the upstream's own `none`, and
      // no tier is added because the endpoint rejects an effort its catalog omits.
      std::vector<ReasoningEffort> levels;
      if (const json::array* advertised = rt::getArray(rt::get(model, "supported_reasoning_levels"))) {
        for (const json::value& entry : *advertised) {
          if (std::optional<ReasoningEffort> level = reasoningLevel(&entry)) levels.push_back(std::move(*level));
        }
      }
      // Array.from(new Set(levels))
      std::vector<ReasoningEffort> unique;
      rt::OrderedSet<std::string> seen;
      for (ReasoningEffort& level : levels) {
        if (seen.has(level)) continue;
        seen.add(level);
        unique.push_back(std::move(level));
      }
      return unique;
    }

    // Object.entries(record): an array enumerates its indexes.
    std::vector<std::pair<std::string, const json::value*>> ownEntries(const json::value& record) {
      std::vector<std::pair<std::string, const json::value*>> entries;
      if (const json::object* object = record.if_object()) {
        for (const json::key_value_pair* entry : rt::objectEntries(*object)) entries.emplace_back(std::string(entry->key()), &entry->value());
      } else if (const json::array* array = record.if_array()) {
        for (std::size_t index = 0; index < array->size(); ++index) entries.emplace_back(rt::numberToString(static_cast<double>(index)), &(*array)[index]);
      }
      return entries;
    }

    // `Readonly<Record<string, ReasoningEffort>> | undefined`
    std::optional<json::object> compactReasoningWireMap(const json::value& model, const std::vector<ReasoningEffort>& levels) {
      rt::OrderedMap<std::string, ReasoningEffort> entries;
      const json::value* wireMap = rt::get(model, "reasoning_effort_wire_map");
      if (utils::isRecord(wireMap)) {
        for (const auto& [effortRaw, wireRaw] : ownEntries(*wireMap)) {
          const std::optional<ReasoningEffort> effort = defaults::normalizeReasoningEffort(json::value(json::string(effortRaw)));
          const std::optional<ReasoningEffort> wireEffort = defaults::normalizeReasoningEffort(wireRaw);
          if (effort && wireEffort) entries.set(*effort, *wireEffort);
        }
      }
      if (rt::includes(levels, ReasoningEffort("ultra"))) entries.set("ultra", "max");
      if (entries.size() == 0) return std::nullopt;
      std::vector<std::pair<std::string, json::value>> pairs;
      for (const auto& [effort, wireEffort] : entries) pairs.emplace_back(effort, json::value(json::string(wireEffort)));
      return rt::objectFromEntries(pairs);
    }

    // `value: Record<string, unknown>` (an array is a record too).
    std::optional<RuntimeCodexModel> compactRuntimeCodexModel(const json::value& value) {
      std::optional<std::string> id = utils::getString(rt::get(value, "slug"));
      if (!id) id = utils::getString(rt::get(value, "id"));
      if (!id) id = utils::getString(rt::get(value, "model"));
      if (!id) id = utils::getString(rt::get(value, "name"));
      const std::string slug = id ? rt::trim(*id) : std::string();
      if (slug.empty()) return std::nullopt;
      std::vector<ReasoningEffort> supportedReasoningLevels = compactReasoningLevels(value);
      const std::optional<ReasoningEffort> defaultReasoningLevel = reasoningLevel(rt::get(value, "default_reasoning_level"));
      if (defaultReasoningLevel && !rt::includes(supportedReasoningLevels, *defaultReasoningLevel)) {
        supportedReasoningLevels.push_back(*defaultReasoningLevel);
      }
      std::optional<json::object> wireMap = compactReasoningWireMap(value, supportedReasoningLevels);
      std::optional<RuntimePromptCacheCapabilities> promptCache = models::codex_models::compactPromptCacheCapabilities(rt::get(value, "prompt_cache"));
      return RuntimeCodexModel{
        .slug = slug,
        .default_reasoning_level = defaultReasoningLevel,
        .supported_reasoning_levels = std::move(supportedReasoningLevels),
        .reasoning_effort_wire_map = std::move(wireMap),
        .prompt_cache = std::move(promptCache),
      };
    }

    std::vector<RuntimeCodexModel> compactRuntimeCodexModelEntries(const json::array& values) {
      std::vector<RuntimeCodexModel> models;
      std::unordered_set<std::string> seen;
      for (const json::value& value : values) {
        if (!utils::isRecord(value)) continue;
        std::optional<RuntimeCodexModel> model = compactRuntimeCodexModel(value);
        if (!model || seen.contains(model->slug)) continue;
        seen.insert(model->slug);
        models.push_back(std::move(*model));
      }
      return models;
    }

    RuntimeCodexModelsSnapshot withRuntimeCodexModelsMeta(const json::object& snapshot, std::vector<RuntimeCodexModel> models) {
      const std::optional<std::string> sourceRaw = utils::getString(rt::get(snapshot, "source"));
      const std::string source = sourceRaw ? rt::trim(*sourceRaw) : std::string();
      const std::optional<double> updated = rt::getNumber(rt::get(snapshot, "updated_at_ms"));
      const double updatedAtMs = updated && rt::Number::isFinite(*updated) ? rt::Math::trunc(*updated) : 0;
      const std::optional<std::string> clientVersionRaw = utils::getString(rt::get(snapshot, "client_version"));
      const std::string clientVersion = clientVersionRaw ? rt::trim(*clientVersionRaw) : std::string();
      if (source.empty() || updatedAtMs <= 0) {
        throw RuntimeConfigError("runtime config requires valid Codex catalog metadata");
      }
      RuntimeCodexModelsSnapshot compacted{.models = std::move(models), .source = source, .updated_at_ms = updatedAtMs};
      if (!clientVersion.empty()) compacted.client_version = clientVersion;
      return compacted;
    }

    // compactRuntimeCodexModels past its `!snapshot` guard: `snapshot.models` exists only on an object.
    RuntimeCodexModelsSnapshot compactSnapshotObject(const json::object& snapshot) {
      const json::array* models = rt::getArray(snapshot, "models");
      if (models == nullptr) throw RuntimeConfigError("runtime config requires a non-empty Codex model catalog");
      std::vector<RuntimeCodexModel> compacted = compactRuntimeCodexModelEntries(*models);
      if (compacted.empty()) throw RuntimeConfigError("runtime config requires a non-empty Codex model catalog");
      return withRuntimeCodexModelsMeta(snapshot, std::move(compacted));
    }

    RuntimeConfigV2 enforceRuntimeConfigSize(RuntimeConfigV2 config) {
      const double bytes = serializedSize(toJson(config));
      if (bytes > RUNTIME_CONFIG_MAX_BYTES) {
        throw RuntimeConfigError("runtime config is too large (" + rt::numberToString(bytes) + " bytes; maximum " + rt::numberToString(RUNTIME_CONFIG_MAX_BYTES) +
                                 " bytes)");
      }
      return config;
    }

    // `snapshot.models.some((model) => model.slug === slug)`
    bool hasModel(const RuntimeCodexModelsSnapshot& snapshot, const std::string& slug) {
      for (const RuntimeCodexModel& model : snapshot.models) {
        if (model.slug == slug) return true;
      }
      return false;
    }

    // A stored value as the guards of this module (and compactPromptCacheCapabilities) observe it,
    // in JSON: what JSON can hold is kept; every other part (an undefined member or element, a hole,
    // a bigint, KvU64, Date, RegExp, Error, Map, Set, buffer or typed array) becomes `{}`, an object
    // without any member those guards read, which they treat as the TS treats the original (an
    // Error's inherited `name` aside: the TS reads it as a model's last slug candidate).

    rt::Task<std::optional<RuntimeConfigV2>> refreshRuntimeConfig(std::optional<std::shared_ptr<rt::Kv>> kvOverride, double nowMs,
                                                                  std::optional<RuntimeConfigV2> stale) {
      std::optional<RuntimeConfigV2> result;
      try {
        std::shared_ptr<rt::Kv> kv;
        if (!kvOverride) {
          const rt::Promise<std::shared_ptr<rt::Kv>> opening = kv::getKv();
          kv = co_await opening.wait();
        } else {
          kv = *kvOverride;
        }
        if (!kv) {
          if (stale) setCachedRuntimeConfig(CachedRuntimeConfig{*stale, nowMs + RUNTIME_CONFIG_CACHE_TTL_MS});
          result = stale;
        } else {
          const rt::KvGetOptions options{.consistency = rt::KvConsistencyLevel::strong};
          const rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(RUNTIME_CONFIG_V2_KEY, options));
          const rt::KvEntryMaybe<> entry = co_await reading.wait();
          std::optional<RuntimeConfigV2> value = normalizeRuntimeConfig(entry.value);
          if (!value) value = stale;
          if (value) setCachedRuntimeConfig(CachedRuntimeConfig{*value, nowMs + RUNTIME_CONFIG_CACHE_TTL_MS});
          result = std::move(value);
        }
      } catch (...) {
        rt::console::warn("[ai.ubq.fi] Runtime configuration refresh failed; using stale configuration:", std::current_exception());
        if (stale) setCachedRuntimeConfig(CachedRuntimeConfig{*stale, nowMs + RUNTIME_CONFIG_CACHE_TTL_MS});
        result = stale;
      }
      // `finally`: when the body finished without suspending, this runs before loadRuntimeConfig
      // stores the promise, which then stays in flight (as in the TS).
      runtimeConfigLoadInFlight().reset();
      co_return result;
    }
  }

  json::value toJson(const RuntimeCodexModel& value) {
    json::value out = Writer()
                        .required("slug", value.slug)
                        .orUndefined("default_reasoning_level", value.default_reasoning_level)
                        .required("supported_reasoning_levels", value.supported_reasoning_levels)
                        .orUndefined("reasoning_effort_wire_map", value.reasoning_effort_wire_map)
                        .done();
    if (value.prompt_cache) out.as_object()["prompt_cache"] = models::codex_models::toJson(*value.prompt_cache);
    return out;
  }

  json::value toJson(const RuntimeCodexModelsSnapshot& value) {
    return Writer()
      .required("models", value.models)
      .required("source", value.source)
      .required("updated_at_ms", value.updated_at_ms)
      .nullable("client_version", value.client_version)
      .done();
  }

  json::value toJson(const RuntimeConfigV2& value) {
    return Writer()
      .required("version", value.version)
      .required("default_model", value.default_model)
      .required("default_reasoning_effort", value.default_reasoning_effort)
      .required("codex_models", value.codex_models)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  RuntimeCodexModelsSnapshot compactRuntimeCodexModels(const std::optional<CodexModelsSnapshot>& snapshot) {
    if (!snapshot) throw RuntimeConfigError("runtime config requires a non-empty Codex model catalog");
    return compactSnapshotObject(*snapshot);
  }

  RuntimeCodexModelsSnapshot compactRuntimeCodexModels(const json::value* snapshot) {
    const json::object* object = snapshot != nullptr ? snapshot->if_object() : nullptr;
    if (object == nullptr) throw RuntimeConfigError("runtime config requires a non-empty Codex model catalog");
    return compactSnapshotObject(*object);
  }

  std::optional<RuntimeConfigV2> normalizeRuntimeConfig(const json::value* value) {
    if (!utils::isRecord(value)) return std::nullopt;
    const std::optional<double> version = rt::getNumber(rt::get(*value, "version"));
    const json::value* codexModels = rt::get(*value, "codex_models");
    if (!version || *version != 2 || !utils::isRecord(codexModels)) return std::nullopt;
    try {
      RuntimeCodexModelsSnapshot snapshot = compactRuntimeCodexModels(codexModels);
      const std::optional<std::string> defaultModelRaw = utils::getString(rt::get(*value, "default_model"));
      const std::string defaultModel = defaultModelRaw ? rt::trim(*defaultModelRaw) : std::string();
      const std::optional<ReasoningEffort> defaultReasoning = defaults::normalizeReasoningEffort(rt::get(*value, "default_reasoning_effort"));
      const std::optional<double> updated = rt::getNumber(rt::get(*value, "updated_at_ms"));
      const double updatedAtMs = updated && rt::Number::isFinite(*updated) ? rt::Math::trunc(*updated) : 0;
      if (defaultModel.empty() || !defaultReasoning || updatedAtMs <= 0) return std::nullopt;
      if (!hasModel(snapshot, defaultModel)) return std::nullopt;
      return enforceRuntimeConfigSize(RuntimeConfigV2{
        .version = 2,
        .default_model = defaultModel,
        .default_reasoning_effort = *defaultReasoning,
        .codex_models = std::move(snapshot),
        .updated_at_ms = updatedAtMs,
      });
    } catch (const rt::JsError&) {
      return std::nullopt;
    }
  }

  std::optional<RuntimeConfigV2> normalizeRuntimeConfig(const json::value& value) { return normalizeRuntimeConfig(&value); }

  std::optional<RuntimeConfigV2> normalizeRuntimeConfig(const rt::KvValue& value) {
    const json::value view = rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS);
    return normalizeRuntimeConfig(&view);
  }

  RuntimeConfigV2 buildRuntimeConfig(const CodexModelsSnapshot& fullSnapshot) { return buildRuntimeConfig(fullSnapshot, BuildRuntimeConfigOptions{}); }

  RuntimeConfigV2 buildRuntimeConfig(const CodexModelsSnapshot& fullSnapshot, const BuildRuntimeConfigOptions& options) {
    RuntimeCodexModelsSnapshot snapshot = compactSnapshotObject(fullSnapshot);
    const std::optional<std::string> requestedDefaultModel = options.defaultModel ? std::optional<std::string>(rt::trim(*options.defaultModel)) : std::nullopt;
    // An absent (or whitespace-only) request falls back to the catalog default.
    std::optional<std::string> defaultModel = requestedDefaultModel;
    if (!requestedDefaultModel || requestedDefaultModel->empty()) {
      json::value catalog = toJson(snapshot);
      defaultModel = models::codex_models::getCodexModelsSnapshotDefaultModel(std::optional<CodexModelsSnapshot>(std::move(catalog.as_object())));
    }
    if (!defaultModel || defaultModel->empty()) throw RuntimeConfigError("runtime config requires a default model");
    if (!hasModel(snapshot, *defaultModel)) {
      throw RuntimeConfigError("runtime config default model is absent from the catalog: " + *defaultModel);
    }
    const json::value reasoningInput = options.defaultReasoningEffort ? json::value(json::string(*options.defaultReasoningEffort)) : json::value(nullptr);
    ReasoningEffort defaultReasoningEffort = defaults::normalizeReasoningEffort(reasoningInput).value_or(defaults::DEFAULT_REASONING_EFFORT);
    const double updatedAtMs = options.nowMs ? *options.nowMs : rt::dateNow();
    return enforceRuntimeConfigSize(RuntimeConfigV2{
      .version = 2,
      .default_model = *defaultModel,
      .default_reasoning_effort = std::move(defaultReasoningEffort),
      .codex_models = std::move(snapshot),
      .updated_at_ms = updatedAtMs,
    });
  }

  rt::Task<std::optional<RuntimeConfigV2>> loadRuntimeConfig() { return loadRuntimeConfig(std::nullopt, rt::dateNow()); }

  rt::Task<std::optional<RuntimeConfigV2>> loadRuntimeConfig(std::shared_ptr<rt::Kv> kvOverride) {
    return loadRuntimeConfig(std::optional<std::shared_ptr<rt::Kv>>(std::move(kvOverride)), rt::dateNow());
  }

  rt::Task<std::optional<RuntimeConfigV2>> loadRuntimeConfig(std::optional<std::shared_ptr<rt::Kv>> kvOverride, double nowMs) {
    if (cachedRuntimeConfig() && cachedRuntimeConfig()->expires_at_ms > nowMs) co_return cachedRuntimeConfig()->value;
    if (runtimeConfigLoadInFlight()) {
      const rt::Promise<std::optional<RuntimeConfigV2>> inFlight = *runtimeConfigLoadInFlight();
      std::optional<RuntimeConfigV2> value = co_await inFlight.wait();
      co_return value;
    }
    std::optional<RuntimeConfigV2> stale;
    if (cachedRuntimeConfig()) stale = cachedRuntimeConfig()->value;
    const rt::Promise<std::optional<RuntimeConfigV2>> load(refreshRuntimeConfig(std::move(kvOverride), nowMs, std::move(stale)));
    runtimeConfigLoadInFlight() = load;
    std::optional<RuntimeConfigV2> value = co_await load.wait();
    co_return value;
  }

  void cacheRuntimeConfig(const RuntimeConfigV2& config) { cacheRuntimeConfig(config, rt::dateNow()); }

  void cacheRuntimeConfig(const RuntimeConfigV2& config, double nowMs) {
    const json::value value = toJson(config);
    std::optional<RuntimeConfigV2> normalized = normalizeRuntimeConfig(value);
    if (!normalized) throw RuntimeConfigError("runtime config is invalid or exceeds one 4 KiB read unit");
    setCachedRuntimeConfig(CachedRuntimeConfig{std::move(*normalized), nowMs + RUNTIME_CONFIG_CACHE_TTL_MS});
  }

  void resetRuntimeConfigCacheForTest() {
    cachedRuntimeConfig().reset();
    rt::memory::setCache(rt::memory::Cache::runtime_config, 0, 0);
    runtimeConfigLoadInFlight().reset();
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const RuntimeCodexModel& value) { out = toJson(value); }

  RuntimeCodexModel tag_invoke(const json::value_to_tag<RuntimeCodexModel>&, const json::value& value) {
    const Reader in(value, "RuntimeCodexModel");
    RuntimeCodexModel model{
      .slug = in.required<std::string>("slug"),
      .default_reasoning_level = in.orUndefined<std::string>("default_reasoning_level"),
      .supported_reasoning_levels = in.required<std::vector<std::string>>("supported_reasoning_levels"),
      .reasoning_effort_wire_map = in.orUndefined<json::object>("reasoning_effort_wire_map"),
    };
    if (model.reasoning_effort_wire_map) {
      for (const json::key_value_pair& entry : *model.reasoning_effort_wire_map) {
        if (!entry.value().is_string()) in.fail("reasoning_effort_wire_map", rt::get(value, "reasoning_effort_wire_map"), "Record<string, string> or undefined");
      }
    }
    if (const json::value* promptCache = rt::get(value, "prompt_cache")) {
      if (promptCache->is_bool() && !promptCache->get_bool()) model.prompt_cache = models::codex_models::RuntimePromptCacheCapabilitiesUnsupported{};
      else if (promptCache->is_object()) model.prompt_cache = json::value_to<models::codex_models::RuntimePromptCacheCapabilitiesV1>(*promptCache);
      else in.fail("prompt_cache", promptCache, "false, object or undefined");
    }
    return model;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const RuntimeCodexModelsSnapshot& value) { out = toJson(value); }

  RuntimeCodexModelsSnapshot tag_invoke(const json::value_to_tag<RuntimeCodexModelsSnapshot>&, const json::value& value) {
    const Reader in(value, "RuntimeCodexModelsSnapshot");
    return {
      .models = in.required<std::vector<RuntimeCodexModel>>("models"),
      .source = in.required<std::string>("source"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
      .client_version = in.nullable<std::string>("client_version"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const RuntimeConfigV2& value) { out = toJson(value); }

  RuntimeConfigV2 tag_invoke(const json::value_to_tag<RuntimeConfigV2>&, const json::value& value) {
    const Reader in(value, "RuntimeConfigV2");
    const double version = in.required<double>("version");
    if (version != 2) in.fail("version", rt::get(value, "version"), "2");
    return {
      .version = version,
      .default_model = in.required<std::string>("default_model"),
      .default_reasoning_effort = in.required<std::string>("default_reasoning_effort"),
      .codex_models = in.required<RuntimeCodexModelsSnapshot>("codex_models"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }
}
