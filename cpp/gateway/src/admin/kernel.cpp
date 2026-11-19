#include "uos/admin/kernel.hpp"
#include "uos/admin/api_keys.hpp"
#include "uos/admin/codex.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/codex/experiment.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/config.hpp"
#include "uos/http.hpp"
#include "uos/kernel/attestation.hpp"
#include "uos/kernel/policy_queue.hpp"
#include "uos/kernel/usage.hpp"
#include "uos/kv.hpp"
#include "uos/metered_quota.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/paid_fallback/ledger_stats.hpp"
#include "uos/paid_fallback/rollups.hpp"
#include "uos/provider/capacity_parse.hpp"
#include "uos/quota_projection.hpp"
#include "uos/request.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"
#include "uos/utils.hpp"
#include <algorithm>
#include <memory>
#include <tuple>
#include <variant>

namespace uos::admin::kernel {
namespace {
  namespace ku = ::uos::kernel::usage;
  namespace qv = ::uos::kernel::quota_v2;
  namespace qw = ::uos::kernel::quota_write;
  namespace cp = ::uos::codex::banked_reset_provider;
  namespace ce = ::uos::codex::experiment;
  namespace cs = ::uos::codex::reset_settings;
  namespace co = ::uos::codex::overage_settings;
  namespace mq = ::uos::metered_quota;
  namespace rollups = ::uos::paid_fallback::rollups;
  namespace stats = ::uos::paid_fallback::ledger_stats;
  namespace backfill = ::uos::paid_fallback::ledger_backfill;
  namespace projection = ::uos::quota_projection;

  rt::Task<std::shared_ptr<rt::Kv>> databaseForRequest() { auto promise = kv::getKv(); auto result = co_await promise.wait(); co_return result; }
  template<class T> json::value orNullJson(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }
  json::value storedWireJson(const rt::KvValue& value) { return rt::jsonParse(rt::kvJsonStringify(value).value_or("null")); }
  json::value queryValue(const rt::URL& url, const std::string& key) { const auto value = url.searchParams().get(key); return value ? json::value(*value) : json::value(nullptr); }
  template<class T> rt::Promise<std::optional<T>> nullableOnFailure(rt::Task<T> task) {
    rt::Promise<T> pending(std::move(task));
    return pending.then([](T value) { return std::optional<T>(std::move(value)); }, [](rt::Reason) { return std::optional<T>(); });
  }
  std::optional<std::string> normalizePem(const json::value* raw) {
    if (!raw || !raw->is_string()) return std::nullopt;
    const auto pem = rt::trim(std::string(raw->as_string()));
    if (!rt::startsWith(pem, "-----BEGIN PUBLIC KEY-----") || !rt::endsWith(pem, "-----END PUBLIC KEY-----")) return std::nullopt;
    return pem;
  }
  rt::KvArray publicKeyRecords(const rt::KvValue& value, bool deleting) {
    if (value.isNullish()) return {};
    if (!value.isArray()) {
      if (deleting) throw rt::TypeError("existing.filter is not a function. (In 'existing.filter((p) => p.app_id !== appId)', 'existing.filter' is undefined)");
      throw rt::TypeError("existing.some is not a function. (In 'existing.some((p) => p.app_id === appId)', 'existing.some' is undefined)");
    }
    return value.asArray();
  }
  const rt::KvValue& publicKeyAppId(const rt::KvValue& value) {
    if (value.isNullish()) throw rt::TypeError(std::string(value.isNull() ? "null" : "undefined") + " is not an object (evaluating 'p.app_id')");
    return value["app_id"];
  }
  rt::Task<void> readQueuedOwnerPolicy(std::shared_ptr<rt::Kv> store, std::shared_ptr<rt::OrderedSet<std::string>> output, std::string owner) {
    const auto key = qv::kernelOrgLimitKey(owner);
    auto reading = store->get(key);
    const auto entry = co_await std::move(reading);
    if (entry.value.truthy()) output->add(owner);
  }
  rt::Task<void> readQueuedRepoPolicy(std::shared_ptr<rt::Kv> store, std::shared_ptr<rt::OrderedSet<std::string>> output, ::uos::types::KernelPolicyQueueItem record) {
    const auto key = qv::kernelLimitKey(record.owner, record.repo);
    auto reading = store->get(key);
    const auto entry = co_await std::move(reading);
    if (entry.value.truthy()) output->add(record.owner + "/" + record.repo);
  }
}
rt::Task<rt::Response> handleAdminKernelPubKeysList() {
  const auto kv = co_await databaseForRequest();
  if (!kv) co_return http::openaiError(500, "Deno KV is not available", "server_error");
  const auto kvEntry = co_await kv->get(::uos::admin::codex::UOS_KERNEL_PUBKEYS_KEY);
  co_return http::json(200, json::object{{"data", kvEntry.value.isNull() || kvEntry.value.isUndefined() ? json::value(json::array()) : storedWireJson(kvEntry.value)}});
}
rt::Task<rt::Response> handleAdminKernelPubKeysCreate(rt::Request req) {
  const auto kv = co_await databaseForRequest();
  if (!kv) co_return http::openaiError(500, "Deno KV is not available", "server_error");
  const auto raw = co_await request::readJsonBody(req);
  if (!rt::truthy(raw) || !utils::isRecord(raw)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
  const auto appId = rt::getNumber(raw, "app_id");
  if (!appId) co_return http::openaiError(400, "app_id is required and must be a number", "invalid_request_error");
  const auto pem = normalizePem(rt::get(raw, "pem"));
  if (!pem) co_return http::openaiError(400, "pem must be a valid RS256 public PEM", "invalid_request_error");
  const auto owner = utils::getString(rt::get(raw, "owner")).value_or("unknown");
  const auto entry = co_await kv->get(::uos::admin::codex::UOS_KERNEL_PUBKEYS_KEY);
  auto existing = publicKeyRecords(entry.value, false);
  for (const auto& item : existing) { if (item.is<rt::KvHole>()) continue; const auto& existingAppId = publicKeyAppId(item); if (existingAppId.isNumber() && existingAppId.asNumber() == *appId) co_return http::openaiError(409, "Public key for App ID " + rt::numberToString(*appId) + " already exists", "invalid_request_error"); }
  const json::object record{{"app_id", rt::makeNumber(*appId)}, {"pem", *pem}, {"owner", owner}, {"added_at_ms", rt::makeNumber(rt::dateNow())}};
  for (auto& item : existing) if (item.is<rt::KvHole>()) item = rt::KvValue();
  existing.emplace_back(record);
  auto operation = kv->atomic(); operation->check(entry).set(::uos::admin::codex::UOS_KERNEL_PUBKEYS_KEY, rt::KvValue(std::move(existing)));
  const auto commit = co_await operation->commit();
  if (!commit.ok) co_return http::openaiError(409, "Concurrent modification; retry", "invalid_request_error");
  co_await ::uos::kernel::attestation::reloadKernelPublicKeys();
  co_return http::json(200, json::object{{"ok", true}, {"data", record}});
}
rt::Task<rt::Response> handleAdminKernelPubKeysDelete(rt::Request req) {
  const auto kv = co_await databaseForRequest();
  if (!kv) co_return http::openaiError(500, "Deno KV is not available", "server_error");
  const rt::URL url(req.url()); const auto appIdStr = url.searchParams().get("app_id");
  const auto appId = appIdStr && !appIdStr->empty() ? std::optional(rt::parseInt(*appIdStr, 10)) : std::nullopt;
  if (!appId || rt::Number::isNaN(*appId)) co_return http::openaiError(400, "app_id query parameter is required and must be a number", "invalid_request_error");
  const auto entry = co_await kv->get(::uos::admin::codex::UOS_KERNEL_PUBKEYS_KEY);
  const auto existing = publicKeyRecords(entry.value, true);
  rt::KvArray updated;
  for (const auto& item : existing) { if (item.is<rt::KvHole>()) continue; const auto& existingAppId = publicKeyAppId(item); if (!existingAppId.isNumber() || existingAppId.asNumber() != *appId) updated.push_back(item); }
  if (updated.size() == existing.size()) co_return http::openaiError(404, "Not found", "not_found");
  auto operation = kv->atomic(); operation->check(entry).set(::uos::admin::codex::UOS_KERNEL_PUBKEYS_KEY, rt::KvValue(std::move(updated)));
  const auto commit = co_await operation->commit();
  if (!commit.ok) co_return http::openaiError(409, "Concurrent modification; retry", "invalid_request_error");
  co_await ::uos::kernel::attestation::reloadKernelPublicKeys();
  co_return http::json(200, json::object{{"ok", true}, {"deleted_app_id", rt::makeNumber(*appId)}});
}
rt::Task<rt::Response> handleAdminKernelPolicyQueueList() {
  const auto records = co_await ::uos::kernel::policy_queue::listKernelPolicyQueue();
  if (!records) co_return http::openaiError(500, "Deno KV is not available", "server_error");
  if (records->empty()) co_return http::json(200, json::object{{"data", json::value_from(*records)}});
  const auto kv = co_await databaseForRequest();
  if (!kv) co_return http::openaiError(500, "Deno KV is not available", "server_error");
  auto orgPolicyOwners = std::make_shared<rt::OrderedSet<std::string>>(); rt::OrderedSet<std::string> owners;
  for (const auto& record : *records) owners.add(record.owner);
  std::vector<rt::Promise<void>> pendingOwners;
  for (const auto& owner : owners) pendingOwners.emplace_back(readQueuedOwnerPolicy(kv, orgPolicyOwners, owner));
  co_await rt::all(std::move(pendingOwners)).wait();
  auto repoPolicyPairs = std::make_shared<rt::OrderedSet<std::string>>(); std::vector<rt::Promise<void>> pendingRepos;
  for (const auto& record : *records) if (!orgPolicyOwners->has(record.owner)) pendingRepos.emplace_back(readQueuedRepoPolicy(kv, repoPolicyPairs, record));
  co_await rt::all(std::move(pendingRepos)).wait();
  json::array pending;
  for (const auto& record : *records) if (!orgPolicyOwners->has(record.owner) && !repoPolicyPairs->has(record.owner + "/" + record.repo)) pending.push_back(json::value_from(record));
  co_return http::json(200, json::object{{"data", pending}});
}
namespace {
  constexpr double KERNEL_USAGE_DAILY_DAYS = 30;
  using OrgUsageRows = rt::OrderedMap<std::string, std::optional<ku::KernelOrgUsageWithDaily>>;
  using RepoUsageRows = rt::OrderedMap<std::string, std::optional<ku::KernelAuthUsageWithDaily>>;
  rt::Task<void> readOrgUsageRow(std::shared_ptr<OrgUsageRows> output, std::string owner) {
    ku::KernelUsageReadOptions options{.includeDaily = true, .dailyDays = KERNEL_USAGE_DAILY_DAYS};
    auto task = ku::getKernelOrgUsage(owner, options);
    auto usage = co_await std::move(task);
    output->set(owner, std::move(usage));
  }
  rt::Task<void> readRepoUsageRow(std::shared_ptr<RepoUsageRows> output, ::uos::types::KernelAuthLimitRecord record) {
    const auto key = record.owner + "/" + record.repo;
    ku::KernelUsageReadOptions options{.includeDaily = true, .dailyDays = KERNEL_USAGE_DAILY_DAYS};
    auto task = ku::getKernelUsage(record.owner, record.repo, options);
    auto usage = co_await std::move(task);
    output->set(key, std::move(usage));
  }
  rt::Task<rt::Response> kernelUsageInventoryResponse(std::string scope) {
    ku::KernelUsageReadOptions options{.includeDaily = true, .dailyDays = KERNEL_USAGE_DAILY_DAYS};
    if (scope == "org") {
      auto task = ku::listKernelOrgUsageRecords(options); const auto orgRecords = co_await std::move(task);
      if (!orgRecords) co_return http::openaiError(500, "Failed to load kernel org usage inventory", "server_error");
      co_return http::json(200, json::object{{"ok", true}, {"scope", scope}, {"usage", json::value_from(*orgRecords)}});
    }
    auto task = ku::listKernelUsageRecords(options); const auto records = co_await std::move(task);
    if (!records) co_return http::openaiError(500, "Failed to load kernel usage inventory", "server_error");
    co_return http::json(200, json::object{{"ok", true}, {"scope", scope}, {"usage", json::value_from(*records)}});
  }
  rt::Task<rt::Response> kernelOrgUsageLimitsResponse(std::string scope, bool includeUsage) {
    const auto limits = co_await qv::listKernelOrgUsageLimits();
    if (!limits) co_return http::openaiError(500, "Failed to load kernel org usage limits", "server_error");
    std::shared_ptr<OrgUsageRows> usageByOwner;
    if (includeUsage) {
      usageByOwner = std::make_shared<OrgUsageRows>();
      std::vector<rt::Promise<void>> pending;
      for (const auto& record : *limits) pending.emplace_back(readOrgUsageRow(usageByOwner, record.owner));
      co_await rt::all(std::move(pending)).wait();
    }
    json::array rows;
    for (const auto& record : *limits) { auto row = json::value_from(record).as_object(); if (includeUsage) { const auto* usage = usageByOwner->get(record.owner); row["usage"] = usage ? orNullJson(*usage) : json::value(nullptr); } rows.push_back(std::move(row)); }
    co_return http::json(200, json::object{{"ok", true}, {"scope", scope}, {"limits", rows}});
  }
  rt::Task<rt::Response> kernelRepoUsageLimitsResponse(std::string scope, bool includeUsage) {
    const auto limits = co_await qv::listKernelUsageLimits();
    if (!limits) co_return http::openaiError(500, "Failed to load kernel usage limits", "server_error");
    std::shared_ptr<RepoUsageRows> usageByRepo;
    if (includeUsage) {
      usageByRepo = std::make_shared<RepoUsageRows>();
      std::vector<rt::Promise<void>> pending;
      for (const auto& record : *limits) pending.emplace_back(readRepoUsageRow(usageByRepo, record));
      co_await rt::all(std::move(pending)).wait();
    }
    json::array rows;
    for (const auto& record : *limits) { auto row = json::value_from(record).as_object(); if (includeUsage) { const auto* usage = usageByRepo->get(record.owner + "/" + record.repo); row["usage"] = usage ? orNullJson(*usage) : json::value(nullptr); } rows.push_back(std::move(row)); }
    co_return http::json(200, json::object{{"ok", true}, {"scope", scope}, {"limits", rows}});
  }
  rt::Task<rt::Response> kernelUsageListResponse(std::string scope, bool includeUsage) {
    auto task = scope == "org" ? kernelOrgUsageLimitsResponse(scope, includeUsage) : kernelRepoUsageLimitsResponse(scope, includeUsage); auto result = co_await std::move(task); co_return result;
  }
  rt::Task<rt::Response> kernelOrgUsageSnapshotResponse(std::string owner, bool includeUsage) {
    const auto limitSnapshot = co_await qv::getKernelOrgUsageLimitSnapshot(owner);
    if (!limitSnapshot) co_return http::openaiError(500, "Failed to load kernel org usage limit", "server_error");
    ku::KernelUsageReadOptions options{.includeDaily = includeUsage, .dailyDays = KERNEL_USAGE_DAILY_DAYS};
    auto task = ku::getKernelOrgUsage(owner, options); const auto usage = co_await std::move(task);
    auto limit = json::value_from(limitSnapshot->record).as_object(); limit["source"] = limitSnapshot->source;
    co_return http::json(200, json::object{{"ok", true}, {"org", json::object{{"owner", owner}}}, {"limit", limit}, {"usage", orNullJson(usage)}});
  }
  rt::Task<rt::Response> kernelRepoUsageSnapshotResponse(std::string owner, std::string repo, bool includeUsage) {
    const auto limitSnapshot = co_await qv::getKernelUsageLimitSnapshot(owner, repo);
    if (!limitSnapshot) co_return http::openaiError(500, "Failed to load kernel usage limit", "server_error");
    ku::KernelUsageReadOptions options{.includeDaily = includeUsage, .dailyDays = KERNEL_USAGE_DAILY_DAYS};
    auto task = ku::getKernelUsage(owner, repo, options); const auto usage = co_await std::move(task);
    auto limit = json::value_from(limitSnapshot->record).as_object(); limit["source"] = limitSnapshot->source;
    co_return http::json(200, json::object{{"ok", true}, {"repo", json::object{{"owner", owner}, {"repo", repo}}}, {"limit", limit}, {"usage", orNullJson(usage)}});
  }
}
rt::Task<rt::Response> handleAdminKernelUsageGet(rt::Request req) {
  const auto kv = co_await databaseForRequest();
  if (!kv) co_return http::openaiError(500, "Deno KV is not available", "server_error");
  const rt::URL url(req.url()); const auto scope = api_keys::normalizeKernelScope(queryValue(url, "scope"));
  const auto listRequested = api_keys::shouldIncludeUsage(url.searchParams().get("list"));
  const auto inventoryRequested = api_keys::shouldIncludeUsage(url.searchParams().get("inventory"));
  const auto includeUsage = api_keys::shouldIncludeUsage(url.searchParams().get("include_usage"));
  if (inventoryRequested) { auto result = co_await kernelUsageInventoryResponse(scope); co_return result; }
  if (listRequested) { auto result = co_await kernelUsageListResponse(scope, includeUsage); co_return result; }
  const auto owner = api_keys::normalizeKernelRepoPart(queryValue(url, "owner"));
  if (!owner) co_return http::openaiError(400, "owner query parameter is required", "invalid_request_error");
  if (scope == "org") { auto result = co_await kernelOrgUsageSnapshotResponse(*owner, includeUsage); co_return result; }
  const auto repo = api_keys::normalizeKernelRepoPart(queryValue(url, "repo"));
  if (!repo) co_return http::openaiError(400, "repo query parameter is required", "invalid_request_error");
  auto result = co_await kernelRepoUsageSnapshotResponse(*owner, *repo, includeUsage); co_return result;
}
namespace {
  struct KernelUsageTarget { std::string scope; std::string owner; std::optional<std::string> repo; };
  // The shared `owner`/`repo`/`scope` validation for the Kernel usage writers.
  std::variant<KernelUsageTarget, rt::Response> resolveKernelUsageTarget(const json::value& raw) {
    const auto owner = api_keys::normalizeKernelRepoPart(rt::get(raw, "owner")); const auto repo = api_keys::normalizeKernelRepoPart(rt::get(raw, "repo"));
    if (!owner) return http::openaiError(400, "owner is required", "invalid_request_error");
    const auto* scopeRaw = rt::get(raw, "scope"); const json::value fallbackScope(repo ? "repo" : "org");
    const auto scope = api_keys::normalizeKernelScope(rt::isNullish(scopeRaw) ? &fallbackScope : scopeRaw);
    if (scope == "repo") { if (!repo) return http::openaiError(400, "repo is required for scope=repo", "invalid_request_error"); return KernelUsageTarget{"repo", *owner, repo}; }
    if (repo) return http::openaiError(400, "repo must be omitted for scope=org", "invalid_request_error");
    return KernelUsageTarget{"org", *owner, std::nullopt};
  }
  struct KernelUsageLimits { double usageLimitRequests; std::optional<double> windowMs; std::optional<double> expiresAtMs; bool resetUsage; };
  std::variant<KernelUsageLimits, rt::Response> resolveKernelUsageLimits(const json::value& raw) {
    const auto usageLimitRequests = api_keys::normalizeKernelUsageLimitInput(rt::get(raw, "usage_limit_requests"));
    if (!usageLimitRequests) return http::openaiError(400, "usage_limit_requests must be a non-negative number, -1, or 'unlimited'", "invalid_request_error");
    const auto* rawWindow = rt::get(raw, "window_ms"); const auto windowMs = api_keys::normalizeKernelWindowMsInput(rawWindow);
    if (rawWindow && !windowMs) return http::openaiError(400, "window_ms must be a positive number", "invalid_request_error");
    const auto* rawExpiry = rt::get(raw, "expires_at_ms"); const auto expiresAtMs = api_keys::normalizeKernelExpiresAtMsInput(rawExpiry, rt::dateNow());
    if (rawExpiry && !expiresAtMs) return http::openaiError(400, "expires_at_ms must be a Unix epoch ms timestamp in the future, or -1", "invalid_request_error");
    const auto* reset = rt::get(raw, "reset_usage");
    if (reset && !reset->is_bool()) return http::openaiError(400, "reset_usage must be a boolean", "invalid_request_error");
    return KernelUsageLimits{*usageLimitRequests, windowMs, expiresAtMs, reset && reset->is_bool() && reset->as_bool()};
  }
  rt::Task<rt::Response> kernelOrgUsageLimitSetResponse(std::string owner, KernelUsageLimits limits) {
    authority::native_kernel_contracts::KernelQuotaPolicySetOptions options{.resetUsage = limits.resetUsage, .windowMs = limits.windowMs, .expiresAtMs = limits.expiresAtMs};
    auto task = qw::setKernelOrgUsageLimit(owner, limits.usageLimitRequests, options); const auto updated = co_await std::move(task);
    if (!updated) co_return http::openaiError(409, "Concurrent modification; retry", "invalid_request_error");
    auto limit = json::value_from(*updated).as_object(); limit["source"] = "kv";
    co_return http::json(200, json::object{{"ok", true}, {"scope", "org"}, {"org", json::object{{"owner", owner}}}, {"limit", limit}});
  }
}
rt::Task<rt::Response> handleAdminKernelUsageSet(rt::Request req) {
  const auto kv = co_await databaseForRequest();
  if (!kv) co_return http::openaiError(500, "Deno KV is not available", "server_error");
  const auto raw = co_await request::readJsonBody(req);
  if (!rt::truthy(raw) || !utils::isRecord(raw)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
  const auto targetResult = resolveKernelUsageTarget(raw); if (const auto* response = std::get_if<rt::Response>(&targetResult)) co_return *response;
  const auto resolved = resolveKernelUsageLimits(raw); if (const auto* response = std::get_if<rt::Response>(&resolved)) co_return *response;
  const auto& target = std::get<KernelUsageTarget>(targetResult); const auto& limits = std::get<KernelUsageLimits>(resolved);
  if (target.scope == "org") { auto task = kernelOrgUsageLimitSetResponse(target.owner, limits); auto result = co_await std::move(task); co_return result; }
  authority::native_kernel_contracts::KernelQuotaPolicySetOptions options{.resetUsage = limits.resetUsage, .windowMs = limits.windowMs, .expiresAtMs = limits.expiresAtMs};
  auto task = qw::setKernelUsageLimit(target.owner, *target.repo, limits.usageLimitRequests, options); const auto updated = co_await std::move(task);
  if (!updated) co_return http::openaiError(409, "Concurrent modification; retry", "invalid_request_error");
  auto limit = json::value_from(*updated).as_object(); limit["source"] = "kv";
  co_return http::json(200, json::object{{"ok", true}, {"scope", target.scope}, {"repo", json::object{{"owner", target.owner}, {"repo", *target.repo}}}, {"limit", limit}});
}
namespace {
  rt::Task<rt::Response> kernelOrgUsageLimitDeleteResponse(std::string owner) {
    const auto deleted = co_await qw::deleteKernelOrgUsageLimit(owner);
    if (deleted && std::holds_alternative<std::string>(*deleted)) co_return http::openaiError(409, "Active Kernel quota reservations must settle before deletion", "invalid_request_error");
    if (!deleted) co_return http::openaiError(500, "Failed to delete kernel org usage limit", "server_error");
    if (!std::get<bool>(*deleted)) co_return http::openaiError(404, "Kernel org usage limit not found", "not_found");
    co_return http::json(200, json::object{{"ok", true}, {"scope", "org"}, {"org", json::object{{"owner", owner}}}, {"deleted", true}});
  }
  rt::Task<rt::Response> kernelRepoUsageLimitDeleteResponse(std::string owner, std::string repo) {
    const auto deleted = co_await qw::deleteKernelUsageLimit(owner, repo);
    if (deleted && std::holds_alternative<std::string>(*deleted)) co_return http::openaiError(409, "Active Kernel quota reservations must settle before deletion", "invalid_request_error");
    if (!deleted) co_return http::openaiError(500, "Failed to delete kernel usage limit", "server_error");
    if (!std::get<bool>(*deleted)) co_return http::openaiError(404, "Kernel usage limit not found", "not_found");
    co_return http::json(200, json::object{{"ok", true}, {"scope", "repo"}, {"repo", json::object{{"owner", owner}, {"repo", repo}}}, {"deleted", true}});
  }
}
rt::Task<rt::Response> handleAdminKernelUsageDelete(rt::Request req) {
  const auto kv = co_await databaseForRequest();
  if (!kv) co_return http::openaiError(500, "Deno KV is not available", "server_error");
  const auto raw = co_await request::readJsonBody(req);
  if (!rt::truthy(raw) || !utils::isRecord(raw)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
  const auto targetResult = resolveKernelUsageTarget(raw); if (const auto* response = std::get_if<rt::Response>(&targetResult)) co_return *response;
  const auto& target = std::get<KernelUsageTarget>(targetResult);
  if (target.scope == "org") { auto result = co_await kernelOrgUsageLimitDeleteResponse(target.owner); co_return result; }
  auto result = co_await kernelRepoUsageLimitDeleteResponse(target.owner, *target.repo); co_return result;
}
namespace {
  constexpr double QUOTA_PROJECTION_DEFAULT_WINDOW_DAYS = 30;
  constexpr double QUOTA_PROJECTION_MAX_BALANCE_SAMPLES = 365;
}
// Admin quota-runway view reads only compact stores, never raw request rows.
rt::Task<rt::Response> handleAdminProvidersQuotaProjection() { return handleAdminProvidersQuotaProjection(rt::Request("https://ai.ubq.fi/admin/providers/quota-projection")); }
rt::Task<rt::Response> handleAdminProvidersQuotaProjection(rt::Request request) {
  const auto kv = co_await databaseForRequest(); const auto nowMs = rt::dateNow(); const rt::URL url(request.url());
  const auto rawWindowDays = rt::parseInt(url.searchParams().get("window_days").value_or(""), 10);
  const auto windowDays = rt::Number::isInteger(rawWindowDays) && (rawWindowDays == 7 || rawWindowDays == 30 || rawWindowDays == 90) ? rawWindowDays : QUOTA_PROJECTION_DEFAULT_WINDOW_DAYS;
  const auto balanceWindowDays = mq::normalizeMeteredQuotaBalanceWindowDays(url.searchParams().get("balance_window_days"));
  const auto windowMs = windowDays * 24 * 60 * 60 * 1'000; const auto balanceWindowMs = balanceWindowDays * 24 * 60 * 60 * 1'000;
  // A closed interval can intersect one more aligned bucket than its duration alone implies.
  const auto balanceHistoryBucketMs = rt::Math::max(mq::METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS, rt::Math::ceil((balanceWindowDays * 24) / (QUOTA_PROJECTION_MAX_BALANCE_SAMPLES - 1)) * mq::METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS);
  rt::Promise<std::optional<std::string>> fingerprintPending(mq::meterQuotaAccountFingerprint(mq::readMeteredAccountCredentials()));
  auto safeFingerprint = fingerprintPending.catch_([](rt::Reason) { return std::optional<std::string>(); }); const auto accountFingerprint = co_await safeFingerprint.wait();
  mq::GetMeteredQuotaSnapshotOptions snapshotOptions; snapshotOptions.kv = kv;
  rt::Promise<std::optional<mq::MeteredQuotaSnapshot>> snapshotPending(mq::getConfiguredMeteredQuotaSnapshot(snapshotOptions));
  auto snapshotSafe = snapshotPending.catch_([](rt::Reason) { return std::optional<mq::MeteredQuotaSnapshot>(); });
  rt::Promise<std::optional<std::vector<rollups::PaidFallbackUsageRollup>>> rollupsPending = rt::Promise<std::optional<std::vector<rollups::PaidFallbackUsageRollup>>>::resolve(std::nullopt);
  rt::Promise<std::optional<std::vector<mq::MeteredQuotaBalanceSample>>> historyPending = rt::Promise<std::optional<std::vector<mq::MeteredQuotaBalanceSample>>>::resolve(std::nullopt);
  if (kv) {
    rollups::ListPaidFallbackUsageRollupsOptions rollupOptions{.sinceMs = nowMs - windowMs, .nowMs = nowMs};
    rollupsPending = nullableOnFailure(rollups::listPaidFallbackUsageRollups(kv, rollupOptions));
    mq::ReadMeteredQuotaBalanceHistoryOptions historyOptions{.sinceMs = nowMs - balanceWindowMs, .nowMs = nowMs, .accountFingerprint = accountFingerprint};
    historyPending = nullableOnFailure(mq::readMeteredQuotaBalanceHistory(kv, historyOptions));
  }
  stats::PaidFallbackLedgerGrowthOptions growthOptions{.nowMs = nowMs, .retentionMs = paid_fallback::ledger_state::PAID_FALLBACK_REQUEST_LOG_RETENTION_MS};
  rt::Promise<stats::PaidFallbackLedgerGrowthView> growthPending(stats::readPaidFallbackLedgerGrowth(kv, growthOptions));
  auto pending = rt::all(snapshotSafe, rollupsPending, historyPending, growthPending); auto values = co_await pending.wait();
  const auto& snapshot = std::get<0>(values); const auto& rollupRows = std::get<1>(values); const auto& sourceBalanceHistory = std::get<2>(values); const auto& ledgerGrowth = std::get<3>(values);
  const auto rollupCount = rollupRows ? static_cast<double>(rollupRows->size()) : 0; const auto balanceSamples = sourceBalanceHistory ? static_cast<double>(sourceBalanceHistory->size()) : 0;
  const auto projectionReadUnits = (kv ? 2 : 0) + (kv ? 1 + rollupCount : 0) + (kv ? 1 + balanceSamples : 0) + ledgerGrowth.read_units;
  stats::PaidFallbackLedgerProjectionStatsInput input{.windowDays = windowDays, .readUnits = projectionReadUnits, .rollupRows = rollupCount, .nowMs = nowMs};
  auto recordTask = stats::recordPaidFallbackLedgerProjectionStats(kv, input); static_cast<void>(co_await std::move(recordTask));
  const auto quota = projection::meteredQuotaRunwayView(snapshot);
  std::optional<std::vector<mq::MeteredQuotaBalanceSample>> balanceHistory;
  if (sourceBalanceHistory) balanceHistory = mq::resampleMeteredQuotaBalanceHistory(*sourceBalanceHistory, balanceHistoryBucketMs, QUOTA_PROJECTION_MAX_BALANCE_SAMPLES);
  const auto usage = projection::summarizePaidFallbackUsage(projection::groupPaidFallbackUsageRollups(rollupRows.value_or(std::vector<rollups::PaidFallbackUsageRollup>())), nowMs);
  json::array models;
  for (const auto& entry : usage) {
    json::array windows; for (const auto& window : entry.windows) if (window.window_days == windowDays) windows.push_back(json::value_from(window));
    json::array estimates; for (const auto& estimate : projection::projectPaidFallbackRunway(entry, quota, nowMs, sourceBalanceHistory)) if (estimate.window_days == windowDays) estimates.push_back(json::value_from(estimate));
    models.push_back(json::object{{"model", entry.model}, {"provider", entry.provider}, {"quota_source", entry.provider == "metered" ? json::value("metered") : json::value(nullptr)}, {"usage_source", rollups::PAID_FALLBACK_SETTLED_PROVIDERS.contains(entry.provider) ? "paid_fallback" : "observability"}, {"usage", windows}, {"estimates", estimates}});
  }
  auto quotaJson = json::value_from(quota).as_object(); rt::objectDelete(quotaJson, "last_inferred_credit_quota");
  co_return http::json(200, json::object{{"snapshot_at_ms", rt::makeNumber(nowMs)}, {"window_days", rt::makeNumber(windowDays)}, {"balance_window_days", rt::makeNumber(balanceWindowDays)}, {"retention", json::object{{"rollup_bucket_ms", rt::makeNumber(rollups::PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS)}, {"rollup_window_ms", rt::makeNumber(windowMs)}, {"balance_history_source_bucket_ms", rt::makeNumber(mq::METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS)}, {"balance_history_bucket_ms", rt::makeNumber(balanceHistoryBucketMs)}, {"balance_history_window_ms", rt::makeNumber(balanceWindowMs)}}}, {"quota", quotaJson}, {"models", models}, {"rollup_scan", rollupRows ? "ok" : "unavailable"}, {"balance_history_scan", balanceHistory ? "ok" : "unavailable"}, {"balance_history", balanceHistory ? json::value_from(*balanceHistory) : json::value(json::array())}, {"ledger_growth", stats::toJson(ledgerGrowth)}}, rt::Headers{{"Cache-Control", "no-store"}});
}
// One-time idempotent, resumable settled-row and window-TTL backfill.
rt::Task<rt::Response> handleAdminProvidersQuotaProjectionBackfill() { return handleAdminProvidersQuotaProjectionBackfill(rt::Request("https://ai.ubq.fi/admin/providers/quota-projection/backfill")); }
rt::Task<rt::Response> handleAdminProvidersQuotaProjectionBackfill(rt::Request request) {
  const auto kv = co_await databaseForRequest(); if (!kv) co_return http::openaiError(503, "KV is unavailable", "server_error");
  const rt::URL url(request.url()); const auto limitRaw = rt::parseInt(url.searchParams().get("limit").value_or(""), 10);
  const auto limit = rt::Number::isInteger(limitRaw) && limitRaw > 0 ? rt::Math::min(limitRaw, 10'000) : 5'000;
  try {
    backfill::BackfillPaidFallbackUsageRollupsOptions requestOptions{.limit = limit}; auto requestTask = backfill::backfillPaidFallbackUsageRollups(kv, requestOptions); const auto requests = co_await std::move(requestTask);
    backfill::BackfillPaidFallbackWindowTtlsOptions windowOptions{.limit = limit}; auto windowTask = backfill::backfillPaidFallbackWindowTtls(kv, windowOptions); const auto windows = co_await std::move(windowTask);
    co_return http::json(200, json::object{{"requests", json::value_from(requests)}, {"windows", json::value_from(windows)}, {"completed", !requests.truncated && !windows.truncated}});
  } catch (...) { const auto error = std::current_exception(); co_return http::openaiError(500, rt::isError(error) ? rt::errorMessage(error).value_or("") : "Paid fallback rollup backfill failed", "server_error"); }
}
namespace {
  rt::Task<json::object> accountIdentity(ce::CodexCapacityAccount account) {
    const auto accountIdHash = co_await utils::sha256Hex(account.account_id);
    const std::string cohortInput = std::string("uos-prompt-cache-account-cohort-v1\0", 35) + account.account_id;
    const auto accountCohortId = co_await utils::sha256Hex(cohortInput);
    co_return json::object{{"slot", rt::makeNumber(account.slot)}, {"account_id_hash", accountIdHash}, {"account_cohort_id", accountCohortId}};
  }
  rt::Task<json::object> resetSettingsRow(std::shared_ptr<rt::Kv> kv, json::object account, std::optional<ce::CodexCapacityAccount> credentials, rt::Request request) {
    const std::string accountIdHash(*rt::getString(account, "account_id_hash"));
    const auto usage = co_await cs::readCodexResetUsage(kv, accountIdHash);
    std::optional<double> availableCount; std::optional<std::vector<cp::CodexResetCreditDiagnostic>> credits;
    const auto diagnosticsSignal = rt::AbortSignal::any({request.signal(), rt::AbortSignal::timeout(5000)});
    std::optional<cp::UpstreamCodexUsageResetProviderOptions> inventoryOptions;
    if (credentials) inventoryOptions = cp::UpstreamCodexUsageResetProviderOptions{.codexBaseUrl = config::config.codexBaseUrl, .accountId = credentials->account_id, .accessToken = credentials->access_token, .userAgent = "codex_cli_rs/0.100.0 (ai.ubq.fi)"};
    try { if (!inventoryOptions) throw rt::JsError("codex reset settings account disappeared"); auto task = cp::readCodexResetAvailableCount(*inventoryOptions, diagnosticsSignal); availableCount = co_await std::move(task); } catch (...) {}
    try { if (!inventoryOptions) throw rt::JsError("codex reset settings account disappeared"); auto task = cp::readCodexResetCredits(*inventoryOptions, diagnosticsSignal); credits = co_await std::move(task); } catch (...) {}
    account["enabled"] = usage.allowed; account["available_count"] = orNullJson(availableCount); account["credits"] = orNullJson(credits); co_return account;
  }
  rt::Task<std::optional<provider::capacity_parse::ProviderCapacityOverageUsage>> readOverageUsageDiagnostics(std::optional<ce::CodexCapacityAccount> account, rt::AbortSignal signal) {
    if (!account) co_return std::nullopt;
    try {
      rt::RequestInit init{.method = "GET", .headers = rt::Headers{{"Accept", "application/json"}, {"Authorization", "Bearer " + account->access_token}, {"ChatGPT-Account-ID", account->account_id}, {"User-Agent", "codex_cli_rs/0.100.0 (ai.ubq.fi)"}}, .signal = signal, .redirect = "manual"};
      rt::Promise<rt::Response> pending(rt::fetch(provider::capacity_parse::codexUsageUrl(), init)); const auto response = co_await pending.wait();
      if (!response.ok()) { if (auto body = response.body()) { try { co_await body->cancel(); } catch (...) {} } co_return std::nullopt; }
      const auto raw = co_await response.json(); co_return provider::capacity_parse::parseCodexOverageUsage(raw);
    } catch (...) { co_return std::nullopt; }
  }
  rt::Task<void> refreshOverageUsageRoutingCache() {
    try { const auto entry = co_await ::uos::codex::auth::getAuthPoolEntry(true); auto task = co::loadOverageUsageSettings(entry.pool, true); co_await std::move(task); } catch (...) {}
  }
  rt::Task<json::object> overageSettingsRow(std::shared_ptr<rt::Kv> kv, json::object account, std::optional<ce::CodexCapacityAccount> credentials, rt::Request request) {
    const std::string accountIdHash(*rt::getString(account, "account_id_hash"));
    const auto allow = co_await co::readOverageUsage(kv, accountIdHash);
    const auto signal = rt::AbortSignal::any({request.signal(), rt::AbortSignal::timeout(5000)});
    auto task = readOverageUsageDiagnostics(credentials, signal); const auto overage = co_await std::move(task);
    account["allow"] = allow; account["overage"] = orNullJson(overage); co_return account;
  }
}
rt::Task<rt::Response> handleAdminCodexResetSettings(rt::Request request) {
  const auto kv = co_await databaseForRequest(); if (!kv) co_return http::openaiError(503, "Settings storage unavailable", "server_error");
  const auto accounts = co_await ce::getCodexCapacityAccounts(); std::vector<rt::Promise<json::object>> pending;
  for (const auto& account : accounts) pending.emplace_back(accountIdentity(account));
  const auto identities = co_await rt::all(std::move(pending)).wait();
  if (request.method() == "PATCH") {
    json::value raw; try { raw = co_await request.json(); } catch (...) {}
    const auto accountIdHash = rt::getString(raw, "account_id_hash"); const auto enabled = rt::getBool(raw, "enabled");
    if (!utils::isRecord(raw) || !accountIdHash || !enabled) co_return http::openaiError(400, "account_id_hash and boolean enabled are required", "invalid_request_error");
    if (std::none_of(identities.begin(), identities.end(), [&](const auto& account) { return rt::getString(account, "account_id_hash") == accountIdHash; })) co_return http::openaiError(409, "Subscription changed. Reload Providers.", "invalid_request_error");
    const auto key = cs::codexResetUsageKey(std::string(*accountIdHash)); const rt::KvValue value(json::object{{"enabled", *enabled}}); co_await kv->set(key, value);
    co_return http::json(200, json::object{{"account_id_hash", *accountIdHash}, {"enabled", *enabled}}, rt::Headers{{"Cache-Control", "no-store"}});
  }
  std::vector<rt::Promise<json::object>> rows;
  for (std::size_t index = 0; index < identities.size(); ++index) rows.emplace_back(resetSettingsRow(kv, identities[index], accounts[index], request));
  const auto data = co_await rt::all(std::move(rows)).wait(); co_return http::json(200, json::object{{"data", json::value_from(data)}}, rt::Headers{{"Cache-Control", "no-store"}});
}
rt::Task<rt::Response> handleAdminCodexOverageUsage(rt::Request request) {
  const auto kv = co_await databaseForRequest(); if (!kv) co_return http::openaiError(503, "Settings storage unavailable", "server_error");
  const auto accounts = co_await ce::getCodexCapacityAccounts(); std::vector<rt::Promise<json::object>> pending;
  for (const auto& account : accounts) pending.emplace_back(accountIdentity(account));
  const auto identities = co_await rt::all(std::move(pending)).wait();
  if (request.method() == "PATCH") {
    json::value raw; try { raw = co_await request.json(); } catch (...) {}
    const auto accountIdHash = rt::getString(raw, "account_id_hash"); const auto allow = rt::getBool(raw, "allow");
    if (!utils::isRecord(raw) || !accountIdHash || !allow) co_return http::openaiError(400, "account_id_hash and boolean allow are required", "invalid_request_error");
    if (std::none_of(identities.begin(), identities.end(), [&](const auto& account) { return rt::getString(account, "account_id_hash") == accountIdHash; })) co_return http::openaiError(409, "Subscription changed. Reload Providers.", "invalid_request_error");
    const std::string hash(*accountIdHash);
    co_await co::writeOverageUsage(kv, hash, *allow);
    co_await refreshOverageUsageRoutingCache();
    co_return http::json(200, json::object{{"account_id_hash", *accountIdHash}, {"allow", *allow}}, rt::Headers{{"Cache-Control", "no-store"}});
  }
  // The admin read refreshes the routing cache from the just-confirmed durable setting.
  co_await refreshOverageUsageRoutingCache(); std::vector<rt::Promise<json::object>> rows;
  for (std::size_t index = 0; index < identities.size(); ++index) rows.emplace_back(overageSettingsRow(kv, identities[index], accounts[index], request));
  const auto data = co_await rt::all(std::move(rows)).wait(); co_return http::json(200, json::object{{"data", json::value_from(data)}}, rt::Headers{{"Cache-Control", "no-store"}});
}
}
