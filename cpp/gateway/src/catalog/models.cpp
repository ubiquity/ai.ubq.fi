#include "uos/catalog/models.hpp"
#include "uos/catalog/types.hpp"
#include "uos/codex/auth.hpp"
#include "uos/deepseek/forwarded_payload_policy.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/input_normalization.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/models/metadata.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/recent_model_context.hpp"
#include "uos/request_policy.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"
#include <algorithm>

namespace uos::catalog::models {
namespace {
  std::string modelId(const json::value& model, bool trim = false) {
    auto value = rt::getString(model, "slug");
    if (!value) value = rt::getString(model, "id");
    if (!value) value = rt::getString(model, "model");
    if (!value) value = rt::getString(model, "name");
    return trim ? rt::trim(value.value_or("")) : std::string(value.value_or(""));
  }
  void shellDefaults(json::object& result, double priority, bool forwarding = false) {
    result["shell_type"] = "shell_command";
    result["visibility"] = "list";
    result["supported_in_api"] = true;
    result["priority"] = priority;
    result["availability_nux"] = nullptr;
    result["upgrade"] = nullptr;
    result["base_instructions"] = "";
    result["support_verbosity"] = false;
    result["default_verbosity"] = nullptr;
    result["apply_patch_tool_type"] = nullptr;
    result["web_search_tool_type"] = "text";
    result["truncation_policy"] = json::object{{"mode", "tokens"}, {"limit", 10000}};
    if (forwarding) {
      const auto& policy = deepseek::forwarded_payload_policy::FORWARDED_PAYLOAD_POLICY;
      result["forwarding_policy"] = json::object{{"version", std::string(policy.version)}, {"mode", std::string(policy.mode)}, {"per_message_limit", policy.perMessageLimit}};
    }
    result["supports_parallel_tool_calls"] = false;
    result["experimental_supported_tools"] = json::array{};
  }
  std::string codexReasoningEffortDescription(const std::string& effort) {
    return effort == "none" ? "No reasoning" : "Reasoning effort: " + effort;
  }
  std::optional<std::vector<std::string>> ascendingExternalReasoningLevels(const std::optional<std::vector<std::string>>& levels) {
    if (!levels) return std::nullopt;
    static const std::vector<std::string> standardOrder{"none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"};
    auto rank = [](const std::string& effort) { const auto at = std::find(standardOrder.begin(), standardOrder.end(), effort); return at == standardOrder.end() ? -1 : static_cast<int>(at - standardOrder.begin()); };
    std::vector<std::string> standardLevels;
    for (const auto& effort : *levels) if (rank(effort) >= 0) standardLevels.push_back(effort);
    std::stable_sort(standardLevels.begin(), standardLevels.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
    auto result = *levels;
    std::size_t index = 0;
    for (auto& effort : result) if (rank(effort) >= 0) effort = standardLevels[index++];
    return result;
  }
  json::array reasoningLevels(const std::optional<std::vector<std::string>>& levels, bool fallback = true) {
    json::array result;
    if (levels) for (const auto& effort : *levels) result.push_back(json::object{{"effort", effort}, {"description", codexReasoningEffortDescription(effort)}});
    if (result.empty() && fallback) result.push_back(json::object{{"effort", "none"}, {"description", "No reasoning"}});
    return result;
  }
  void defaultReasoning(json::object& result, const ::uos::models::metadata::ResolvedModelMetadata& resolved) {
    if (resolved.default_reasoning_effort || !resolved.supported_reasoning_levels) result["default_reasoning_level"] = resolved.default_reasoning_effort.value_or("none");
  }
  void contextWindow(json::object& result, const ::uos::models::metadata::ResolvedModelMetadata& resolved) {
    if (!resolved.context_window_tokens) return;
    result["context_window"] = *resolved.context_window_tokens;
    result["max_context_window"] = resolved.max_context_window_tokens ? json::value(*resolved.max_context_window_tokens) : json::value(nullptr);
    if (resolved.auto_compact_token_limit_tokens) result["auto_compact_token_limit"] = *resolved.auto_compact_token_limit_tokens;
    if (resolved.effective_context_window_percent) result["effective_context_window_percent"] = *resolved.effective_context_window_percent;
  }
  json::array appendConfigured(json::array models, const json::array& configured) {
    if (configured.empty()) return models;
    rt::OrderedSet<std::string> present;
    for (const auto& model : models) present.add(std::string(rt::getString(model, "slug").value_or(rt::getString(model, "id").value_or(""))));
    for (const auto& model : configured) if (!present.has(rt::toString(rt::get(model, "slug")))) models.push_back(model);
    return models;
  }
}

json::array claudeCodeCodexModels(const std::vector<provider::claude_code_models::ClaudeCodeModel>& models) {
  json::array result;
  for (const auto& model : models) {
    json::array levels;
    for (const auto& effort : provider::claude_code_models::claudeCodeReasoningLevels(model)) levels.push_back(json::object{{"effort", effort}, {"description", effort == "none" ? "No reasoning" : effort}});
    json::object row{{"slug", model.id}, {"display_name", model.display_name.value_or(model.id)}, {"owned_by", "anthropic"},
      {"supported_endpoint_types", json::array{"openai-response", "openai"}}, {"supported_reasoning_levels", levels}};
    if (model.supports_disabled_thinking.value_or(false)) row["default_reasoning_level"] = "none";
    if (model.context_window_tokens && rt::truthy(*model.context_window_tokens)) { row["context_window"] = *model.context_window_tokens; row["max_context_window"] = *model.context_window_tokens; }
    if (model.max_output_tokens && rt::truthy(*model.max_output_tokens)) row["max_output_tokens"] = *model.max_output_tokens;
    shellDefaults(row, 1000);
    result.push_back(std::move(row));
  }
  return result;
}
json::array withClaudeCodeModels(json::array models, const std::vector<provider::claude_code_models::ClaudeCodeModel>& claude) {
  if (claude.empty()) return models;
  const auto seen = catalogModelIds(models);
  for (const auto& model : claudeCodeCodexModels(claude)) if (!seen.has(rt::toString(rt::get(model, "slug")))) models.push_back(model);
  return models;
}
rt::Task<void> maybeUpdateNormalizedSnapshot(rt::Kv& kv, std::string version, std::string authGeneration, json::object parsed, double updatedAtMs) {
  ::uos::models::codex_models::CodexModelsPayloadOverrides overrides;
  overrides.source = "chatgpt_codex"; overrides.clientVersion = version; overrides.updatedAtMs = updatedAtMs;
  const auto next = ::uos::models::codex_models::normalizeCodexModelsPayload(json::value(std::move(parsed)), overrides);
  if (!next) co_return;
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto generation = co_await kv.get(catalog::types::CODEX_CATALOG_AUTH_GENERATION_KEY);
    if (!generation.value.isString() || generation.value.asString() != authGeneration) co_return;
    rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
    rt::Promise<rt::KvEntryMaybe<>> currentTask(kv.get(codex::auth::CODEX_MODELS_KV_KEY, strong));
    rt::Promise<rt::KvEntryMaybe<>> runtimeTask(kv.get(runtime_config::RUNTIME_CONFIG_V2_KEY, strong));
    auto both = co_await rt::all(currentTask, runtimeTask).wait();
    const auto& current = std::get<0>(both); const auto& runtimeEntry = std::get<1>(both);
    auto raw = rt::storedJsonView(current.value);
    std::optional<::uos::models::codex_models::CodexModelsSnapshot> previous;
    if (raw.is_object()) previous = raw.as_object();
    const auto currentVersion = rt::getString(raw, "client_version");
    if (currentVersion && !currentVersion->empty()) {
      auto comparison = ::uos::models::codex_models::compareCodexClientVersions(version, *currentVersion);
      if (!comparison || *comparison < 0) co_return;
    }
    auto nextWithPromptCacheEvidence = ::uos::models::codex_models::mergeCodexModelPromptCacheCapabilities(*next, previous);
    auto currentRuntime = runtime_config::normalizeRuntimeConfig(runtimeEntry.value);
    runtime_config::BuildRuntimeConfigOptions options;
    options.defaultModel = ::uos::models::codex_models_fetch::preserveCodexDefaultModel(nextWithPromptCacheEvidence, currentRuntime ? std::optional(currentRuntime->default_model) : std::nullopt);
    options.defaultReasoningEffort = currentRuntime ? std::optional(currentRuntime->default_reasoning_effort) : std::nullopt;
    options.nowMs = updatedAtMs;
    auto nextRuntime = runtime_config::buildRuntimeConfig(nextWithPromptCacheEvidence, options);
    auto operation = kv.atomic();
    operation->check(generation).check(current).check(runtimeEntry).set(codex::auth::CODEX_MODELS_KV_KEY, rt::KvValue(nextWithPromptCacheEvidence)).set(runtime_config::RUNTIME_CONFIG_V2_KEY, rt::KvValue(runtime_config::toJson(nextRuntime)));
    auto commit = co_await operation->commit();
    if (commit.ok) { runtime_config::cacheRuntimeConfig(nextRuntime); co_return; }
  }
}
bool etagMatches(const std::optional<std::string>& requestValue, const std::optional<std::string>& etag) {
  if (!requestValue || requestValue->empty() || !etag || etag->empty()) return false;
  for (const auto& candidate : rt::split(*requestValue, ",")) if (rt::trim(candidate) == "*" || rt::trim(candidate) == *etag) return true;
  return false;
}
rt::OrderedMap<std::string, json::object> codexSnapshotRecords(const std::optional<::uos::models::codex_models::CodexModelsSnapshot>& snapshot) {
  rt::OrderedMap<std::string, json::object> records;
  if (snapshot) if (const auto* rows = rt::getArray(*snapshot, "models")) for (const auto& row : *rows) {
    if (!utils::isRecord(row)) continue;
    const auto id = modelId(row, true);
    if (!id.empty() && row.is_object()) records.set(id, row.as_object());
  }
  return records;
}
json::object meteredCodexModelRecord(const json::object& model, const json::object* codexRecord) {
  ::uos::models::metadata::ModelMetadataSources sources;
  if (codexRecord) { sources.codex = rt::orNull(::uos::models::metadata::codexSnapshotMetadataHint(*codexRecord)); sources.codexSubscription = ::uos::models::metadata::codexSubscriptionMetadataHint(); }
  const auto id = rt::getString(model, "id").value_or("");
  const auto resolved = ::uos::models::metadata::resolveModelMetadata(id, sources);
  const auto levels = codexRecord ? resolved.supported_reasoning_levels : ascendingExternalReasoningLevels(resolved.supported_reasoning_levels);
  json::object result{{"slug", id}, {"display_name", id}};
  if (const auto* description = rt::get(model, "description")) result["description"] = *description;
  result["owned_by"] = *rt::get(model, "owned_by");
  result["supported_endpoint_types"] = *rt::get(model, "supported_endpoint_types");
  result["supported_reasoning_levels"] = reasoningLevels(levels);
  defaultReasoning(result, resolved); contextWindow(result, resolved); shellDefaults(result, 1000);
  return result;
}
json::array uniqueResponsesModels(const json::array& models) {
  rt::OrderedSet<std::string> seen;
  json::array result;
  for (const auto& model : models) {
    const auto* endpoints = rt::getArray(model, "supported_endpoint_types");
    const auto id = rt::getString(model, "id").value_or("");
    if (!endpoints || !rt::includes(*endpoints, json::value("openai-response")) || seen.has(id)) continue;
    seen.add(std::string(id)); result.push_back(model);
  }
  return result;
}
json::array deepSeekOfficialCodexModels() {
  json::array result;
  if (!deepseek::index::readDeepSeekApiKey()) return result;
  for (auto idView : deepseek::index::DEEPSEEK_OFFICIAL_MODEL_IDS) {
    std::string id(idView);
    ::uos::models::metadata::ModelMetadataSources sources;
    ::uos::models::metadata::ModelMetadataHint hint;
    hint.context_window_tokens = deepseek::index::DEEPSEEK_CONTEXT_WINDOW_TOKENS; hint.max_context_window_tokens = deepseek::index::DEEPSEEK_CONTEXT_WINDOW_TOKENS; sources.provider = hint;
    auto resolved = ::uos::models::metadata::resolveModelMetadata(id, sources);
    const auto* display = deepseek::index::DEEPSEEK_DISPLAY_NAMES.get(id);
    json::object row{{"slug", id}, {"display_name", display ? *display : id}, {"description", "DeepSeek official API (deepseek-flash) served by this gateway."}, {"owned_by", "deepseek"}, {"supported_endpoint_types", json::array{"openai-response", "openai-chat"}},
      {"supported_reasoning_levels", json::array{json::object{{"effort", "none"}, {"description", "Disable thinking mode"}}, json::object{{"effort", "low"}, {"description", "Thinking effort: low"}}, json::object{{"effort", "high"}, {"description", "Thinking effort: high"}}, json::object{{"effort", "max"}, {"description", "Thinking effort: maximum"}}}}, {"default_reasoning_level", "high"}};
    contextWindow(row, resolved); shellDefaults(row, 1, true); result.push_back(std::move(row));
  }
  return result;
}
json::array withDeepSeekOfficialModels(json::array models) {
  const auto configured = deepSeekOfficialCodexModels();
  if (configured.empty()) return models;
  auto id = [](const json::value& row) { auto slug = rt::trim(rt::getString(row, "slug").value_or("")); return !slug.empty() ? slug : rt::trim(rt::getString(row, "id").value_or("")); };
  rt::OrderedMap<std::string, json::value> configuredById;
  for (const auto& row : configured) configuredById.set(id(row), row);
  rt::OrderedSet<std::string> present;
  for (auto& row : models) {
    const auto name = id(row);
    present.add(name);
    if (const auto* replacement = configuredById.get(name)) row = *replacement;
  }
  for (const auto& row : configured) if (!present.has(id(row))) models.push_back(row);
  return models;
}
json::object buildDeepSeekWaterfallCodexRecord() {
  namespace w = deepseek::waterfall;
  json::array levels;
  for (auto effort : w::DEEPSEEK_WATERFALL_REASONING_LEVELS) levels.push_back(json::object{{"effort", std::string(effort)}, {"description", codexReasoningEffortDescription(std::string(effort))}});
  const auto window = w::DEEPSEEK_WATERFALL_CONTEXT_WINDOW_TOKENS;
  json::object row{
    {"slug", w::DEEPSEEK_WATERFALL_MODEL_ID},
    {"display_name", w::DEEPSEEK_WATERFALL_DISPLAY_NAME},
    {"description", w::DEEPSEEK_WATERFALL_DESCRIPTION},
    {"owned_by", "ubiquity"},
    {"supported_endpoint_types", json::array{"openai-response"}},
    {"supported_reasoning_levels", std::move(levels)},
    {"default_reasoning_level", w::DEEPSEEK_WATERFALL_DEFAULT_REASONING_LEVEL},
    {"context_window", window},
    {"max_context_window", window},
    {"auto_compact_token_limit", recent_model_context::deriveAutoCompactTokenLimit(window)},
    {"effective_context_window_percent", w::DEEPSEEK_WATERFALL_EFFECTIVE_CONTEXT_WINDOW_PERCENT},
  };
  shellDefaults(row, 0); return row;
}
json::array withDeepSeekWaterfallModel(json::array models) {
  const json::value record = buildDeepSeekWaterfallCodexRecord();
  bool present = false;
  for (auto& row : models) {
    if (rt::getString(row, "slug").value_or(rt::getString(row, "id").value_or("")) != deepseek::waterfall::DEEPSEEK_WATERFALL_MODEL_ID) continue;
    row = record;
    present = true;
  }
  if (!present) models.push_back(record);
  return models;
}
json::array lithosCodexModels() {
  json::array result;
  if (!provider::lithos::readLithosApiKey()) return result;
  for (auto idView : provider::lithos::LITHOS_MODEL_IDS) {
    std::string id(idView);
    ::uos::models::metadata::ModelMetadataSources sources;
    ::uos::models::metadata::ModelMetadataHint hint;
    hint.context_window_tokens = provider::lithos::LITHOS_CONTEXT_WINDOW_TOKENS; hint.max_context_window_tokens = provider::lithos::LITHOS_CONTEXT_WINDOW_TOKENS; hint.effective_context_window_percent = provider::lithos::LITHOS_EFFECTIVE_CONTEXT_WINDOW_PERCENT; sources.provider = hint;
    auto resolved = ::uos::models::metadata::resolveModelMetadata(id, sources);
    std::vector<std::string> levels; for (auto effort : provider::lithos::LITHOS_REASONING_LEVELS) levels.emplace_back(effort);
    const auto* display = provider::lithos::LITHOS_DISPLAY_NAMES.get(id);
    json::object row{{"slug", id}, {"display_name", display ? *display : id}, {"description", "LithosAI API (Chat Completions) served by this gateway."}, {"owned_by", "lithos"}, {"supported_endpoint_types", json::array{"openai-response", "openai-chat"}}, {"supported_reasoning_levels", reasoningLevels(levels)}, {"default_reasoning_level", std::string(provider::lithos::LITHOS_DEFAULT_REASONING_EFFORT)}};
    contextWindow(row, resolved); shellDefaults(row, 1, true); result.push_back(std::move(row));
  }
  return result;
}
json::array withLithosModels(json::array models) { return appendConfigured(std::move(models), lithosCodexModels()); }
json::array cerebrasCodexModels() {
  json::array result;
  if (!provider::cerebras::readCerebrasApiKey()) return result;
  for (auto idView : provider::cerebras::CEREBRAS_MODELS) {
    std::string id(idView);
    auto hint = request_policy::cerebrasProviderHint(id);
    ::uos::models::metadata::ModelMetadataSources sources; sources.provider = hint; sources.openRouter = nullptr;
    auto resolved = ::uos::models::metadata::resolveModelMetadata(id, sources);
    std::vector<std::string> levels; if (hint.supported_reasoning_levels.hasValue()) for (const auto& effort : *hint.supported_reasoning_levels) levels.push_back(rt::toString(effort));
    const auto* display = input_normalization::CEREBRAS_MODEL_DISPLAY_NAMES.get(id);
    json::object row{{"slug", id}, {"display_name", display ? *display : id}, {"description", "Cerebras API (Chat Completions) served by this gateway."}, {"owned_by", "cerebras"}, {"supported_endpoint_types", json::array{"openai-response", "openai-chat"}}, {"supported_reasoning_levels", reasoningLevels(levels, false)}};
    defaultReasoning(row, resolved); contextWindow(row, resolved); shellDefaults(row, 1, true); result.push_back(std::move(row));
  }
  return result;
}
json::array withCerebrasModels(json::array models) { return appendConfigured(std::move(models), cerebrasCodexModels()); }
json::array openRouterCodexModels() {
  json::array result;
  if (!provider::openrouter::readOpenRouterApiKey()) return result;
  auto snapshot = ::uos::models::openrouter_models::openRouterModelsSnapshot();
  if (!snapshot) return result;
  for (const auto& model : snapshot->models) {
    if (!::uos::models::openrouter_models::openRouterModelSupportsText(model)) continue;
    auto resolved = ::uos::models::metadata::resolveModelMetadata(model.id);
    const auto owned = rt::split(model.id, "/")[0];
    json::object row{{"slug", model.id}, {"display_name", model.id}, {"description", "OpenRouter upstream (Chat Completions and Responses) served by this gateway."}, {"owned_by", owned.empty() ? "openrouter" : owned}, {"supported_endpoint_types", json::array{"openai-response", "openai-chat"}}, {"supported_reasoning_levels", reasoningLevels(ascendingExternalReasoningLevels(resolved.supported_reasoning_levels))}};
    defaultReasoning(row, resolved); contextWindow(row, resolved); shellDefaults(row, 1000); result.push_back(std::move(row));
  }
  return result;
}
json::array withOpenRouterModels(json::array models) { return appendConfigured(std::move(models), openRouterCodexModels()); }
rt::OrderedSet<std::string> catalogModelIds(const json::array& models) {
  rt::OrderedSet<std::string> ids;
  for (const auto& model : models) if (utils::isRecord(model)) { const auto id = modelId(model, true); if (!id.empty()) ids.add(id); }
  return ids;
}
}
