#include "uos/models/openrouter_models.hpp"
#include "uos/rt/memory.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <unordered_set>
#include <utility>

namespace uos::models::openrouter_models {
  namespace {
    using utils::isRecord;
    using SnapshotPointer = std::shared_ptr<const OpenRouterModelsSnapshot>;

    constexpr std::string_view OPENROUTER_MODELS_QUERY = "output_modalities=text%2Cdecisions";
    const std::string OPENROUTER_MODELS_URL = std::string(OPENROUTER_BASE_URL) + "/api/v1/models?" + std::string(OPENROUTER_MODELS_QUERY);
    constexpr double OPENROUTER_MODELS_FAILURE_BACKOFF_MS = 30'000;

    const json::value* member(const json::value* value, std::string_view key) { return value != nullptr ? rt::get(*value, key) : nullptr; }

    std::optional<std::string> nonEmptyString(const json::value* value) {
      if (value == nullptr || !value->is_string()) return std::nullopt;
      std::string trimmed = rt::trim(std::string_view(value->get_string()));
      if (trimmed.empty()) return std::nullopt;
      return trimmed;
    }

    std::optional<std::string> nonEmptyString(std::string_view value) {
      std::string trimmed = rt::trim(value);
      if (trimmed.empty()) return std::nullopt;
      return trimmed;
    }

    std::optional<std::int64_t> positiveTokenCount(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      if (number && rt::Number::isSafeInteger(*number) && *number > 0) return static_cast<std::int64_t>(*number);
      return std::nullopt;
    }

    /** The wider of a model's own window and the window its default provider reports. */
    std::optional<std::int64_t> widerWindow(std::optional<std::int64_t> context, std::optional<std::int64_t> providerContext) {
      if (!context) return providerContext;
      if (!providerContext) return context;
      return std::max(*context, *providerContext);
    }

    std::vector<std::string> uniqueStrings(const std::vector<std::string>& values) {
      rt::OrderedSet<std::string> set(values.begin(), values.end());
      return std::vector<std::string>(set.begin(), set.end());
    }

    std::vector<std::string> nonEmptyStrings(const json::array& values) {
      std::vector<std::string> out;
      for (const json::value& entry : values) {
        std::optional<std::string> text = nonEmptyString(&entry);
        if (text) out.push_back(std::move(*text));
      }
      return out;
    }

    std::optional<OpenRouterReasoning> parseReasoning(const json::value* value) {
      if (!isRecord(value)) return std::nullopt;
      const json::array* supported = rt::getArray(member(value, "supported_efforts"));
      const std::vector<std::string> efforts = supported != nullptr ? nonEmptyStrings(*supported) : std::vector<std::string>{};
      std::optional<std::string> defaultEffort = nonEmptyString(member(value, "default_effort"));
      if (efforts.empty() && !defaultEffort) return std::nullopt;
      const json::value* mandatory = member(value, "mandatory");
      return OpenRouterReasoning{uniqueStrings(efforts), std::move(defaultEffort), mandatory != nullptr && mandatory->is_bool() && mandatory->get_bool()};
    }

    std::optional<std::vector<std::string>> outputModalitiesFrom(const json::value* value) {
      const json::array* modalities = rt::getArray(member(value, "output_modalities"));
      if (!isRecord(value) || modalities == nullptr) return std::nullopt;
      const std::vector<std::string> values = nonEmptyStrings(*modalities);
      if (values.empty()) return std::nullopt;
      return uniqueStrings(values);
    }

    std::optional<OpenRouterModelMetadata> openRouterModelFromUpstream(const json::value* value) {
      if (!isRecord(value)) return std::nullopt;
      std::optional<std::string> id = nonEmptyString(member(value, "id"));
      if (!id) return std::nullopt;
      const std::optional<std::int64_t> context = positiveTokenCount(member(value, "context_length"));
      const json::value* topProvider = isRecord(member(value, "top_provider")) ? member(value, "top_provider") : nullptr;
      const std::optional<std::int64_t> providerContext = positiveTokenCount(member(topProvider, "context_length"));
      std::optional<std::vector<std::string>> outputModalities = outputModalitiesFrom(member(value, "architecture"));
      return OpenRouterModelMetadata{std::move(*id), context, widerWindow(context, providerContext), parseReasoning(member(value, "reasoning")), std::move(outputModalities)};
    }

    bool contains(const std::vector<std::string>& values, std::string_view search) { return std::find(values.begin(), values.end(), search) != values.end(); }

    /** Lowercase alphanumerics joined by single dashes: `z-ai/glm-5.3` and `z-ai-glm-5-3` agree. */
    std::string normalizeModelKey(std::string_view value) {
      static const rt::Regex RUNS("[^a-z0-9]+", "g");
      static const rt::Regex LEADING("^-");
      static const rt::Regex TRAILING("-$");
      return rt::replace(rt::replace(rt::replace(rt::toLowerCase(rt::trim(value)), RUNS, "-"), LEADING, ""), TRAILING, "");
    }

    std::string lastSegment(std::string_view value) {
      std::vector<std::string> parts = rt::split(value, "/");
      return parts.back();
    }

    /**
     * `~author/model-latest` ids are rolling aliases of a canonical model. They are
     * indexed only under their alias form so the canonical row always wins a tie,
     * and they still resolve ids that exist nowhere else, such as the official
     * `deepseek-flash`.
     */
    std::optional<std::string> aliasKeyOf(std::string_view id) {
      static const rt::Regex LATEST("[-:]latest$");
      if (!rt::startsWith(id, "~")) return std::nullopt;
      const std::string withoutPrefix = rt::slice(id, 1);
      const std::string slug = lastSegment(withoutPrefix);
      const std::string strippedSlug = rt::replace(slug, LATEST, "");
      if (strippedSlug.empty()) return std::nullopt;
      return normalizeModelKey(strippedSlug);
    }

    using Tier = std::unordered_map<std::string, const OpenRouterModelMetadata*>;

    void claim(Tier& tier, const std::string& key, const OpenRouterModelMetadata& model) {
      if (key.empty()) return;
      const auto existing = tier.find(key);
      if (existing == tier.end()) {
        tier.emplace(key, &model);
        return;
      }
      // A key claimed by two upstream models cannot identify either of them.
      if (existing->second == nullptr || existing->second->id != model.id) existing->second = nullptr;
    }

    /**
     * A trailing billing or processing marker (`:free`, `:batch`) names the same
     * model under different terms, so its base row still describes the capabilities.
     * Only these two markers are stripped: `preview` and dated suffixes name
     * different snapshots and must not be folded together.
     */
    const rt::Regex& billingMarkerSuffix() {
      static const rt::Regex regex("[-:](?:free|batch)$");
      return regex;
    }

    std::optional<std::string> baseKeyOf(const std::string& slug) {
      const std::string stripped = rt::replace(slug, billingMarkerSuffix(), "");
      if (stripped == slug) return std::nullopt;
      return normalizeModelKey(stripped);
    }

    OpenRouterMatchIndex buildMatchIndex(const std::vector<OpenRouterModelMetadata>& models) {
      OpenRouterMatchIndex index;
      for (const OpenRouterModelMetadata& model : models) {
        const std::string slug = lastSegment(model.id);
        claim(index.exactId, rt::toLowerCase(model.id), model);
        claim(index.exactSlug, rt::toLowerCase(slug), model);
        claim(index.normalizedId, normalizeModelKey(model.id), model);
        claim(index.normalizedSlug, normalizeModelKey(slug), model);
        const std::optional<std::string> aliasKey = aliasKeyOf(model.id);
        if (rt::truthy(aliasKey)) claim(index.alias, *aliasKey, model);
        const std::optional<std::string> baseKey = baseKeyOf(slug);
        if (rt::truthy(baseKey)) claim(index.base, *baseKey, model);
      }
      return index;
    }

    const OpenRouterModelMetadata* lookupTier(const Tier& tier, const std::string& key) {
      const auto found = tier.find(key);
      return found == tier.end() ? nullptr : found->second;
    }

    struct ModuleState {
      SnapshotPointer cache;
      std::shared_ptr<const OpenRouterMatchIndex> index;
      std::optional<rt::Promise<SnapshotPointer>> fetchInFlight;
      double retryAfterMs = 0;
      OpenRouterFetch fetchForTest;
      bool failIndexAllocationForTest = false;
    };

    ModuleState& moduleState() {
      static ModuleState state;
      return state;
    }

    const OpenRouterFetch& defaultOpenRouterFetch() {
      static const OpenRouterFetch fetch = rt::defaultFetch();
      return fetch;
    }

    rt::AbortSignal boundedOpenRouterSignal(const std::optional<rt::AbortSignal>& signal) {
      const rt::AbortSignal timeout = rt::AbortSignal::timeout(OPENROUTER_FETCH_TIMEOUT_MS);
      if (!signal) return timeout;
      return rt::AbortSignal::any({*signal, timeout});
    }

    SnapshotPointer publishOpenRouterSnapshot(std::vector<OpenRouterModelMetadata> models) {
      auto snapshot = std::make_shared<const OpenRouterModelsSnapshot>(OpenRouterModelsSnapshot{std::move(models), rt::dateNow()});
      ModuleState& state = moduleState();
      if (state.failIndexAllocationForTest) throw std::bad_alloc();
      auto index = std::make_shared<const OpenRouterMatchIndex>(buildMatchIndex(snapshot->models));
      std::size_t bytes = snapshot->models.capacity() * sizeof(OpenRouterModelMetadata);
      for (const auto& model : snapshot->models) {
        bytes += rt::memory::bytes(model.id);
        if (model.reasoning) bytes += rt::memory::bytes(model.reasoning->supported_efforts);
        if (model.output_modalities) bytes += rt::memory::bytes(*model.output_modalities);
      }
      for (const auto* tier : {&index->exactId, &index->exactSlug, &index->normalizedId, &index->normalizedSlug, &index->alias, &index->base}) {
        bytes += tier->bucket_count() * sizeof(void*);
        for (const auto& entry : *tier) bytes += sizeof(entry) + rt::memory::bytes(entry.first);
      }
      rt::memory::setCache(rt::memory::Cache::openrouter_models, snapshot->models.size(), bytes);
      state.cache = snapshot;
      state.index = std::move(index);
      state.retryAfterMs = 0;
      return snapshot;
    }

    rt::Task<SnapshotPointer> requestOpenRouterModels(OpenRouterFetch fetcher, std::optional<rt::AbortSignal> callerSignal) {
      const rt::AbortSignal signal = boundedOpenRouterSignal(callerSignal);
      SnapshotPointer published;
      bool failed = false;
      try {
        rt::RequestInit init;
        init.headers = rt::Headers{{"Accept", "application/json"}};
        init.signal = signal;
        rt::RequestInfo input = OPENROUTER_MODELS_URL;
        rt::Task<rt::Response> call = fetcher(std::move(input), std::move(init));
        const rt::Response response = co_await std::move(call);
        if (!response.ok()) {
          if (std::optional<rt::ByteStream> body = response.body()) {
            rt::Task<void> cancelling = body->cancel();
            try {
              co_await std::move(cancelling);
            } catch (...) {
            }
          }
          throw rt::JsError("OpenRouter models request failed with HTTP " + rt::numberToString(response.status()));
        }
        rt::Task<json::value> reading = response.json();
        const json::value payload = co_await std::move(reading);
        std::vector<OpenRouterModelMetadata> models = openRouterModelsFromPayload(payload);
        // An empty catalog is a failed refresh, not an empty world: keep the last
        // good snapshot so a schema change upstream cannot blank the page.
        if (models.empty()) throw rt::JsError("OpenRouter models response carried no models");
        published = publishOpenRouterSnapshot(std::move(models));
      } catch (...) {
        failed = true;
      }
      if (failed) {
        moduleState().retryAfterMs = rt::dateNow() + OPENROUTER_MODELS_FAILURE_BACKOFF_MS;
        co_return moduleState().cache;
      }
      co_return published;
    }
  }

  bool openRouterModelSupportsText(const OpenRouterModelMetadata& model) { return !model.output_modalities || contains(*model.output_modalities, "text"); }

  bool openRouterModelSupportsDecisions(const OpenRouterModelMetadata& model) { return model.output_modalities && contains(*model.output_modalities, "decisions"); }

  std::vector<std::string> openRouterModelEndpointPaths(const OpenRouterModelMetadata& model) {
    std::vector<std::string> paths;
    if (openRouterModelSupportsText(model)) {
      paths.emplace_back("/v1/responses");
      paths.emplace_back("/v1/chat/completions");
    }
    if (openRouterModelSupportsDecisions(model)) paths.emplace_back("/v1/decisions");
    return paths;
  }

  std::vector<OpenRouterModelMetadata> openRouterModelsFromPayload(const json::value* payload) {
    if (!isRecord(payload)) return {};
    const json::array* data = rt::getArray(member(payload, "data"));
    if (data == nullptr) return {};
    std::vector<OpenRouterModelMetadata> models;
    std::unordered_set<std::string> seen;
    for (const json::value& value : *data) {
      std::optional<OpenRouterModelMetadata> model = openRouterModelFromUpstream(&value);
      if (!model || !seen.insert(model->id).second) continue;
      models.push_back(std::move(*model));
    }
    return models;
  }

  std::vector<OpenRouterModelMetadata> openRouterModelsFromPayload(const json::value& payload) { return openRouterModelsFromPayload(&payload); }

  std::optional<OpenRouterModelMetadata> matchOpenRouterModel(const std::vector<OpenRouterModelMetadata>& models, std::string_view modelId,
                                                              const OpenRouterMatchIndex* index) {
    const std::optional<std::string> target = nonEmptyString(modelId);
    if (!target) return std::nullopt;
    std::optional<OpenRouterMatchIndex> built;
    if (index == nullptr) built = buildMatchIndex(models);
    const OpenRouterMatchIndex& tiers = index != nullptr ? *index : *built;
    const std::string slug = lastSegment(*target);
    // A gateway id may carry the billing marker itself (`hy3-free` where upstream
    // lists `tencent/hy3`), so the marker-stripped slug is tried last.
    const std::string strippedSlug = rt::replace(slug, billingMarkerSuffix(), "");
    const OpenRouterModelMetadata* stripped = nullptr;
    if (strippedSlug != slug) {
      stripped = lookupTier(tiers.exactSlug, rt::toLowerCase(strippedSlug));
      if (stripped == nullptr) stripped = lookupTier(tiers.normalizedSlug, normalizeModelKey(strippedSlug));
    }
    const OpenRouterModelMetadata* match = lookupTier(tiers.exactId, rt::toLowerCase(*target));
    if (match == nullptr) match = lookupTier(tiers.exactSlug, rt::toLowerCase(slug));
    if (match == nullptr) match = lookupTier(tiers.normalizedId, normalizeModelKey(*target));
    if (match == nullptr) match = lookupTier(tiers.normalizedSlug, normalizeModelKey(slug));
    if (match == nullptr) match = lookupTier(tiers.alias, normalizeModelKey(slug));
    if (match == nullptr) match = lookupTier(tiers.base, normalizeModelKey(slug));
    if (match == nullptr) match = stripped;
    if (match == nullptr) return std::nullopt;
    return *match;
  }

  void resetOpenRouterModelsCacheForTest() {
    ModuleState& state = moduleState();
    state.cache.reset();
    rt::memory::setCache(rt::memory::Cache::openrouter_models, 0, 0);
    state.index.reset();
    state.fetchInFlight.reset();
    state.retryAfterMs = 0;
    state.failIndexAllocationForTest = false;
  }

  void setOpenRouterModelsFetchForTest(OpenRouterFetch fetcher) { moduleState().fetchForTest = std::move(fetcher); }
  void setOpenRouterIndexAllocationFailureForTest(bool fail) { moduleState().failIndexAllocationForTest = fail; }

  rt::Task<std::shared_ptr<const OpenRouterModelsSnapshot>> fetchOpenRouterModels(FetchOpenRouterModelsOptions options) {
    ModuleState& state = moduleState();
    if (!options.force && state.cache && rt::dateNow() - state.cache->updated_at_ms < OPENROUTER_MODELS_CACHE_TTL_MS) {
      co_return state.cache;
    }
    if (!options.force && rt::dateNow() < state.retryAfterMs) co_return state.cache;
    if (options.cachedOnly) co_return state.cache;
    OpenRouterFetch fetcher = options.fetcher;
    if (!fetcher) fetcher = state.fetchForTest;
    if (!fetcher && rt::globalFetch() == defaultOpenRouterFetch()) fetcher = defaultOpenRouterFetch();
    if (!fetcher) co_return state.cache;
    const bool shouldCoalesce = !options.force && !options.fetcher && !options.signal;
    if (shouldCoalesce && state.fetchInFlight) {
      const rt::Promise<SnapshotPointer> inFlight = *state.fetchInFlight;
      SnapshotPointer coalesced = co_await inFlight.wait();
      co_return coalesced;
    }

    const rt::Promise<SnapshotPointer> request(requestOpenRouterModels(fetcher, options.signal));
    if (shouldCoalesce) state.fetchInFlight = request;
    rt::Reason failure;
    SnapshotPointer result;
    try {
      result = co_await request.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    if (state.fetchInFlight && *state.fetchInFlight == request) state.fetchInFlight.reset();
    if (failure) std::rethrow_exception(failure);
    co_return result;
  }

  void warmOpenRouterModels() { rt::spawn(fetchOpenRouterModels(), rt::ignoreRejection); }

  std::optional<OpenRouterModelMetadata> openRouterMetadataFor(std::string_view modelId) {
    const ModuleState& state = moduleState();
    if (!state.cache) return std::nullopt;
    return matchOpenRouterModel(state.cache->models, modelId, state.index.get());
  }

  std::shared_ptr<const OpenRouterModelsSnapshot> openRouterModelsSnapshot() { return moduleState().cache; }
}
