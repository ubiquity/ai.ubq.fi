#include "uos/admin/codex.hpp"
#include "uos/admin/api_keys.hpp"
#include "uos/admin/callback_task.hpp"
#include "uos/cache/prompt_analytics_read.hpp"
#include "uos/cache/scope_experiment.hpp"
#include "uos/catalog/store.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/banked_reset_pool.hpp"
#include "uos/codex/experiment.hpp"
#include "uos/debug_routing.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/models/codex_models_whitelist.hpp"
#include "uos/provider/capacity.hpp"
#include "uos/provider/gemini.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/provider/presentation.hpp"
#include "uos/request.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/string.hpp"
#include "uos/upstream_auth.hpp"
#include "uos/utils.hpp"
#include <algorithm>
#include <tuple>

namespace uos::admin::codex {
namespace {
  namespace auth = ::uos::codex::auth;
  namespace cm = models::codex_models;
  namespace cf = models::codex_models_fetch;
  namespace wl = models::codex_models_whitelist;
  namespace sel = provider::selection;
  namespace scope = cache::scope_experiment;
  namespace analytics = cache::prompt_analytics_core;
  inline const rt::KvKey UOS_CODEX_PROMPTS_KEY{"uos_ai", "codex_instructions"};
  inline const rt::KvKey UOS_CODEX_PROMPTS_CHUNK_PREFIX{"uos_ai", "codex_instructions_chunk"};
  rt::Response error(double status, const std::string& message, const std::string& code, bool server = false, bool noStore = false) {
    http::OpenaiErrorOptions options; if (server) options.type = "server_error"; if (noStore) options.headers = rt::Headers{{"Cache-Control", "no-store"}};
    return http::openaiError(status, message, code, options);
  }
  rt::Response paramError(const std::string& message, const std::string& param) { http::OpenaiErrorOptions options; options.param = std::optional(param); return http::openaiError(400, message, "invalid_request_error", options); }
  rt::Task<std::shared_ptr<rt::Kv>> databaseForRequest() { auto promise = kv::getKv(); auto result = co_await promise.wait(); co_return result; }
  json::value memberOrNull(const json::object& value, std::string_view key) { const auto* member = rt::get(value, key); return member && !member->is_null() ? *member : json::value(nullptr); }
  std::optional<json::object> storedSnapshot(const rt::KvValue& value) { auto raw = rt::storedJsonView(value); return raw.is_object() ? std::optional(raw.as_object()) : std::nullopt; }
  rt::Task<json::array> codexSubscriptionRoster() {
    json::array rows;
    try {
      const auto accounts = co_await ::uos::codex::experiment::getCodexCapacityAccounts();
      std::vector<rt::Promise<std::string>> hashes; for (const auto& account : accounts) hashes.emplace_back(sel::codexSubscriptionHash(account.account_id));
      auto resolved = co_await rt::all(std::move(hashes)).wait();
      for (std::size_t i = 0; i < accounts.size(); ++i) rows.push_back(json::object{{"id", sel::codexSubscriptionSelectionId(resolved[i])}, {"label", provider::capacity::codexAccountLabel(accounts[i].slot, accounts[i].email)}, {"slot", accounts[i].slot}});
    } catch (...) { rows.clear(); }
    co_return rows;
  }
  std::string tierLabel(const std::string& id) { for (const auto& tier : provider::presentation::PROVIDER_TIERS) if (tier.id == id) return std::string(tier.label); return id; }
}
std::optional<rt::Response> runtimeConfigErrorResponse(rt::Reason failure) {
  try { if (failure) std::rethrow_exception(failure); }
  catch (const runtime_config::RuntimeConfigError& e) {
    const auto status = rt::includes(e.message, "too large") || rt::includes(e.message, "4 KiB") ? 413 : 409;
    http::OpenaiErrorOptions options; options.type = "invalid_request_error"; return http::openaiError(status, e.message, "runtime_config_invalid", options);
  } catch (...) {}
  return std::nullopt;
}
rt::Task<rt::Response> handleAdminDebugRouting(rt::Request req) {
  if (req.method() == "GET") { const auto routing = co_await debug_routing::loadDebugRoutingConfig(); co_return http::json(200, json::object{{"routing", json::value_from(routing)}}, rt::Headers{{"Cache-Control", "no-store"}}); }
  if (req.method() == "DELETE") {
    const std::string scenario = "normal";
    const auto routing = co_await debug_routing::setDebugRoutingConfig(scenario, 0);
    co_return http::json(200, json::object{{"routing", json::value_from(routing)}}, rt::Headers{{"Cache-Control", "no-store"}});
  }
  const auto raw = co_await request::readJsonBody(req);
  if (!rt::truthy(raw) || !utils::isRecord(raw)) co_return error(400, "Invalid JSON body", "invalid_request_error");
  auto scenarioRaw = rt::getString(raw, "scenario"); const auto scenario = scenarioRaw ? rt::trim(*scenarioRaw) : std::string();
  const auto* durationRaw = rt::get(raw, "duration_ms"); const double duration = durationRaw ? rt::toNumber(durationRaw) : 15 * 60'000;
  if (scenario.empty()) co_return paramError("scenario is required", "scenario");
  if (!rt::Number::isSafeInteger(duration) || duration < 0 || duration > debug_routing::DEBUG_ROUTING_MAX_DURATION_MS) co_return paramError("duration_ms must be between 0 and 3600000", "duration_ms");
  try { const auto routing = co_await debug_routing::setDebugRoutingConfig(scenario, duration); co_return http::json(200, json::object{{"routing", json::value_from(routing)}}, rt::Headers{{"Cache-Control", "no-store"}}); }
  catch (...) { const auto reason = std::current_exception(); co_return error(400, rt::isError(reason) ? rt::errorMessage(reason).value_or("") : "Invalid debug routing scenario", "invalid_request_error"); }
}
rt::Task<rt::Response> handleAdminCodexRecheck(double slot) {
  if (!rt::Number::isInteger(slot) || slot < 1 || slot > 2) co_return error(404, "Codex account slot not found", "not_found");
  const bool accepted = co_await ::uos::codex::account_routing::recheckCodexRoutingSlot(slot);
  if (!accepted) co_return error(404, "Codex account slot is not configured", "not_found");
  co_return rt::Response(nullptr, rt::ResponseInit{.status = 204});
}
rt::Task<rt::Response> handleAdminCodexBankedResetShadowDecisions() {
  const auto decisions = co_await ::uos::codex::banked_reset_pool::listCodexResetShadowDecisions();
  if (!decisions) co_return error(503, "Codex banked-reset shadow decisions are unavailable", "codex_banked_reset_shadow_unavailable", true, true);
  co_return http::json(200, json::object{{"decisions", json::value_from(*decisions)}}, rt::Headers{{"Cache-Control", "no-store"}});
}
rt::Task<rt::Response> handleAdminCodexCacheScopeExperiment(rt::Request req) {
  if (upstream_auth::followerMode()) co_return error(409, std::string(upstream_auth::FOLLOWER_READ_ONLY), "upstream_auth_follower");
  const auto body = co_await req.text(); if (!rt::trim(body).empty()) co_return error(400, "Prompt-cache scope experiment does not accept request fields", "invalid_request_error");
  try {
    const auto baseline = co_await scope::assertPromptCacheScopeExperimentTelemetryBaseline(); auto task = scope::runPromptCacheScopeExperiment(baseline); const auto result = co_await std::move(task);
    co_return http::json(result.status == "in_progress" ? 202 : 200, json::value_from(result));
  } catch (const scope::PromptCacheScopeExperimentBusyError& e) { co_return error(409, e.message, "prompt_cache_scope_experiment_busy"); }
  catch (const scope::PromptCacheScopeExperimentUnavailableError& e) { co_return error(503, e.message, "prompt_cache_scope_experiment_unavailable", true); }
  catch (const scope::PromptCacheScopeExperimentFailedError& e) { co_return error(503, e.message, "prompt_cache_scope_experiment_failed", true); }
  catch (...) { rt::console::error("[ai.ubq.fi] Prompt-cache scope experiment could not run."); co_return error(503, "Prompt-cache scope experiment could not run.", "prompt_cache_scope_experiment_failed", true); }
}
rt::Task<rt::Response> handleAdminCodexCacheScopeExperimentTelemetryBaseline(ReadBaseline readBaseline) {
  ReadBaselineJson reader = [readBaseline]() -> rt::Task<json::value> {
    auto task = readBaseline ? readBaseline() : scope::readPromptCacheScopeExperimentTelemetryBaseline();
    const auto baseline = co_await std::move(task); co_return cache::telemetry_gate::toJson(baseline);
  };
  return handleAdminCodexCacheScopeExperimentTelemetryBaseline(reader);
}
rt::Task<rt::Response> handleAdminCodexCacheScopeExperimentTelemetryBaseline(ReadBaselineJson readBaseline) {
  try {
    auto task = readBaseline(); const auto baseline = co_await std::move(task);
    if (baseline.is_null()) throw rt::TypeError("Cannot destructure a nullish telemetry baseline");
    json::object projected;
    for (const auto key : {"status", "reason", "release", "provider", "aggregate", "routes"}) if (const auto* value = rt::get(baseline, key)) projected[key] = *value;
    co_return http::json(200, projected, rt::Headers{{"Cache-Control", "no-store"}});
  } catch (...) { rt::console::error("[ai.ubq.fi] Prompt-cache Stage 0 telemetry baseline could not be read."); co_return error(503, "Prompt-cache Stage 0 telemetry baseline could not be read.", "prompt_cache_scope_experiment_unavailable", true, true); }
}
rt::Task<rt::Response> handleAdminPromptCacheAnalytics(rt::Request req, ReadAnalytics readAnalytics) {
  const rt::URL url(req.url());
  for (const auto& key : url.searchParams().keys()) if (key != "group_by") co_return paramError("Only group_by is supported for prompt-cache analytics", key);
  const auto parameters = url.searchParams().getAll("group_by"); if (parameters.size() > 1) co_return paramError("group_by may appear only once", "group_by");
  auto groupBy = rt::split(parameters.empty() ? "key_presence" : parameters[0], ","); for (auto& group : groupBy) group = rt::trim(group);
  if (!cache::prompt_analytics_read::isValidPromptCacheAnalyticsGroupBy(groupBy)) co_return paramError("group_by must contain up to two distinct values from provider, model, route, key_presence, mode, or fallback", "group_by");
  try { analytics::PromptCacheAnalyticsReadOptions options; options.groupBy = groupBy; auto task = readAnalytics ? readAnalytics(options) : cache::prompt_analytics_read::readPromptCacheAnalytics(options); const auto view = co_await std::move(task); co_return http::json(200, json::value_from(view), rt::Headers{{"Cache-Control", "no-store"}}); }
  catch (...) { rt::console::error("[ai.ubq.fi] Prompt-cache analytics could not be read."); co_return error(503, "Prompt-cache analytics are unavailable", "prompt_cache_analytics_unavailable", true, true); }
}
rt::Task<rt::Response> handleAdminCodexAuth(rt::Request req) {
  if (upstream_auth::followerMode()) co_return error(409, std::string(upstream_auth::FOLLOWER_READ_ONLY), "upstream_auth_follower");
  auto database = co_await databaseForRequest(); if (!database) co_return error(500, "Deno KV is not available; cannot persist Codex auth", "server_error");
  auto body = co_await request::readJsonBody(req); const auto* authPayload = body.is_object() && rt::hasOwn(body.as_object(), "auth") ? rt::get(body, "auth") : &body;
  const auto* modelsPayload = body.is_object() && rt::hasOwn(body.as_object(), "models") ? rt::get(body, "models") : nullptr;
  const auto token = auth::parseCodexAuthFromAuthJson(authPayload); if (!token) co_return error(400, "Body does not look like a Codex auth.json", "invalid_request_error");
  ::uos::types::CodexAuthState seed{token->access_token, token->refresh_token, token->account_id, rt::dateNow(), token->native_owner};
  std::optional<std::string> version; if (modelsPayload && utils::isRecord(*modelsPayload)) { auto v = rt::getString(*modelsPayload, "client_version"); if (!v) v = rt::getString(*modelsPayload, "clientVersion"); if (v) version = std::string(*v); }
  std::optional<cf::ValidateCodexAuthJsonResult> validation;
  try { cf::ValidateCodexAuthJsonOptions options; options.clientVersion = version; auto task = cf::validateCodexAuthJson(seed, options); validation = co_await std::move(task); }
  catch (const auth::CodexError& e) { rt::console::error("[ai.ubq.fi] Codex auth validation failed:", e); co_return error(e.status, e.message, e.code); }
  catch (...) { const auto reason = std::current_exception(); rt::console::error("[ai.ubq.fi] Codex auth validation failed:", reason); const auto detail = rt::isError(reason) ? rt::errorMessage(reason).value_or("") : rt::reasonToString(reason); co_return error(502, detail.empty() ? "Upstream validation request failed." : "Upstream validation request failed: " + detail, "codex_upstream_unreachable"); }
  const auto* valid = std::get_if<cf::ValidateCodexAuthJsonResultOk>(&*validation); const auto* invalid = std::get_if<cf::ValidateCodexAuthJsonResultNotOk>(&*validation);
  if (invalid && invalid->status != 429) co_return error(401, "Invalid Codex auth.json (upstream " + rt::numberToString(invalid->status) + "): " + invalid->body, "invalid_api_key");
  const auto validatedAuth = valid ? valid->auth : seed; std::optional<cm::CodexModelsSnapshot> snapshot; std::optional<std::string> generation;
  if (valid) {
    cm::CodexModelsPayloadOverrides options; options.source = "chatgpt_codex"; options.clientVersion = valid->clientVersion;
    snapshot = cm::normalizeCodexModelsPayload(valid->models, options);
    if (!snapshot) co_return error(502, "Codex upstream models response did not include a non-empty model catalog", "codex_upstream_unreachable");
    const auto size = api_keys::estimateJsonSize(*snapshot); if (!size) co_return error(400, "models payload could not be serialized", "invalid_request_error");
    if (*size > api_keys::SAFE_KV_BYTES) co_return error(413, "models snapshot too large (" + rt::numberToString(*size) + " bytes; max " + rt::numberToString(api_keys::MAX_KV_BYTES) + ").", "invalid_request_error");
    generation = rt::crypto::randomUUID();
  }
  std::optional<::uos::types::CodexAuthPoolState> persistedPool; std::optional<cm::CodexModelsSnapshot> persistedSnapshot; std::optional<runtime_config::RuntimeConfigV2> runtime;
  for (int attempt = 0; attempt < 3 && !persistedPool; ++attempt) {
    try {
      rt::Promise<rt::KvEntryMaybe<>> p(database->get(auth::CODEX_AUTH_POOL_KV_KEY)); rt::Promise<rt::KvEntryMaybe<>> s(database->get(auth::CODEX_MODELS_KV_KEY)); rt::Promise<rt::KvEntryMaybe<>> r(database->get(runtime_config::RUNTIME_CONFIG_V2_KEY));
      auto all = co_await rt::all(p, s, r).wait(); const auto& pe = std::get<0>(all); const auto& se = std::get<1>(all); const auto& re = std::get<2>(all);
      auto nextPool = auth::upsertCodexAuthAccount(auth::parseCodexAuthPool(rt::storedJsonView(pe.value)), validatedAuth);
      if (!nextPool) co_return error(409, "Codex auth pool already contains two accounts; upload an auth.json for an existing account to rotate it", "codex_auth_pool_full");
      auto previous = storedSnapshot(se.value); auto next = snapshot ? std::optional(cm::mergeCodexModelPromptCacheCapabilities(*snapshot, previous)) : previous;
      if (!next) co_return error(409, "Cannot store rate-limited Codex auth without an existing model catalog", "codex_catalog_required");
      std::optional<runtime_config::RuntimeConfigV2> nextRuntime;
      if (snapshot && generation) {
        const auto current = runtime_config::normalizeRuntimeConfig(re.value); runtime_config::BuildRuntimeConfigOptions options;
        options.defaultModel = cf::preserveCodexDefaultModel(*next, current ? std::optional(current->default_model) : std::nullopt); options.defaultReasoningEffort = current ? std::optional(current->default_reasoning_effort) : std::nullopt;
        try { nextRuntime = runtime_config::buildRuntimeConfig(*next, options); } catch (...) { auto response = runtimeConfigErrorResponse(std::current_exception()); if (response) co_return *response; throw; }
      }
      auto atomic = database->atomic(); atomic->check(pe).check(se).set(auth::CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(*nextPool)));
      if (snapshot && nextRuntime && generation) atomic->check(re).set(::uos::catalog::types::CODEX_CATALOG_AUTH_GENERATION_KEY, rt::KvValue(*generation)).set(auth::CODEX_MODELS_KV_KEY, rt::KvValue(*next)).set(runtime_config::RUNTIME_CONFIG_V2_KEY, rt::KvValue(runtime_config::toJson(*nextRuntime)));
      const auto commit = co_await atomic->commit(); if (!commit.ok) continue;
      persistedPool = *nextPool; persistedSnapshot = *next; runtime = nextRuntime;
    } catch (const auth::CodexError& e) { co_return error(e.status, e.message, e.code); }
  }
  if (!persistedPool) co_return error(500, "Deno KV could not persist Codex auth and models", "server_error");
  auth::cacheCodexAuthPool(*persistedPool); if (runtime) runtime_config::cacheRuntimeConfig(*runtime);
  bool seeded = false;
  if (valid && generation) try {
    ::uos::catalog::store::StoreCodexCatalogInput input{.clientVersion = valid->clientVersion, .authGeneration = *generation, .body = valid->modelsBody, .etag = valid->etag, .contentType = valid->contentType, .fetchedAtMs = rt::dateNow()}; auto task = ::uos::catalog::store::storeCodexCatalog(*database, input); seeded = co_await std::move(task);
  } catch (...) { rt::console::error("[ai.ubq.fi] Codex catalog seed failed:", std::current_exception()); }
  const auto* rows = rt::getArray(*persistedSnapshot, "models"); json::array ids; for (const auto& account : persistedPool->accounts) ids.emplace_back(account.account_id);
  const auto exp = auth::getJwtExpMs(std::string_view(validatedAuth.access_token));
  json::object stored{{"count", rows ? static_cast<double>(rows->size()) : 0}, {"source", memberOrNull(*persistedSnapshot, "source")}, {"updated_at_ms", memberOrNull(*persistedSnapshot, "updated_at_ms")}, {"client_version", memberOrNull(*persistedSnapshot, "client_version")}};
  co_return http::json(200, json::object{{"stored", true}, {"refreshed", valid ? valid->refreshed : false}, {"account_id", validatedAuth.account_id}, {"account_count", persistedPool->accounts.size()}, {"account_ids", ids}, {"access_token_expires_at_ms", exp ? json::value(*exp) : json::value(nullptr)}, {"updated_at_ms", validatedAuth.updated_at_ms}, {"upstream_status", valid ? valid->status : invalid->status}, {"upstream_content_type", valid && valid->contentType ? json::value(*valid->contentType) : json::value(nullptr)}, {"models", stored}, {"catalog_seeded", seeded}, {"normalized_snapshot_updated", valid != nullptr}}, rt::Headers{{"x-uos-upstream", "chatgpt_codex"}});
}
rt::Task<rt::Response> handleAdminCodexModelsGet() { const auto snapshot = co_await cf::loadFullCodexModelsSnapshot(); co_return http::json(200, json::object{{"ok", true}, {"data", snapshot ? json::value(*snapshot) : json::value(nullptr)}}); }
rt::Task<rt::Response> handleAdminCodexModelsSet(rt::Request req) {
  const auto raw = co_await request::readJsonBody(req); if (!rt::truthy(raw)) co_return error(400, "Invalid JSON body", "invalid_request_error");
  const auto snapshot = cm::normalizeCodexModelsPayload(raw); if (!snapshot) co_return error(400, "models must include a non-empty models array", "invalid_request_error");
  const auto size = api_keys::estimateJsonSize(*snapshot); if (!size) co_return error(400, "models payload could not be serialized", "invalid_request_error");
  if (*size > api_keys::SAFE_KV_BYTES) co_return error(413, "models snapshot too large (" + rt::numberToString(*size) + " bytes; max " + rt::numberToString(api_keys::MAX_KV_BYTES) + ").", "invalid_request_error");
  bool stored;
  try { auto task = cf::storeCodexModelsSnapshot(*snapshot); stored = co_await std::move(task); } catch (...) { auto response = runtimeConfigErrorResponse(std::current_exception()); if (response) co_return *response; throw; }
  if (!stored) co_return error(500, "Deno KV is not available; cannot persist Codex models", "server_error");
  const auto* rows = rt::getArray(*snapshot, "models"); co_return http::json(200, json::object{{"ok", true}, {"stored", true}, {"count", rows ? rows->size() : 0}, {"source", memberOrNull(*snapshot, "source")}, {"updated_at_ms", memberOrNull(*snapshot, "updated_at_ms")}, {"client_version", memberOrNull(*snapshot, "client_version")}});
}
rt::Task<rt::Response> handleAdminCodexPromptsPurge() {
  auto database = co_await databaseForRequest(); if (!database) co_return error(500, "Deno KV is not available; cannot purge Codex prompts", "server_error");
  double deleted = 0; const auto entry = co_await database->get(UOS_CODEX_PROMPTS_KEY);
  if (!entry.value.isNull()) { co_await database->delete_(UOS_CODEX_PROMPTS_KEY); ++deleted; }
  rt::KvListSelector selector{.prefix = UOS_CODEX_PROMPTS_CHUNK_PREFIX}; auto iterator = database->list(selector);
  while (auto row = co_await iterator->next()) { co_await database->delete_(row->key); ++deleted; }
  co_return http::json(200, json::object{{"deleted", deleted}});
}
rt::Task<rt::Response> handleAdminCodexModelsWhitelistGet() {
  auto database = co_await databaseForRequest(); if (!database) co_return error(500, "Deno KV is not available; cannot read model whitelist", "server_error", true);
  const auto whitelist = co_await wl::loadCodexModelsWhitelist(database);
  co_return http::json(200, json::object{{"ok", true}, {"data", json::object{{"model_ids", whitelist ? json::value_from(whitelist->model_ids) : json::value(json::array{})}, {"updated_at_ms", whitelist ? whitelist->updated_at_ms : 0}}}});
}
rt::Task<rt::Response> handleAdminModelsCatalogGet() { return handleAdminModelsCatalogGet(CatalogDependencies{}); }
rt::Task<rt::Response> handleAdminModelsCatalogGet(CatalogDependencies dependencies) {
  auto database = co_await databaseForRequest(); if (!database) co_return error(500, "Deno KV is not available; cannot read the model catalog", "server_error", true);
  rt::Promise<models::catalog::ModelCatalogSnapshot> c(dependencies.buildCatalog
    ? detail::invokeOwnedTask(std::move(dependencies.buildCatalog)) : models::catalog::buildModelCatalogSnapshot());
  rt::Promise<std::optional<wl::CodexModelsWhitelist>> w(wl::loadCodexModelsWhitelist(database));
  const auto all = co_await rt::all(c, w).wait(); const auto& catalog = std::get<0>(all); const auto& whitelist = std::get<1>(all); auto encoded = json::value_from(catalog); const auto ids = whitelist ? whitelist->model_ids : std::vector<std::string>{};
  co_return http::json(200, json::object{{"ok", true}, {"data", json::object{{"models", *rt::get(encoded, "models")}, {"sources", *rt::get(encoded, "sources")}, {"whitelist", json::object{{"model_ids", json::value_from(ids)}, {"updated_at_ms", whitelist ? whitelist->updated_at_ms : 0}}}, {"filter_active", !ids.empty()}}}}, rt::Headers{{"Cache-Control", "no-store"}});
}
rt::Task<rt::Response> handleAdminModelsRefresh() { return handleAdminModelsRefresh(ModelsRefreshDependencies{}); }
rt::Task<rt::Response> handleAdminModelsRefresh(ModelsRefreshDependencies dependencies) {
  const auto before = models::openrouter_models::openRouterModelsSnapshot(); const auto beforeLithos = provider::lithos_models::lithosModelsSnapshot();
  models::openrouter_models::FetchOpenRouterModelsOptions eo; eo.force = true; provider::metered::FetchMeteredModelsOptions mo; mo.force = true; provider::surplus::FetchSurplusModelsOptions so; so.force = true; so.requireApiKey = false; provider::lithos_models::FetchLithosModelsOptions lo; lo.force = true; provider::claude_code_models::CatalogOptions co; co.force = true;
  rt::Promise<std::shared_ptr<const models::openrouter_models::OpenRouterModelsSnapshot>> e(dependencies.refreshEnrichment
    ? detail::invokeOwnedTask(std::move(dependencies.refreshEnrichment)) : models::openrouter_models::fetchOpenRouterModels(eo));
  rt::Promise<std::optional<provider::metered::MeteredModelsSnapshot>> m(dependencies.refreshOpenlux
    ? detail::invokeOwnedTask(std::move(dependencies.refreshOpenlux), mo) : provider::metered::fetchMeteredModels(mo));
  rt::Promise<std::optional<provider::surplus::SurplusModelsSnapshot>> s(dependencies.refreshSurplus
    ? detail::invokeOwnedTask(std::move(dependencies.refreshSurplus), so) : provider::surplus::fetchSurplusModels(so));
  rt::Promise<std::optional<provider::lithos_models::LithosModelsSnapshot>> l(dependencies.refreshLithos
    ? detail::invokeOwnedTask(std::move(dependencies.refreshLithos), lo) : provider::lithos_models::fetchLithosModels(lo));
  rt::Promise<std::optional<provider::claude_code_models::ClaudeCodeModelsSnapshot>> c(dependencies.refreshClaude
    ? detail::invokeOwnedTask(std::move(dependencies.refreshClaude), co) : provider::claude_code_models::fetchClaudeCodeModels(co));
  const auto all = co_await rt::all(e, m, s, l, c).wait(); const auto& enrichment = std::get<0>(all); const auto& metered = std::get<1>(all); const auto& surplus = std::get<2>(all); const auto& lithos = std::get<3>(all); const auto& claude = std::get<4>(all);
  const auto et = enrichment ? std::optional(enrichment->updated_at_ms) : std::nullopt, lt = lithos ? std::optional(lithos->updated_at_ms) : std::nullopt;
  auto nullable = [](std::optional<double> value) { return value ? json::value(*value) : json::value(nullptr); };
  co_return http::json(200, json::object{{"ok", true}, {"data", json::object{{"openrouter", json::object{{"upstream_models", enrichment ? enrichment->models.size() : 0}, {"updated_at_ms", nullable(et)}, {"refreshed", et != (before ? std::optional(before->updated_at_ms) : std::nullopt)}}}, {"openlux", json::object{{"models", metered ? metered->models.size() : 0}, {"updated_at_ms", nullable(metered ? std::optional(metered->updated_at_ms) : std::nullopt)}}}, {"surplus", json::object{{"models", surplus ? surplus->models.size() : 0}, {"updated_at_ms", nullable(surplus ? std::optional(surplus->updated_at_ms) : std::nullopt)}}}, {"lithos", json::object{{"models", lithos ? lithos->models.size() : 0}, {"updated_at_ms", nullable(lt)}, {"refreshed", lt != (beforeLithos ? std::optional(beforeLithos->updated_at_ms) : std::nullopt)}}}, {"claude_code", json::object{{"models", claude ? claude->models.size() : 0}, {"updated_at_ms", nullable(claude ? std::optional(claude->updated_at_ms) : std::nullopt)}, {"status", claude && !claude->stale.value_or(false) ? "available" : "unavailable"}, {"stale", claude ? claude->stale.value_or(false) : false}}}}}}, rt::Headers{{"Cache-Control", "no-store"}});
}
rt::Task<rt::Response> handleAdminCodexModelsWhitelistSet(rt::Request req) {
  auto database = co_await databaseForRequest(); if (!database) co_return error(500, "Deno KV is not available; cannot store model whitelist", "server_error", true);
  const auto raw = co_await request::readJsonBody(req); if (!rt::truthy(raw) || !utils::isRecord(raw)) co_return error(400, "Invalid JSON body", "invalid_request_error");
  const auto* ids = rt::getArray(raw, "model_ids"); if (!ids) co_return error(400, "model_ids must be an array", "invalid_request_error");
  for (const auto& id : *ids) if (!id.is_string()) co_return error(400, "model_ids must contain only strings", "invalid_request_error");
  const auto normalized = wl::normalizeWhitelistModelIds(*ids); const auto size = api_keys::estimateJsonSize(json::object{{"model_ids", json::value_from(normalized)}});
  if (!size || *size > api_keys::SAFE_KV_BYTES) co_return error(413, "model_ids payload too large (max " + rt::numberToString(api_keys::MAX_KV_BYTES) + " bytes).", "invalid_request_error");
  const auto stored = co_await wl::storeCodexModelsWhitelist(database, normalized); if (!stored) co_return error(500, "Deno KV is not available; cannot persist model whitelist", "server_error", true);
  co_return http::json(200, json::object{{"ok", true}, {"stored", true}, {"model_ids", json::value_from(normalized)}, {"updated_at_ms", rt::dateNow()}});
}
rt::Task<rt::Response> handleAdminProviderSelectionGet() { return handleAdminProviderSelectionGet(CatalogDependencies{}); }
rt::Task<rt::Response> handleAdminProviderSelectionGet(CatalogDependencies dependencies) {
  auto database = co_await databaseForRequest(); if (!database) co_return error(500, "Deno KV is not available; cannot read the provider selection", "server_error", true);
  rt::Promise<models::catalog::ModelCatalogSnapshot> c(dependencies.buildCatalog
    ? detail::invokeOwnedTask(std::move(dependencies.buildCatalog)) : models::catalog::buildModelCatalogSnapshot());
  rt::Promise<std::optional<sel::ProviderSelection>> s(sel::loadProviderSelection(database));
  rt::Promise<json::array> r(codexSubscriptionRoster());
  const auto all = co_await rt::all(c, s, r).wait(); const auto& catalog = std::get<0>(all); const auto& selection = std::get<1>(all); const auto& subscriptions = std::get<2>(all);
  rt::OrderedMap<std::string, double> counts; for (auto id : sel::SELECTABLE_PROVIDER_IDS) counts.set(std::string(id), 0);
  for (const auto& entry : catalog.models) for (const auto& p : entry.providers) if (auto* count = counts.get(p.id)) ++*count;
  json::array providers;
  for (auto idView : sel::SELECTABLE_PROVIDER_IDS) {
    const std::string id(idView);
    const auto* source = catalog.sources.get(id);
    if (id == provider::presentation::GATEWAY_PROVIDER_ID || (id == provider::gemini::GEMINI_PROVIDER_ID && !source)) continue;
    const auto presentation = provider::presentation::providerPresentation(id);
    const std::optional<bool> credential = id == "openrouter" ? std::optional(provider::openrouter::readOpenRouterApiKey().has_value()) : std::nullopt;
    json::object row{{"id", id}, {"label", presentation.label}, {"tier", presentation.tier}, {"tier_label", tierLabel(presentation.tier)}, {"detail", presentation.detail}, {"endpoints", json::value_from(presentation.endpoints)}, {"health_key", presentation.health_key}, {"model_count", *counts.get(id)}, {"status", credential ? (*credential ? "available" : "unavailable") : source ? source->status : "unavailable"}, {"configured", credential.value_or(source ? source->configured.value_or(source->status == "available") : false)}};
    if (id == "codex") row["subscriptions"] = subscriptions;
    providers.push_back(row);
  }
  const auto& gateway = provider::presentation::GATEWAY_PROVIDER_PRESENTATION; const bool ready = deepseek::waterfall::deepSeekWaterfallCatalogEnabled(std::nullopt);
  providers.push_back(json::object{{"id", std::string(provider::presentation::GATEWAY_PROVIDER_ID)}, {"label", gateway.label}, {"tier", gateway.tier}, {"tier_label", tierLabel(gateway.tier)}, {"detail", gateway.detail}, {"endpoints", json::value_from(gateway.endpoints)}, {"health_key", gateway.health_key}, {"model_count", *counts.get(std::string(provider::presentation::GATEWAY_PROVIDER_ID))}, {"status", ready ? "available" : "unavailable"}, {"configured", ready}});
  json::array tiers; for (const auto& tier : provider::presentation::PROVIDER_TIERS) tiers.push_back(json::object{{"id", std::string(tier.id)}, {"label", std::string(tier.label)}});
  co_return http::json(200, json::object{{"ok", true}, {"data", json::object{{"providers", providers}, {"tiers", tiers}, {"selection", json::object{{"provider_ids", selection ? json::value_from(selection->provider_ids) : json::value(json::array{})}, {"updated_at_ms", selection ? selection->updated_at_ms : 0}}}, {"filter_active", sel::providerSelectionIsActive(selection)}}}}, rt::Headers{{"Cache-Control", "no-store"}});
}
rt::Task<rt::Response> handleAdminProviderSelectionSet(rt::Request req) {
  auto database = co_await databaseForRequest(); if (!database) co_return error(500, "Deno KV is not available; cannot store the provider selection", "server_error", true);
  const auto raw = co_await request::readJsonBody(req); if (!rt::truthy(raw) || !utils::isRecord(raw)) co_return error(400, "Invalid JSON body", "invalid_request_error");
  const auto* ids = rt::getArray(raw, "provider_ids"); if (!ids) co_return error(400, "provider_ids must be an array", "invalid_request_error"); std::vector<std::string> submitted, unknown;
  for (const auto& id : *ids) if (!id.is_string()) co_return error(400, "provider_ids must contain only strings", "invalid_request_error");
  for (const auto& id : *ids) {
    auto value = rt::trim(id.as_string());
    submitted.push_back(value);
    if (!sel::isProviderSelectionId(std::string_view(value)) ||
        (value == provider::gemini::GEMINI_PROVIDER_ID && !provider::gemini::geminiCliConfigured())) unknown.push_back(value);
  }
  if (!unknown.empty()) co_return paramError("unknown provider ids: " + rt::join(unknown, ", "), "provider_ids");
  const auto stored = co_await sel::storeProviderSelection(database, submitted); if (!stored) co_return error(500, "Deno KV is not available; cannot persist the provider selection", "server_error", true);
  co_return http::json(200, json::object{{"ok", true}, {"stored", true}, {"provider_ids", json::value_from(stored->provider_ids)}, {"updated_at_ms", stored->updated_at_ms}});
}
}
