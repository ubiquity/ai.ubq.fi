/** Preserve a saved pre-rename synthetic selection across the versionless rename. */

/**
 * Normalize a raw KV value into a valid `CodexModelsWhitelist`.
 * Returns `null` when the value is absent or cannot be read.
 */

/**
 * Canonical form of a submitted model-ID list: non-empty identifiers only, in
 * first-seen order, without duplicates. Storage and the wire response both use
 * this form so a re-save of an already-normalized selection is a no-op.
 */

/**
 * Load the whitelist from KV. Returns `null` when KV is unavailable or the value
 * is absent/invalid.
 */

/**
 * Store a whitelist to KV.
 * `modelIds` is the list of model identifiers to allow. An empty list clears the filter.
 * Identifiers are trimmed and de-duplicated before they are persisted.
 */

/**
 * Build a Set of whitelisted model identifiers for efficient lookup.
 * Returns `null` when the whitelist is empty or absent.
 */

/**
 * Extract a model identifier from a raw catalog snapshot record (slug/id/model/name).
 * Returns `null` when no valid identifier is found.
 */

/**
 * Filter a `Record<string, unknown>[]` (the model-object array format used by
 * `/v1/models`, `/uos/models/capabilities`, and catalog entries).
 * When the whitelist is empty/absent, returns the input unchanged.
 */

/**
 * Filter a catalog `parsed.models` array in the Codex catalog response format
 * (raw records with slug/id/model/name identifiers).
 * When the whitelist is empty/absent, returns the input unchanged.
 */

/**
 * Filter a catalog-public model entries (entries with `id` field).
 * When the whitelist is empty/absent, returns the input unchanged.
 */
#include "uos/models/codex_models_whitelist.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"
#include <algorithm>
#include <unordered_set>
#include <utility>
namespace uos::models::codex_models_whitelist {
  namespace {
    std::string canonicalWhitelistModelId(std::string id) {
      return id == deepseek::waterfall::DEEPSEEK_WATERFALL_LEGACY_MODEL_ID ? std::string(deepseek::waterfall::DEEPSEEK_WATERFALL_MODEL_ID) : id;
    }
    std::optional<std::string> modelIdFromRecord(const json::value& model) {
      auto id = utils::getString(rt::get(model, "slug"));
      if (!id) id = utils::getString(rt::get(model, "id"));
      if (!id) id = utils::getString(rt::get(model, "model"));
      if (!id) id = utils::getString(rt::get(model, "name"));
      return id ? std::optional(rt::trim(*id)) : std::nullopt;
    }
    template<class Predicate> json::array filter(const json::array& models, const std::optional<CodexModelsWhitelist>& whitelist, Predicate predicate) {
      if (!whitelist || whitelist->model_ids.empty()) return models;
      std::unordered_set<std::string> allowed(whitelist->model_ids.begin(), whitelist->model_ids.end());
      json::array result;
      for (const auto& model : models) {
        const auto id = predicate(model);
        if (id && allowed.contains(*id)) result.push_back(model);
      }
      return result;
    }
    template<class Fn> std::vector<json::object> typedFilter(const std::vector<json::object>& models, const std::optional<CodexModelsWhitelist>& whitelist, Fn fn) {
      json::array raw;
      for (const auto& model : models) raw.push_back(model);
      const auto filtered = fn(raw, whitelist);
      std::vector<json::object> result;
      for (const auto& model : filtered) result.push_back(model.as_object());
      return result;
    }
  }
  std::optional<CodexModelsWhitelist> normalizeCodexModelsWhitelist(const json::value* value) {
    if (!utils::isRecord(value)) return std::nullopt;
    const auto* rawIds = rt::getArray(*value, "model_ids");
    if (!rawIds) return std::nullopt;
    std::vector<std::string> modelIds;
    for (const auto& raw : *rawIds) {
      const auto id = utils::getString(raw);
      if (!id || id->empty()) return std::nullopt;
      const auto trimmed = rt::trim(*id);
      if (trimmed.empty()) return std::nullopt;
      modelIds.push_back(canonicalWhitelistModelId(trimmed));
    }
    const auto timestamp = rt::getNumber(*value, "updated_at_ms");
    if (!timestamp || !rt::Number::isSafeInteger(*timestamp) || *timestamp <= 0) return std::nullopt;
    return CodexModelsWhitelist{std::move(modelIds), *timestamp};
  }
  std::optional<CodexModelsWhitelist> normalizeCodexModelsWhitelist(const json::value& value) { return normalizeCodexModelsWhitelist(&value); }
  std::optional<CodexModelsWhitelist> normalizeCodexModelsWhitelist(const rt::KvValue& value) {
    const auto view = rt::storedJsonView(value);
    return normalizeCodexModelsWhitelist(view);
  }
  std::vector<std::string> normalizeWhitelistModelIds(const json::array& rawIds) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    for (const auto& raw : rawIds) {
      const auto id = utils::getString(raw);
      if (!id || id->empty()) continue;
      const auto modelId = canonicalWhitelistModelId(rt::trim(*id));
      if (modelId.empty() || seen.contains(modelId)) continue;
      seen.insert(modelId);
      result.push_back(modelId);
    }
    return result;
  }
  std::vector<std::string> normalizeWhitelistModelIds(const std::vector<std::string>& rawIds) {
    json::array values;
    for (const auto& id : rawIds) values.push_back(json::value(id));
    return normalizeWhitelistModelIds(values);
  }
  rt::Task<std::optional<CodexModelsWhitelist>> loadCodexModelsWhitelist(std::shared_ptr<rt::Kv> kv) {
    if (!kv) co_return std::nullopt;
    const rt::KvGetOptions options{.consistency = rt::KvConsistencyLevel::strong};
    auto reading = kv->get(CODEX_MODELS_WHITELIST_KV_KEY, options);
    const auto entry = co_await std::move(reading);
    co_return normalizeCodexModelsWhitelist(entry.value);
  }
  rt::Task<bool> storeCodexModelsWhitelist(std::shared_ptr<rt::Kv> kv, std::vector<std::string> modelIds) {
    const CodexModelsWhitelist whitelist{normalizeWhitelistModelIds(modelIds), rt::dateNow()};
    const rt::KvValue stored(json::value_from(whitelist));
    const auto commit = co_await kv->set(CODEX_MODELS_WHITELIST_KV_KEY, stored);
    co_return commit.ok;
  }
  json::array filterWhitelistedModelList(const json::array& models, const std::optional<CodexModelsWhitelist>& whitelist) {
    return filter(models, whitelist, [](const json::value& model) { return utils::getString(rt::get(model, "id")); });
  }
  std::vector<json::object> filterWhitelistedModelList(const std::vector<json::object>& models, const std::optional<CodexModelsWhitelist>& whitelist) {
    return typedFilter(models, whitelist, [](const auto& values, const auto& selection) { return filterWhitelistedModelList(values, selection); });
  }
  json::array filterWhitelistedCatalogModels(const json::array& models, const std::optional<CodexModelsWhitelist>& whitelist) {
    return filter(models, whitelist, [](const json::value& model) { return utils::isRecord(model) ? modelIdFromRecord(model) : std::nullopt; });
  }
  std::vector<json::object> filterWhitelistedCatalogModels(const std::vector<json::object>& models, const std::optional<CodexModelsWhitelist>& whitelist) {
    return typedFilter(models, whitelist, [](const auto& values, const auto& selection) { return filterWhitelistedCatalogModels(values, selection); });
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexModelsWhitelist& value) {
    out = rt::record::Writer().required("model_ids", value.model_ids).required("updated_at_ms", value.updated_at_ms).done();
  }
  CodexModelsWhitelist tag_invoke(const json::value_to_tag<CodexModelsWhitelist>&, const json::value& value) {
    const rt::record::Reader in(value, "CodexModelsWhitelist");
    return {in.required<std::vector<std::string>>("model_ids"), in.required<double>("updated_at_ms")};
  }
}
