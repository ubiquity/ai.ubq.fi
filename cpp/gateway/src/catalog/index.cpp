#include "uos/catalog/index.hpp"
#include "uos/catalog/models.hpp"
#include "uos/catalog/store.hpp"
#include "uos/catalog/types.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/models/codex_models_whitelist.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>

namespace uos::catalog::index {
namespace {
  namespace whitelist = ::uos::models::codex_models_whitelist;
  namespace selection = provider::selection;
  namespace fetch = ::uos::models::codex_models_fetch;
  using Selection = std::optional<selection::ProviderSelection>;
  using Claude = std::optional<provider::claude_code_models::ClaudeCodeModelsSnapshot>;
  using Paid = std::tuple<std::optional<provider::metered::MeteredModelsSnapshot>, std::optional<provider::surplus::SurplusModelsSnapshot>>;
  using store::Catalog;
  std::string catalogBodyEtag(std::string_view body) {
    // Both the decoded stored body and the serializer supply UTF-8.
    const std::span bytes(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    const auto digest = rt::crypto::subtle::digest("SHA-256", bytes);
    return "\"uos-catalog-" + utils::encodeHex(digest).substr(0, 32) + "\"";
  }
  rt::Task<Claude> enabledClaudeCatalog(Selection selected) {
    if (!selection::isProviderEnabled("claude_code", selected)) co_return std::nullopt;
    auto resultTask = provider::claude_code_models::fetchClaudeCodeModels(); auto result = co_await std::move(resultTask); co_return result;
  }
  rt::Task<Paid> enabledPaidCatalogSources(Selection selected, bool force = false) {
    const bool openlux = selection::isProviderEnabled("openlux", selected), surplus = selection::isProviderEnabled("surplus", selected);
    provider::metered::FetchMeteredModelsOptions mo; mo.force = force; mo.cachedOnly = !force;
    provider::surplus::FetchSurplusModelsOptions so; so.force = force; so.cachedOnly = !force;
    rt::Promise<std::optional<provider::metered::MeteredModelsSnapshot>> m = openlux ? rt::Promise(provider::metered::fetchMeteredModels(mo)) : rt::Promise<std::optional<provider::metered::MeteredModelsSnapshot>>::resolve(std::nullopt);
    rt::Promise<std::optional<provider::surplus::SurplusModelsSnapshot>> s = surplus ? rt::Promise(provider::surplus::fetchSurplusModels(so)) : rt::Promise<std::optional<provider::surplus::SurplusModelsSnapshot>>::resolve(std::nullopt);
    auto cached = co_await rt::all(m, s).wait();
    if (force) co_return cached;
    auto cm = std::get<0>(cached); auto cs = std::get<1>(cached);
    auto m2 = openlux && !cm ? rt::Promise(provider::metered::fetchMeteredModels()) : rt::Promise<std::optional<provider::metered::MeteredModelsSnapshot>>::resolve(cm);
    auto s2 = surplus && !cs ? rt::Promise(provider::surplus::fetchSurplusModels()) : rt::Promise<std::optional<provider::surplus::SurplusModelsSnapshot>>::resolve(cs);
    auto resultTask = rt::all(m2, s2).wait(); auto result = co_await std::move(resultTask); co_return result;
  }
  json::array paidModels(const Paid& paid) {
    json::array rows;
    if (const auto& m = std::get<0>(paid)) for (const auto& model : m->models) rows.push_back(json::value_from(model));
    if (const auto& s = std::get<1>(paid)) for (const auto& model : s->models) rows.push_back(json::value_from(model));
    return models::uniqueResponsesModels(rows);
  }
  template <class Body>
  rt::Response bodyResponse(Body&& body, const rt::Request& req, rt::Headers headers, std::string etag) {
    headers.set("ETag", etag);
    rt::ResponseInit init; init.status = models::etagMatches(req.headers().get("If-None-Match"), etag) ? 304 : 200; init.headers = headers;
    return rt::Response(init.status == 304 ? rt::BodyInit(nullptr) : rt::BodyInit(std::forward<Body>(body)), init);
  }
  rt::Task<rt::Response> catalogResponse(Catalog catalog, rt::Request req, std::string cacheState) {
    ::uos::models::openrouter_models::warmOpenRouterModels();
    rt::Headers headers{{"Content-Type", catalog->metadata.content_type}, {"Cache-Control", "private, max-age=300, stale-while-revalidate=86400"}, {"x-uos-upstream", "chatgpt_codex"}, {"x-uos-cache", cacheState}};
    auto selected = co_await selection::loadProviderSelectionCached();
    const bool codex = selection::isProviderEnabled("codex", selected), deepseek = selection::isProviderEnabled("deepseek", selected), waterfall = deepseek::waterfall::deepSeekWaterfallCatalogEnabled(selected), lithos = selection::isProviderEnabled("lithos", selected), cerebras = selection::isProviderEnabled("cerebras", selected), openrouter = selection::isProviderEnabled("openrouter", selected) && provider::openrouter::readOpenRouterApiKey().has_value();
    auto kvp = kv::getKv(); auto database = co_await kvp.wait();
    auto wl = co_await whitelist::loadCodexModelsWhitelist(database);
    rt::Promise<Paid> paidTask(enabledPaidCatalogSources(selected)); rt::Promise<Claude> claudeTask(enabledClaudeCatalog(selected));
    auto catalogs = co_await rt::all(paidTask, claudeTask).wait();
    const auto& paid = std::get<0>(catalogs); const auto& claude = std::get<1>(catalogs);
    const auto now = rt::dateNow();
    if (const auto& m = std::get<0>(paid); m && now - m->updated_at_ms >= provider::metered::METERED_MODELS_CACHE_TTL_MS) rt::spawn(provider::metered::fetchMeteredModels(), rt::ignoreRejection);
    if (const auto& s = std::get<1>(paid); s && now - s->updated_at_ms >= provider::surplus::SURPLUS_MODELS_CACHE_TTL_MS) rt::spawn(provider::surplus::fetchSurplusModels(), rt::ignoreRejection);
    auto paidRows = paidModels(paid);
    if ((!wl || wl->model_ids.empty()) && paidRows.empty() && (!claude || claude->models.empty()) && !deepseek && !waterfall && !lithos && !cerebras && !openrouter && codex) {
      co_return bodyResponse(catalog->body, req, headers, catalogBodyEtag(catalog->body));
    }
    json::value parsed(catalog->parsed);
    auto& rows = parsed.as_object().at("models").as_array();
    if (!codex) rows.clear();
    if (!paidRows.empty()) {
      auto seen = models::catalogModelIds(rows);
      auto full = co_await fetch::loadFullCodexModelsSnapshot();
      auto records = models::codexSnapshotRecords(full);
      for (const auto& model : paidRows) {
        const auto id = std::string(rt::getString(model, "id").value_or(""));
        if (seen.has(id)) continue;
        rows.push_back(models::meteredCodexModelRecord(model.as_object(), records.get(id))); seen.add(id);
      }
    }
    if (claude && !claude->models.empty()) rows = models::withClaudeCodeModels(std::move(rows), claude->models);
    if (deepseek) rows = models::withDeepSeekOfficialModels(std::move(rows));
    if (waterfall) rows = models::withDeepSeekWaterfallModel(std::move(rows));
    if (lithos) rows = models::withLithosModels(std::move(rows));
    if (cerebras) rows = models::withCerebrasModels(std::move(rows));
    if (openrouter) rows = models::withOpenRouterModels(std::move(rows));
    if (wl && !wl->model_ids.empty()) rows = whitelist::filterWhitelistedCatalogModels(rows, wl);
    auto body = rt::jsonStringify(parsed);
    auto etag = catalogBodyEtag(body);
    co_return bodyResponse(std::move(body), req, headers, etag);
  }
  rt::Task<std::optional<rt::Response>> meteredCatalogResponse(Selection selected) {
    ::uos::models::openrouter_models::warmOpenRouterModels();
    rt::Promise<Paid> paidTask(enabledPaidCatalogSources(selected, true)); rt::Promise<Claude> claudeTask(enabledClaudeCatalog(selected));
    auto catalogs = co_await rt::all(paidTask, claudeTask).wait();
    auto paid = paidModels(std::get<0>(catalogs)); const auto& claude = std::get<1>(catalogs);
    const bool deepseek = selection::isProviderEnabled("deepseek", selected), lithos = selection::isProviderEnabled("lithos", selected), cerebras = selection::isProviderEnabled("cerebras", selected), openrouter = selection::isProviderEnabled("openrouter", selected), waterfall = deepseek::waterfall::deepSeekWaterfallCatalogEnabled(selected);
    const std::vector<provider::claude_code_models::ClaudeCodeModel> empty;
    auto configured = models::claudeCodeCodexModels(claude ? claude->models : empty);
    if (waterfall) configured = models::withDeepSeekWaterfallModel(std::move(configured));
    auto append = [&](const json::array& more) { for (const auto& row : more) configured.push_back(row); };
    if (deepseek) append(models::deepSeekOfficialCodexModels());
    if (lithos) append(models::lithosCodexModels());
    if (cerebras) append(models::cerebrasCodexModels());
    if (openrouter && provider::openrouter::readOpenRouterApiKey()) append(models::openRouterCodexModels());
    if (paid.empty() && configured.empty()) co_return std::nullopt;
    auto full = co_await fetch::loadFullCodexModelsSnapshot(); auto records = models::codexSnapshotRecords(full);
    auto rows = models::claudeCodeCodexModels(claude ? claude->models : empty);
    for (const auto& model : paid) rows.push_back(models::meteredCodexModelRecord(model.as_object(), records.get(rt::getString(model, "id").value_or(""))));
    if (deepseek) rows = models::withDeepSeekOfficialModels(std::move(rows));
    if (waterfall) rows = models::withDeepSeekWaterfallModel(std::move(rows));
    if (lithos) rows = models::withLithosModels(std::move(rows));
    if (cerebras) rows = models::withCerebrasModels(std::move(rows));
    if (openrouter) rows = models::withOpenRouterModels(std::move(rows));
    json::array unique; rt::OrderedSet<std::string> seen;
    for (const auto& row : rows) { const auto id = std::string(rt::getString(row, "slug").value_or("")); if (!seen.has(id)) { seen.add(id); unique.push_back(row); } }
    auto kvp = kv::getKv(); auto database = co_await kvp.wait();
    std::optional<whitelist::CodexModelsWhitelist> wl; if (database) wl = co_await whitelist::loadCodexModelsWhitelist(database);
    rt::ResponseInit init; init.status = 200; init.headers = rt::Headers{{"Content-Type", "application/json"}, {"Cache-Control", "private, max-age=300"}, {"x-uos-upstream", "metered"}};
    json::object payload{{"models", whitelist::filterWhitelistedCatalogModels(unique, wl)}};
    co_return rt::Response(rt::jsonStringify(payload), init);
  }
  rt::Task<rt::Response> meteredCatalogOrError(std::string message) {
    auto selected = co_await selection::loadProviderSelectionCached(); auto responseTask = meteredCatalogResponse(selected); auto response = co_await std::move(responseTask);
    co_return response.value_or(http::openaiError(502, message, "codex_catalog_unavailable"));
  }
  rt::Task<void> recordCatalogDegradation(std::string version, CodexCatalogDependencies dependencies) {
    try { if (dependencies.recordSentinelDegradation) { auto task = dependencies.recordSentinelDegradation(dependencies.now ? dependencies.now() : rt::dateNow()); rt::Promise<bool> promise(std::move(task)); co_await promise.wait(); } else co_await rt::microtask(); }
    catch (...) { rt::console::error("[ai.ubq.fi] Sentinel catalog degradation record failed for " + version + ":", std::current_exception()); }
  }
  struct CodexCatalogRefreshContext {
    std::shared_ptr<rt::Kv> kv;
    rt::Request req{"http://localhost/"};
    std::string version;
    std::string authGeneration;
    Catalog cached;
    double nowMs;
    store::RefreshLeaseHeartbeat heartbeat;
    CodexCatalogDependencies dependencies;
    bool providerDegradationObserved = false;
    bool providerDegradationRecorded = false;
  };
  using Context = std::shared_ptr<CodexCatalogRefreshContext>;
  rt::Task<void> recordObservedProviderDegradation(Context c) {
    if (!c->providerDegradationObserved || c->providerDegradationRecorded) co_return;
    c->providerDegradationRecorded = true;
    auto task = recordCatalogDegradation(c->version, c->dependencies); co_await std::move(task);
  }
  rt::Task<rt::Response> rotatedCatalogResponse(std::shared_ptr<rt::Kv> database, rt::Request req, std::string version) {
    Catalog replacement; try { replacement = co_await store::loadCurrentGenerationCatalog(*database, version); } catch (...) {}
    if (replacement) { auto resultTask = catalogResponse(replacement, req, "rotated"); auto result = co_await std::move(resultTask); co_return result; }
    co_return http::openaiError(502, "Codex authentication changed during catalog refresh", "codex_catalog_unavailable");
  }
  rt::Task<rt::Response> staleOrRotatedOrUnavailable(Context c, std::string message) {
    bool current = false; if (c->cached) current = co_await store::authGenerationIsCurrent(*c->kv, c->authGeneration);
    if (c->cached && current) { auto resultTask = catalogResponse(c->cached, c->req, "stale"); auto result = co_await std::move(resultTask); co_return result; }
    Catalog replacement; try { replacement = co_await store::loadCurrentGenerationCatalog(*c->kv, c->version); } catch (...) {}
    if (replacement) { auto resultTask = catalogResponse(replacement, c->req, "rotated"); auto result = co_await std::move(resultTask); co_return result; }
    auto resultTask = meteredCatalogOrError(message); auto result = co_await std::move(resultTask); co_return result;
  }
  rt::Task<rt::Response> revalidateCachedCatalog(Context c, std::optional<std::string> sourceBody) {
    bool current = false; if (!c->heartbeat.lost()) current = co_await store::authGenerationIsCurrent(*c->kv, c->authGeneration);
    if (!current) { auto resultTask = rotatedCatalogResponse(c->kv, c->req, c->version); auto result = co_await std::move(resultTask); co_return result; }
    if (!sourceBody) { auto resultTask = staleOrRotatedOrUnavailable(c, "Codex catalog source body is unavailable for revalidation"); auto result = co_await std::move(resultTask); co_return result; }
    const auto& cached = c->cached;
    store::StoreCodexCatalogInput input{.clientVersion = c->version, .authGeneration = c->authGeneration, .body = std::move(*sourceBody), .etag = cached->metadata.etag, .contentType = cached->metadata.content_type, .fetchedAtMs = c->nowMs};
    auto pending = store::storeCodexCatalog(*c->kv, std::move(input)); auto stored = co_await std::move(pending);
    bool stillCurrent = false; if (stored) stillCurrent = co_await store::authGenerationIsCurrent(*c->kv, c->authGeneration);
    if (!stillCurrent) { auto resultTask = rotatedCatalogResponse(c->kv, c->req, c->version); auto result = co_await std::move(resultTask); co_return result; }
    auto refreshed = co_await store::loadCatalog(*c->kv, c->version, c->authGeneration, c->nowMs);
    auto resultTask = catalogResponse(refreshed ? refreshed : cached, c->req, "revalidated"); auto result = co_await std::move(resultTask); co_return result;
  }
  rt::Task<rt::Response> storeRefreshedCatalog(Context c, std::string body, json::object parsed, std::optional<std::string> contentType, std::optional<std::string> etag) {
    bool current = false; if (!c->heartbeat.lost()) current = co_await store::authGenerationIsCurrent(*c->kv, c->authGeneration);
    if (!current) { auto resultTask = rotatedCatalogResponse(c->kv, c->req, c->version); auto result = co_await std::move(resultTask); co_return result; }
    store::StoreCodexCatalogInput input{.clientVersion = c->version, .authGeneration = c->authGeneration, .body = std::move(body), .etag = etag, .contentType = contentType, .fetchedAtMs = c->nowMs};
    auto task = store::storeCodexCatalog(*c->kv, std::move(input)); auto stored = co_await std::move(task);
    if (!stored) { auto resultTask = staleOrRotatedOrUnavailable(c, "Codex model catalog could not be cached"); auto result = co_await std::move(resultTask); co_return result; }
    try { auto updateTask = models::maybeUpdateNormalizedSnapshot(*c->kv, c->version, c->authGeneration, std::move(parsed), c->nowMs); co_await std::move(updateTask); }
    catch (...) { rt::console::error("[ai.ubq.fi] Codex normalized snapshot update failed for " + c->version + ":", std::current_exception()); }
    current = co_await store::authGenerationIsCurrent(*c->kv, c->authGeneration);
    if (!current) { auto resultTask = rotatedCatalogResponse(c->kv, c->req, c->version); auto result = co_await std::move(resultTask); co_return result; }
    auto storedCatalog = co_await store::loadCatalog(*c->kv, c->version, c->authGeneration, c->nowMs);
    if (storedCatalog) { auto resultTask = catalogResponse(storedCatalog, c->req, "miss"); auto result = co_await std::move(resultTask); co_return result; }
    auto resultTask = meteredCatalogOrError("Codex model catalog could not be read after caching"); auto result = co_await std::move(resultTask); co_return result;
  }
  rt::Task<rt::Response> refreshCodexCatalog(Context c) {
    fetch::FetchCodexModelsOptions options; options.clientVersion = c->version;
    std::optional<std::string> sourceBody;
    if (c->cached) {
      try {
        auto reading = store::loadCatalogSource(*c->kv, c->cached);
        sourceBody = co_await std::move(reading);
      } catch (...) {
        rt::console::error("[ai.ubq.fi] Codex catalog source read failed for " + c->version + ":", std::current_exception());
      }
      if (sourceBody) options.ifNoneMatch = c->cached->metadata.etag;
    }
    options.onProviderTransportFailure = [c] { c->providerDegradationObserved = true; };
    auto task = fetch::fetchCodexModels(options); auto upstream = co_await std::move(task);
    if (upstream.status() >= 500 && upstream.status() <= 599) c->providerDegradationObserved = true;
    co_await recordObservedProviderDegradation(c);
    if (upstream.status() == 304 && c->cached) { auto resultTask = revalidateCachedCatalog(c, std::move(sourceBody)); auto result = co_await std::move(resultTask); co_return result; }
    sourceBody.reset();
    auto contentType = upstream.headers().get("Content-Type");
    std::string body; bool readFailed = false;
    try { body = co_await upstream.text(); } catch (...) { readFailed = true; rt::console::error("[ai.ubq.fi] Codex catalog response read failed for " + c->version + ":", std::current_exception()); }
    if (readFailed) { c->providerDegradationObserved = true; co_await recordObservedProviderDegradation(c); }
    auto parsed = contentType && rt::includes(rt::toLowerCase(*contentType), "application/json") ? store::parseCatalogBody(body) : std::nullopt;
    if (!upstream.ok() || !parsed) {
      rt::console::error("[ai.ubq.fi] Codex catalog refresh failed for " + c->version + ": upstream " + rt::numberToString(upstream.status()) + " " + rt::slice(body, 0, 240));
      auto resultTask = staleOrRotatedOrUnavailable(c, "Codex upstream did not return a valid model catalog"); auto result = co_await std::move(resultTask); co_return result;
    }
    auto resultTask = storeRefreshedCatalog(c, std::move(body), std::move(*parsed), contentType, upstream.headers().get("ETag")); auto result = co_await std::move(resultTask); co_return result;
  }
  rt::Task<rt::Response> recoverFailedCatalogRefresh(Context c) {
    bool current = false; try { current = co_await store::authGenerationIsCurrent(*c->kv, c->authGeneration); } catch (...) {}
    Catalog recovered;
    if (current) { recovered = c->cached; if (!recovered) try { recovered = co_await store::loadCatalog(*c->kv, c->version, c->authGeneration, rt::dateNow()); } catch (...) {} }
    else try { recovered = co_await store::loadCurrentGenerationCatalog(*c->kv, c->version); } catch (...) {}
    if (!recovered) { auto resultTask = meteredCatalogOrError("Codex upstream model catalog is unavailable"); auto result = co_await std::move(resultTask); co_return result; }
    auto label = !current ? std::string("rotated") : c->cached ? std::string("stale") : std::string("miss");
    auto resultTask = catalogResponse(recovered, c->req, label); auto result = co_await std::move(resultTask); co_return result;
  }
  rt::Task<rt::Response> refreshCodexCatalogWithLease(Context c, std::string leaseOwner) {
    c->heartbeat = store::startRefreshLeaseHeartbeat(*c->kv, c->version, leaseOwner);
    std::optional<rt::Response> response; rt::Reason failure;
    try { auto refreshTask = refreshCodexCatalog(c); response = co_await std::move(refreshTask); }
    catch (...) { rt::console::error("[ai.ubq.fi] Codex catalog refresh failed for " + c->version + ":", std::current_exception()); failure = std::current_exception(); }
    if (failure) {
      try { co_await recordObservedProviderDegradation(c); auto recoveryTask = recoverFailedCatalogRefresh(c); response = co_await std::move(recoveryTask); failure = nullptr; } catch (...) { failure = std::current_exception(); }
    }
    co_await c->heartbeat.stop(); co_await store::releaseRefreshLease(*c->kv, c->version, leaseOwner);
    if (failure) std::rethrow_exception(failure);
    co_return *response;
  }
}
rt::Task<rt::Response> handleCodexCatalogModels(rt::Request req, std::string rawVersion) { return handleCodexCatalogModels(req, std::move(rawVersion), CodexCatalogDependencies{}); }
rt::Task<rt::Response> handleCodexCatalogModels(rt::Request req, std::string rawVersion, CodexCatalogDependencies dependencies) {
  const auto version = rt::trim(rawVersion);
  if (!::uos::models::codex_models::parseCodexClientVersion(version)) {
    http::OpenaiErrorOptions options; options.param = std::optional<std::string>("client_version");
    co_return http::openaiError(400, "client_version must be an exact X.Y.Z version", "invalid_client_version", options);
  }
  auto kvp = kv::getKv(); auto database = co_await kvp.wait();
  if (!database) { auto resultTask = meteredCatalogOrError("Codex model catalog cache is unavailable"); auto result = co_await std::move(resultTask); co_return result; }
  std::string generation; bool failed = false;
  try { generation = co_await store::getAuthGeneration(*database); } catch (...) { failed = true; rt::console::error("[ai.ubq.fi] Codex catalog generation initialization failed:", std::current_exception()); }
  if (failed) { auto resultTask = meteredCatalogOrError("Codex model catalog cache is unavailable"); auto result = co_await std::move(resultTask); co_return result; }
  const auto now = rt::dateNow(); Catalog cached;
  try { cached = co_await store::loadCatalog(*database, version, generation, now); } catch (...) { rt::console::error("[ai.ubq.fi] Codex catalog cache read failed for " + version + ":", std::current_exception()); }
  if (cached && now - cached->metadata.fetched_at_ms < types::CODEX_CATALOG_FRESH_MS) { auto resultTask = catalogResponse(cached, req, "hit"); auto result = co_await std::move(resultTask); co_return result; }
  const auto owner = rt::crypto::randomUUID(); bool acquired = false;
  try { acquired = co_await store::acquireRefreshLease(*database, version, owner, now); } catch (...) { rt::console::error("[ai.ubq.fi] Codex catalog lease acquisition failed for " + version + ":", std::current_exception()); }
  if (!acquired) {
    if (cached) { auto resultTask = catalogResponse(cached, req, "stale"); auto result = co_await std::move(resultTask); co_return result; }
    Catalog waited; try { waited = co_await store::waitForColdCatalog(*database, version); } catch (...) { rt::console::error("[ai.ubq.fi] Codex catalog cold-cache wait failed for " + version + ":", std::current_exception()); }
    if (waited) { auto resultTask = catalogResponse(waited, req, "wait"); auto result = co_await std::move(resultTask); co_return result; }
    auto resultTask = meteredCatalogOrError("Codex model catalog refresh is already in progress"); auto result = co_await std::move(resultTask); co_return result;
  }
  auto context = std::make_shared<CodexCatalogRefreshContext>(); context->kv = database; context->req = req; context->version = version; context->authGeneration = generation; context->cached = cached; context->nowMs = now; context->dependencies = dependencies;
  if (cached) {
    auto background = refreshCodexCatalogWithLease(context, owner);
    rt::spawn(std::move(background), [version](rt::Reason error) { rt::console::error("[ai.ubq.fi] Codex catalog background refresh failed for " + version + ":", error); });
    auto resultTask = catalogResponse(cached, req, "stale"); auto result = co_await std::move(resultTask); co_return result;
  }
  auto resultTask = refreshCodexCatalogWithLease(context, owner); auto result = co_await std::move(resultTask); co_return result;
}
std::optional<std::string> getCatalogClientVersion(const rt::Request& req) {
  auto values = rt::URL(req.url()).searchParams().getAll("client_version");
  if (values.size() == 1) return values[0];
  if (values.empty()) return std::nullopt;
  return "";
}
}
