#include "uos/models/catalog.hpp"
#include "uos/catalog/index.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/http.hpp"
#include "uos/input_normalization.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/models/codex_models_whitelist.hpp"
#include "uos/models/metadata.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/gemini.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/request_policy.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"
#include <algorithm>
#include <tuple>
#include <unordered_set>

namespace uos::models::catalog {
namespace {
  namespace sel = provider::selection;
  namespace cc = provider::claude_code_models;
  namespace cm = codex_models_fetch;
  namespace norm = input_normalization;
  using Selection = std::optional<sel::ProviderSelection>;
  using Snapshot = std::optional<codex_models::CodexModelsSnapshot>;
  using Claude = std::optional<cc::ClaudeCodeModelsSnapshot>;
  using ModelMap = rt::OrderedMap<std::string, PublicModelCatalogEntry>;
  using rt::record::Reader;
  using rt::record::Writer;
  std::string snapshotUpstreamSource(const Snapshot& snapshot) {
    const auto source = snapshot ? rt::getString(*snapshot, "source") : std::nullopt;
    return source && !source->empty() ? std::string(*source) : "stored_codex_models";
  }
  rt::Task<Claude> enabledClaudeCatalog(Selection selection) {
    if (!sel::isProviderEnabled("claude_code", selection)) co_return std::nullopt;
    auto result = co_await cc::fetchClaudeCodeModels(); co_return result;
  }
  void appendClaudeModels(json::array& models, const std::vector<cc::ClaudeCodeModel>& claude) {
    std::unordered_set<std::string> seen;
    for (const auto& model : models) seen.emplace(rt::getString(model, "id").value_or(""));
    for (const auto& model : claude) if (!seen.contains(model.id)) models.push_back(json::object{{"id", model.id}, {"object", "model"}, {"created", model.created}, {"owned_by", "anthropic"}});
  }
  json::array claudeCapabilities(const std::vector<cc::ClaudeCodeModel>& models) {
    json::array rows;
    for (const auto& model : models) {
      const auto levels = cc::claudeCodeReasoningLevels(model);
      json::object row{{"id", model.id}, {"object", "uos.model_capabilities"}, {"owned_by", "anthropic"}, {"display_name", model.display_name.value_or(model.id)}, {"upstream_provider", "claude_code"}, {"supported_endpoints", json::array{"/v1/responses", "/v1/chat/completions"}}, {"supported_reasoning_levels", json::value_from(levels)}};
      if (model.supports_disabled_thinking.value_or(false)) row["default_reasoning_effort"] = "none";
      row["context_window_tokens"] = model.context_window_tokens ? json::value(*model.context_window_tokens) : json::value(nullptr);
      row["context_source"] = model.context_window_tokens && rt::truthy(*model.context_window_tokens) ? "provider_discovery" : "unknown";
      row["reasoning_source"] = levels.empty() ? "unknown" : "provider_discovery";
      row["max_output_tokens"] = model.max_output_tokens ? json::value(*model.max_output_tokens) : json::value(nullptr);
      row["upstream_capabilities"] = json::object{{"image_input", model.supports_image_input ? json::value(*model.supports_image_input) : json::value(nullptr)}, {"pdf_input", model.supports_pdf_input ? json::value(*model.supports_pdf_input) : json::value(nullptr)}};
      rows.push_back(std::move(row));
    }
    return rows;
  }
  std::vector<std::string> providerSupportedEndpointPaths(const std::vector<std::string>& supported) {
    std::vector<std::string> result;
    if (rt::includes(supported, std::string("openai-response"))) result.push_back("/v1/responses");
    if (rt::includes(supported, std::string("openai"))) result.push_back("/v1/chat/completions");
    return result;
  }
  std::string catalogAvailabilityStatus(bool available) { return available ? "available" : "unavailable"; }
  PublicModelCatalogEntry publicModelCatalogEntry(const std::string& id, const PublicModelProvider& provider, const json::value& created, const metadata::ModelMetadataSources& sources) {
    auto resolved = metadata::resolveModelMetadata(id, sources);
    PublicModelCatalogEntry row{.id = id, .providers = {provider}};
    if (rt::Number::isSafeInteger(created) && rt::toNumber(created) > 0) row.created = rt::toNumber(created);
    row.context_window_tokens = resolved.context_window_tokens; row.max_context_window_tokens = resolved.max_context_window_tokens;
    row.auto_compact_token_limit_tokens = resolved.auto_compact_token_limit_tokens; row.effective_context_window_percent = resolved.effective_context_window_percent;
    row.supported_reasoning_levels = resolved.supported_reasoning_levels; row.default_reasoning_effort = resolved.default_reasoning_effort;
    row.context_source = resolved.context_source; row.reasoning_source = resolved.reasoning_source;
    return row;
  }
  void mergeProviderMetadata(PublicModelCatalogEntry& row, const std::string& id, const metadata::ModelMetadataSources& sources) {
    metadata::ModelMetadataSources providerSources; providerSources.provider = sources.provider; providerSources.openRouter = nullptr;
    auto resolved = metadata::resolveModelMetadata(id, providerSources);
    if (resolved.context_window_tokens) row.context_window_tokens = resolved.context_window_tokens;
    if (resolved.max_context_window_tokens) row.max_context_window_tokens = resolved.max_context_window_tokens;
    if (resolved.auto_compact_token_limit_tokens) row.auto_compact_token_limit_tokens = resolved.auto_compact_token_limit_tokens;
    if (resolved.effective_context_window_percent) row.effective_context_window_percent = resolved.effective_context_window_percent;
    if (resolved.context_window_tokens || resolved.max_context_window_tokens) row.context_source = resolved.context_source;
    if (resolved.supported_reasoning_levels) row.supported_reasoning_levels = resolved.supported_reasoning_levels;
    if (resolved.default_reasoning_effort) row.default_reasoning_effort = resolved.default_reasoning_effort;
    if (resolved.supported_reasoning_levels || resolved.default_reasoning_effort) row.reasoning_source = resolved.reasoning_source;
  }
  void addPublicModelCatalogEntry(ModelMap& models, const std::string& id, const PublicModelProvider& provider, const json::value& created = nullptr, const metadata::ModelMetadataSources& sources = {}) {
    if (auto* existing = models.get(id)) {
      existing->providers.push_back(provider);
      if (!existing->created && rt::Number::isSafeInteger(created) && rt::toNumber(created) > 0) existing->created = rt::toNumber(created);
      if (sources.provider.hasValue()) mergeProviderMetadata(*existing, id, sources);
    } else models.set(id, publicModelCatalogEntry(id, provider, created, sources));
  }
  rt::OrderedMap<std::string, json::object> collectCodexSnapshotRecords(const Snapshot& snapshot) {
    rt::OrderedMap<std::string, json::object> rows;
    if (!snapshot) return rows;
    if (const auto* models = rt::getArray(*snapshot, "models")) for (const auto& model : *models) {
      if (!utils::isRecord(model) || !model.is_object()) continue;
      const auto id = request_policy::modelIdFromSnapshotRecord(model.as_object());
      if (id) rows.set(*id, model.as_object());
    }
    return rows;
  }
  ModelCatalogSource credentialGatedCatalogSource(bool configured, double count) { return {.status = catalogAvailabilityStatus(configured), .count = configured ? count : 0, .updated_at_ms = std::nullopt, .configured = configured}; }
  struct CredentialCounts { double deepseek = 0; double cerebras = 0; double lithos = 0; double gemini = 0; };
  CredentialCounts addCredentialGatedCatalogProviders(ModelMap& models) {
    CredentialCounts count;
    if (provider::gemini::geminiCliConfigured()) {
      PublicModelProvider provider{"gemini", "google", {"/v1/chat/completions", "/v1/responses"}};
      for (const auto id : provider::gemini::GEMINI_MODEL_IDS) {
        metadata::ModelMetadataHint hint;
        hint.context_window_tokens = provider::gemini::GEMINI_CONTEXT_WINDOW_TOKENS;
        hint.max_context_window_tokens = provider::gemini::GEMINI_CONTEXT_WINDOW_TOKENS;
        const std::string effort = rt::endsWith(id, "-high") ? "high" : rt::endsWith(id, "-medium") ? "medium" : "low";
        hint.supported_reasoning_levels = json::array{effort};
        hint.default_reasoning_effort = effort;
        metadata::ModelMetadataSources sources; sources.provider = hint;
        addPublicModelCatalogEntry(models, std::string(id), provider, nullptr, sources);
        ++count.gemini;
      }
    }
    if (deepseek::index::readDeepSeekApiKey()) {
      PublicModelProvider provider{"deepseek", "deepseek", {"/v1/chat/completions", "/v1/responses"}};
      metadata::ModelMetadataHint hint; hint.context_window_tokens = deepseek::index::DEEPSEEK_CONTEXT_WINDOW_TOKENS; hint.max_context_window_tokens = deepseek::index::DEEPSEEK_CONTEXT_WINDOW_TOKENS;
      json::array levels; for (auto effort : deepseek::index::DEEPSEEK_REASONING_LEVELS) levels.emplace_back(std::string(effort)); hint.supported_reasoning_levels = levels; hint.default_reasoning_effort = json::value(std::string(deepseek::index::DEEPSEEK_DEFAULT_REASONING_EFFORT));
      metadata::ModelMetadataSources sources; sources.provider = hint;
      for (auto id : deepseek::index::DEEPSEEK_OFFICIAL_MODEL_IDS) { addPublicModelCatalogEntry(models, std::string(id), provider, nullptr, sources); ++count.deepseek; }
    }
    if (provider::cerebras::readCerebrasApiKey()) {
      PublicModelProvider provider{"cerebras", "cerebras", {"/v1/chat/completions"}};
      for (auto id : provider::cerebras::CEREBRAS_MODELS) { metadata::ModelMetadataSources sources; sources.provider = request_policy::cerebrasProviderHint(std::string(id)); addPublicModelCatalogEntry(models, std::string(id), provider, nullptr, sources); ++count.cerebras; }
    }
    if (provider::lithos::readLithosApiKey()) {
      PublicModelProvider provider{"lithos", "lithos", {"/v1/chat/completions", "/v1/responses"}};
      metadata::ModelMetadataSources sources; sources.provider = request_policy::LITHOS_PROVIDER_HINT();
      for (auto id : provider::lithos::LITHOS_MODEL_IDS) { addPublicModelCatalogEntry(models, std::string(id), provider, nullptr, sources); ++count.lithos; }
    }
    return count;
  }
  void addDeepSeekWaterfallCatalogEntries(ModelMap& models) {
    namespace w = deepseek::waterfall;
    bool configured = false; for (auto provider : w::DEEPSEEK_WATERFALL_ORDER) if (w::deepSeekWaterfallProviderConfigured(provider)) configured = true;
    if (!configured) return;
    metadata::ModelMetadataHint hint; hint.context_window_tokens = w::DEEPSEEK_WATERFALL_CONTEXT_WINDOW_TOKENS; hint.max_context_window_tokens = w::DEEPSEEK_WATERFALL_CONTEXT_WINDOW_TOKENS; hint.effective_context_window_percent = w::DEEPSEEK_WATERFALL_EFFECTIVE_CONTEXT_WINDOW_PERCENT;
    json::array levels; for (auto effort : w::DEEPSEEK_WATERFALL_REASONING_LEVELS) levels.emplace_back(std::string(effort)); hint.supported_reasoning_levels = levels; hint.default_reasoning_effort = json::value(std::string(w::DEEPSEEK_WATERFALL_DEFAULT_REASONING_LEVEL));
    metadata::ModelMetadataSources sources; sources.provider = hint;
    // The rolling OpenRouter alias advertises the provider's window. This route uses the gateway's admission-safe cap.
    sources.openRouter = nullptr;
    addPublicModelCatalogEntry(models, std::string(w::DEEPSEEK_WATERFALL_MODEL_ID), {"ubiquity", "ubiquity", {"/v1/responses"}}, w::DEEPSEEK_WATERFALL_CREATED, sources);
    for (auto provider : w::DEEPSEEK_WATERFALL_ORDER) if (w::deepSeekWaterfallProviderConfigured(provider)) addPublicModelCatalogEntry(models, std::string(w::DEEPSEEK_WATERFALL_MODEL_ID), {std::string(provider), "ubiquity", {"/v1/responses"}});
  }
  double addOpenRouterCatalogEntries(ModelMap& models) {
    if (!provider::openrouter::readOpenRouterApiKey()) return 0;
    double served = 0;
    if (const auto snapshot = openrouter_models::openRouterModelsSnapshot()) for (const auto& model : snapshot->models) {
      const auto endpoints = openrouter_models::openRouterModelEndpointPaths(model); if (endpoints.empty()) continue;
      const auto author = rt::split(model.id, "/")[0];
      addPublicModelCatalogEntry(models, model.id, {"openrouter", author.empty() ? "openrouter" : author, endpoints}, 0); ++served;
    }
    addPublicModelCatalogEntry(models, std::string(norm::OPENROUTER_SERVED_MODEL_ID), {"openrouter", "typesafe", {"/v1/decisions", "/v1/systemone"}}, 0);
    return served + 1;
  }
  bool hasId(const json::array& rows, std::string_view id) { return std::any_of(rows.begin(), rows.end(), [id](const auto& row) { return rt::getString(row, "id") == id; }); }
  json::object discoveredModelCapabilitiesEntry(const json::object& model, const std::string& provider) {
    const auto id = std::string(rt::getString(model, "id").value_or("")); const auto resolved = metadata::resolveModelMetadata(id);
    std::vector<std::string> supported; if (auto* endpoints = rt::getArray(model, "supported_endpoint_types")) for (const auto& endpoint : *endpoints) supported.push_back(rt::toString(endpoint));
    json::object row{{"id", id}, {"object", "uos.model_capabilities"}, {"owned_by", *rt::get(model, "owned_by")}, {"display_name", id}, {"upstream_provider", provider}, {"supported_endpoints", json::value_from(providerSupportedEndpointPaths(supported))}, {"supported_reasoning_levels", resolved.supported_reasoning_levels ? json::value_from(*resolved.supported_reasoning_levels) : json::value(json::array{"none"})}};
    if (resolved.default_reasoning_effort || !resolved.supported_reasoning_levels) row["default_reasoning_effort"] = resolved.default_reasoning_effort.value_or("none");
    row["reasoning_effort_wire_map"] = json::object{};
    row["context_window_tokens"] = resolved.context_window_tokens ? json::value(*resolved.context_window_tokens) : json::value(nullptr);
    row["max_context_window_tokens"] = resolved.max_context_window_tokens ? json::value(*resolved.max_context_window_tokens) : json::value(nullptr);
    row["auto_compact_token_limit_tokens"] = resolved.auto_compact_token_limit_tokens ? json::value(*resolved.auto_compact_token_limit_tokens) : json::value(nullptr);
    if (resolved.effective_context_window_percent) row["effective_context_window_percent"] = *resolved.effective_context_window_percent;
    row["context_source"] = resolved.context_source; row["reasoning_source"] = resolved.reasoning_source;
    return row;
  }
  template<class SnapshotType> json::array discoveryRows(const std::optional<SnapshotType>& snapshot) { json::array rows; if (snapshot) for (const auto& model : snapshot->models) rows.push_back(json::value_from(model)); return rows; }
  rt::Task<Snapshot> fullOrRuntimeSnapshot() { auto snapshot = co_await cm::loadFullCodexModelsSnapshot(); if (!snapshot) snapshot = co_await cm::loadCodexModelsSnapshot(); co_return snapshot; }
}
rt::Task<rt::Response> handleModels(std::optional<rt::Request> req) {
  if (req) { const auto version = ::uos::catalog::index::getCatalogClientVersion(*req); if (version) { auto task = ::uos::catalog::index::handleCodexCatalogModels(*req, *version); auto response = co_await std::move(task); co_return response; } }
  openrouter_models::warmOpenRouterModels();
  auto selection = co_await sel::loadProviderSelectionCached(); auto snapshot = co_await cm::loadCodexModelsSnapshot();
  std::optional<json::object> normalized; if (snapshot) if (const auto* rows = rt::getArray(*snapshot, "models"); rows && !rows->empty()) normalized = norm::normalizeModelList(json::value(*snapshot));
  json::array codexModels; if (sel::isProviderEnabled("codex", selection) && normalized) if (auto* rows = rt::getArray(*normalized, "data")) codexModels = *rows;
  auto merged = norm::withConfiguredCerebrasModel(codexModels, sel::isProviderEnabled("cerebras", selection));
  merged = norm::withConfiguredGeminiModels(merged, sel::isProviderEnabled("gemini", selection));
  merged = norm::withConfiguredDeepSeekModels(merged, sel::isProviderEnabled("deepseek", selection)); merged = norm::withConfiguredLithosModels(merged, sel::isProviderEnabled("lithos", selection)); merged = norm::withConfiguredDeepSeekWaterfallModels(merged, deepseek::waterfall::deepSeekWaterfallCatalogEnabled(selection));
  auto m = sel::isProviderEnabled("openlux", selection) ? rt::Promise(provider::metered::fetchMeteredModels()) : rt::Promise<std::optional<provider::metered::MeteredModelsSnapshot>>::resolve(std::nullopt);
  auto s = sel::isProviderEnabled("surplus", selection) ? rt::Promise(provider::surplus::fetchSurplusModels()) : rt::Promise<std::optional<provider::surplus::SurplusModelsSnapshot>>::resolve(std::nullopt);
  rt::Promise<Claude> c(enabledClaudeCatalog(selection)); auto all = co_await rt::all(m, s, c).wait();
  const std::vector<cc::ClaudeCodeModel> empty; const auto& claude = std::get<2>(all); appendClaudeModels(merged, claude ? claude->models : empty);
  auto paid = discoveryRows(std::get<0>(all)); const auto surplus = discoveryRows(std::get<1>(all)); for (const auto& row : surplus) paid.push_back(row);
  for (const auto& model : paid) {
    const auto* endpoints = rt::getArray(model, "supported_endpoint_types"); const auto id = rt::getString(model, "id").value_or("");
    if (!endpoints || (!rt::includes(*endpoints, json::value("openai")) && !rt::includes(*endpoints, json::value("openai-response"))) || hasId(merged, id)) continue;
    merged.push_back(json::object{{"id", *rt::get(model, "id")}, {"object", "model"}, {"created", *rt::get(model, "created")}, {"owned_by", *rt::get(model, "owned_by")}});
  }
  auto kp = kv::getKv(); auto database = co_await kp.wait(); std::optional<codex_models_whitelist::CodexModelsWhitelist> wl; if (database) wl = co_await codex_models_whitelist::loadCodexModelsWhitelist(database);
  auto data = codex_models_whitelist::filterWhitelistedModelList(merged, wl);
  const auto openrouter = sel::isProviderEnabled("openrouter", selection) ? norm::configuredOpenRouterModels() : json::array{};
  std::unordered_set<std::string> seen; for (const auto& row : data) seen.emplace(rt::getString(row, "id").value_or(""));
  for (const auto& row : openrouter) if (!seen.contains(std::string(rt::getString(row, "id").value_or("")))) data.push_back(row);
  co_return http::json(200, json::object{{"object", "list"}, {"data", data}}, rt::Headers{{"x-uos-upstream", snapshotUpstreamSource(snapshot)}});
}
rt::Task<ModelCatalogSnapshot> buildModelCatalogSnapshot() {
  openrouter_models::warmOpenRouterModels(); auto snapshot = co_await fullOrRuntimeSnapshot();
  std::optional<json::object> normalized; if (snapshot) if (const auto* rows = rt::getArray(*snapshot, "models"); rows && !rows->empty()) normalized = norm::normalizeModelList(json::value(*snapshot));
  rt::Promise<std::optional<provider::metered::MeteredModelsSnapshot>> m(provider::metered::fetchMeteredModels());
  provider::surplus::FetchSurplusModelsOptions options; options.requireApiKey = false;
  rt::Promise<std::optional<provider::surplus::SurplusModelsSnapshot>> s(provider::surplus::fetchSurplusModels(options));
  rt::Promise<Claude> c(cc::fetchClaudeCodeModels()); auto kp = kv::getKv(); auto database = co_await kp.wait();
  rt::Promise<std::optional<provider::claude_code_credentials::ClaudeCodeResolvedCredential>> credential(provider::claude_code_credentials::resolveClaudeCodeCredential(database));
  auto all = co_await rt::all(m, s, c, credential).wait(); const auto& metered = std::get<0>(all); const auto& surplus = std::get<1>(all); const auto& claude = std::get<2>(all);
  ModelMap models; const auto records = collectCodexSnapshotRecords(snapshot); json::array codexModels;
  if (normalized) if (const auto* rows = rt::getArray(*normalized, "data")) codexModels = *rows;
  for (const auto& model : codexModels) {
    const auto id = rt::getString(model, "id"); if (!id || id->empty()) continue;
    const auto* record = records.get(*id); metadata::ModelMetadataSources sources; sources.codex = rt::orNull(metadata::codexSnapshotMetadataHint(record)); sources.codexSubscription = metadata::codexSubscriptionMetadataHint();
    addPublicModelCatalogEntry(models, std::string(*id), {"codex", std::string(rt::getString(model, "owned_by").value_or("openai")), {"/v1/responses", "/v1/chat/completions"}}, *rt::get(model, "created"), sources);
  }
  if (metered) for (const auto& model : metered->models) addPublicModelCatalogEntry(models, model.id, {"openlux", model.owned_by, providerSupportedEndpointPaths(model.supported_endpoint_types)}, model.created);
  if (surplus) for (const auto& model : surplus->models) addPublicModelCatalogEntry(models, model.id, {"surplus", model.owned_by, providerSupportedEndpointPaths(model.supported_endpoint_types)}, model.created);
  if (claude) for (const auto& model : claude->models) {
    metadata::ModelMetadataHint hint; hint.context_window_tokens = rt::orUndefined(model.context_window_tokens); hint.max_context_window_tokens = rt::orUndefined(model.context_window_tokens); metadata::ModelMetadataSources sources; sources.provider = hint;
    addPublicModelCatalogEntry(models, model.id, {"claude_code", "anthropic", {"/v1/responses", "/v1/chat/completions"}}, model.created, sources);
  }
  const auto counts = addCredentialGatedCatalogProviders(models); addDeepSeekWaterfallCatalogEntries(models); const auto openrouter = addOpenRouterCatalogEntries(models);
  ModelCatalogSnapshot result; for (const auto& [id, row] : models) { static_cast<void>(id); result.models.push_back(row); }
  std::stable_sort(result.models.begin(), result.models.end(), [](const auto& a, const auto& b) { return rt::localeCompare(a.id, b.id) < 0; });
  const auto snapshotTime = snapshot ? rt::getNumber(*snapshot, "updated_at_ms") : std::nullopt;
  result.sources.set("codex", {.status = catalogAvailabilityStatus(normalized.has_value()), .count = static_cast<double>(codexModels.size()), .updated_at_ms = snapshotTime});
  ModelCatalogSource claudeSource{.status = catalogAvailabilityStatus(claude && !claude->stale.value_or(false)), .count = claude ? static_cast<double>(claude->models.size()) : 0, .updated_at_ms = claude ? std::optional(claude->updated_at_ms) : std::nullopt, .configured = std::get<3>(all).has_value()}; if (claude && claude->stale.value_or(false)) claudeSource.stale = true; result.sources.set("claude_code", claudeSource);
  result.sources.set("openlux", {.status = catalogAvailabilityStatus(metered.has_value()), .count = metered ? static_cast<double>(metered->models.size()) : 0, .updated_at_ms = metered ? std::optional(metered->updated_at_ms) : std::nullopt});
  result.sources.set("surplus", {.status = catalogAvailabilityStatus(surplus.has_value()), .count = surplus ? static_cast<double>(surplus->models.size()) : 0, .updated_at_ms = surplus ? std::optional(surplus->updated_at_ms) : std::nullopt});
  if (counts.gemini > 0) result.sources.set("gemini", credentialGatedCatalogSource(true, counts.gemini));
  result.sources.set("deepseek", credentialGatedCatalogSource(deepseek::index::readDeepSeekApiKey().has_value(), counts.deepseek)); result.sources.set("cerebras", credentialGatedCatalogSource(provider::cerebras::readCerebrasApiKey().has_value(), counts.cerebras)); result.sources.set("lithos", credentialGatedCatalogSource(provider::lithos::readLithosApiKey().has_value(), counts.lithos));
  auto orSource = credentialGatedCatalogSource(provider::openrouter::readOpenRouterApiKey().has_value(), openrouter); auto orSnapshot = openrouter_models::openRouterModelsSnapshot(); orSource.updated_at_ms = orSnapshot ? std::optional(orSnapshot->updated_at_ms) : std::nullopt; result.sources.set("openrouter", orSource);
  co_return result;
}
authority::contracts::Record<ModelCatalogSource> selectedCatalogSources(const authority::contracts::Record<ModelCatalogSource>& sources, const Selection& selection) {
  auto adjusted = sources; if (!selection || selection->provider_ids.empty()) return adjusted;
  for (auto id : sel::SELECTABLE_PROVIDER_IDS) if (!sel::isProviderEnabled(id, selection) && adjusted.has(id)) adjusted.set(std::string(id), {.status = "unavailable", .count = 0, .updated_at_ms = std::nullopt, .configured = false, .disabled = true});
  return adjusted;
}
rt::Task<rt::Response> handlePublicModelCatalog() {
  rt::Promise<ModelCatalogSnapshot> c(buildModelCatalogSnapshot()); rt::Promise<Selection> s(sel::loadProviderSelectionCached()); auto all = co_await rt::all(c, s).wait();
  const auto& catalog = std::get<0>(all); const auto& selection = std::get<1>(all); auto kp = kv::getKv(); auto database = co_await kp.wait();
  std::optional<codex_models_whitelist::CodexModelsWhitelist> wl; if (database) wl = co_await codex_models_whitelist::loadCodexModelsWhitelist(database);
  const auto selected = sel::filterCatalogEntriesByProviderSelection(catalog.models, selection); auto data = codex_models_whitelist::filterWhitelistedModelMap(selected, wl);
  std::stable_sort(data.begin(), data.end(), [](const auto& a, const auto& b) { return rt::localeCompare(a.id, b.id) < 0; });
  ModelCatalogSnapshot projected; projected.sources = selectedCatalogSources(catalog.sources, selection); auto encoded = json::value_from(projected);
  co_return http::json(200, json::object{{"object", "uos.model_catalog"}, {"data", json::value_from(data)}, {"sources", *rt::get(encoded, "sources")}});
}
rt::Task<rt::Response> handleModelCapabilities() {
  openrouter_models::warmOpenRouterModels(); auto selection = co_await sel::loadProviderSelectionCached(); auto snapshot = co_await fullOrRuntimeSnapshot(); json::array data;
  if (snapshot && sel::isProviderEnabled("codex", selection)) if (const auto* rows = rt::getArray(*snapshot, "models")) for (const auto& row : *rows) { auto entry = norm::normalizeModelCapabilitiesEntry(row); if (entry) data.push_back(*entry); }
  if (sel::isProviderEnabled("cerebras", selection)) for (auto id : provider::cerebras::CEREBRAS_MODELS) { auto row = norm::configuredCerebrasModelCapabilities(std::string(id)); if (row && !hasId(data, id)) data.push_back(*row); }
  auto m = sel::isProviderEnabled("openlux", selection) ? rt::Promise(provider::metered::fetchMeteredModels()) : rt::Promise<std::optional<provider::metered::MeteredModelsSnapshot>>::resolve(std::nullopt);
  auto s = sel::isProviderEnabled("surplus", selection) ? rt::Promise(provider::surplus::fetchSurplusModels()) : rt::Promise<std::optional<provider::surplus::SurplusModelsSnapshot>>::resolve(std::nullopt);
  rt::Promise<Claude> c(enabledClaudeCatalog(selection)); auto all = co_await rt::all(m, s, c).wait(); const auto& claude = std::get<2>(all); const std::vector<cc::ClaudeCodeModel> empty;
  std::unordered_set<std::string> seen; for (const auto& row : data) seen.emplace(rt::getString(row, "id").value_or(""));
  for (const auto& row : claudeCapabilities(claude ? claude->models : empty)) if (!seen.contains(std::string(rt::getString(row, "id").value_or("")))) data.push_back(row);
  for (const auto& row : discoveryRows(std::get<0>(all))) if (!hasId(data, rt::getString(row, "id").value_or(""))) data.push_back(discoveredModelCapabilitiesEntry(row.as_object(), "metered"));
  for (const auto& row : discoveryRows(std::get<1>(all))) if (!hasId(data, rt::getString(row, "id").value_or(""))) data.push_back(discoveredModelCapabilitiesEntry(row.as_object(), "surplus"));
  data = norm::withConfiguredGeminiCapabilities(data, sel::isProviderEnabled("gemini", selection));
  data = norm::withConfiguredDeepSeekCapabilities(data, sel::isProviderEnabled("deepseek", selection)); data = norm::withConfiguredLithosCapabilities(data, sel::isProviderEnabled("lithos", selection)); data = norm::withConfiguredDeepSeekWaterfallCapabilities(data, deepseek::waterfall::deepSeekWaterfallCatalogEnabled(selection));
  auto kp = kv::getKv(); auto database = co_await kp.wait(); std::optional<codex_models_whitelist::CodexModelsWhitelist> wl; if (database) wl = co_await codex_models_whitelist::loadCodexModelsWhitelist(database);
  auto filtered = codex_models_whitelist::filterWhitelistedModelList(data, wl); seen.clear(); for (const auto& row : filtered) seen.emplace(rt::getString(row, "id").value_or(""));
  const auto openrouter = sel::isProviderEnabled("openrouter", selection) ? norm::configuredOpenRouterModelCapabilities() : json::array{};
  for (const auto& row : openrouter) if (!seen.contains(std::string(rt::getString(row, "id").value_or("")))) filtered.push_back(row);
  json::object result{{"object", "list"}, {"data", filtered}, {"upstream_provider", "codex_chatgpt"}, {"source", snapshot ? rt::getString(*snapshot, "source").value_or("stored_codex_models") : std::string_view("stored_codex_models")}, {"client_version", snapshot && rt::get(*snapshot, "client_version") ? *rt::get(*snapshot, "client_version") : json::value(nullptr)}, {"updated_at_ms", snapshot && rt::get(*snapshot, "updated_at_ms") ? *rt::get(*snapshot, "updated_at_ms") : json::value(nullptr)}};
  co_return http::json(200, result, rt::Headers{{"x-uos-upstream", snapshotUpstreamSource(snapshot)}});
}
void tag_invoke(const json::value_from_tag&, json::value& out, const PublicModelProvider& v) { out = Writer().required("id", v.id).required("owned_by", v.owned_by).required("supported_endpoints", v.supported_endpoints).done(); }
PublicModelProvider tag_invoke(const json::value_to_tag<PublicModelProvider>&, const json::value& v) { const Reader r(v, "PublicModelProvider"); return {r.required<std::string>("id"), r.required<std::string>("owned_by"), r.required<std::vector<std::string>>("supported_endpoints")}; }
void tag_invoke(const json::value_from_tag&, json::value& out, const PublicModelCatalogEntry& v) { out = Writer().required("id", v.id).required("providers", v.providers).orUndefined("created", v.created).orUndefined("context_window_tokens", v.context_window_tokens).orUndefined("max_context_window_tokens", v.max_context_window_tokens).orUndefined("auto_compact_token_limit_tokens", v.auto_compact_token_limit_tokens).orUndefined("effective_context_window_percent", v.effective_context_window_percent).orUndefined("supported_reasoning_levels", v.supported_reasoning_levels).orUndefined("default_reasoning_effort", v.default_reasoning_effort).orUndefined("context_source", v.context_source).orUndefined("reasoning_source", v.reasoning_source).done(); }
PublicModelCatalogEntry tag_invoke(const json::value_to_tag<PublicModelCatalogEntry>&, const json::value& v) { const Reader r(v, "PublicModelCatalogEntry"); return {.id = r.required<std::string>("id"), .providers = r.required<std::vector<PublicModelProvider>>("providers"), .created = r.orUndefined<double>("created"), .context_window_tokens = r.orUndefined<double>("context_window_tokens"), .max_context_window_tokens = r.orUndefined<double>("max_context_window_tokens"), .auto_compact_token_limit_tokens = r.orUndefined<double>("auto_compact_token_limit_tokens"), .effective_context_window_percent = r.orUndefined<double>("effective_context_window_percent"), .supported_reasoning_levels = r.orUndefined<std::vector<std::string>>("supported_reasoning_levels"), .default_reasoning_effort = r.orUndefined<std::string>("default_reasoning_effort"), .context_source = r.orUndefined<std::string>("context_source"), .reasoning_source = r.orUndefined<std::string>("reasoning_source")}; }
void tag_invoke(const json::value_from_tag&, json::value& out, const ModelCatalogSource& v) { out = Writer().required("status", v.status).required("count", v.count).orNull("updated_at_ms", v.updated_at_ms).orUndefined("configured", v.configured).orUndefined("disabled", v.disabled).orUndefined("stale", v.stale).done(); }
ModelCatalogSource tag_invoke(const json::value_to_tag<ModelCatalogSource>&, const json::value& v) { const Reader r(v, "ModelCatalogSource"); return {.status = r.required<std::string>("status"), .count = r.required<double>("count"), .updated_at_ms = r.orNull<double>("updated_at_ms"), .configured = r.orUndefined<bool>("configured"), .disabled = r.orUndefined<bool>("disabled"), .stale = r.orUndefined<bool>("stale")}; }
void tag_invoke(const json::value_from_tag&, json::value& out, const ModelCatalogSnapshot& v) { json::object sources; for (const auto& id : v.sources.keys()) rt::objectDefine(sources, id, json::value_from(*v.sources.get(id))); out = Writer().required("models", v.models).required("sources", sources).done(); }
ModelCatalogSnapshot tag_invoke(const json::value_to_tag<ModelCatalogSnapshot>&, const json::value& v) { const Reader r(v, "ModelCatalogSnapshot"); ModelCatalogSnapshot result; result.models = r.required<std::vector<PublicModelCatalogEntry>>("models"); auto sources = r.required<json::object>("sources"); for (const auto& [id, source] : sources) result.sources.define(std::string(id), json::value_to<ModelCatalogSource>(source)); return result; }
}
