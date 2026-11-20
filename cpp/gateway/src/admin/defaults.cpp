#include "uos/admin/defaults.hpp"
#include "uos/admin/callback_task.hpp"
#include "uos/admin/api_keys.hpp"
#include "uos/admin/codex.hpp"
#include "uos/cache/kv_migration_base.hpp"
#include "uos/cache/kv_migration_validation.hpp"
#include "uos/codex/auth.hpp"
#include "uos/defaults.hpp"
#include "uos/http.hpp"
#include "uos/kernel/quota_reservations.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/request.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/upstream_auth.hpp"
#include "uos/utils.hpp"
#include <algorithm>
#include <tuple>

namespace uos::admin::defaults {
namespace {
  namespace base = ::uos::defaults;
  namespace migration = cache::kv_migration_base;
  namespace kernel = ::uos::kernel::quota_reservations;
  std::optional<bool> parseBooleanParam(const rt::URL& url, const std::string& name) {
    const auto value = url.searchParams().get(name); if (!value) return std::nullopt;
    const auto normalized = rt::toLowerCase(rt::trim(*value));
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") return true;
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") return false;
    return std::nullopt;
  }
  std::optional<std::string> parseMigrationProfile(const rt::URL& url) {
    const auto raw = url.searchParams().get("profile"); const auto profile = raw ? rt::trim(*raw) : std::string();
    if (profile == "local") return "local";
    if (profile.empty() || profile == "prod") return "prod";
    return std::nullopt;
  }
  std::vector<std::string> splitNdjsonLines(const std::string& text) {
    static const rt::Regex newline("\\r?\\n"); std::vector<std::string> lines;
    for (const auto& line : rt::split(text, newline)) if (!rt::trim(line).empty()) lines.push_back(line);
    return lines;
  }
  rt::Task<std::shared_ptr<rt::Kv>> databaseForRequest() { auto p = kv::getKv(); auto database = co_await p.wait(); co_return database; }
  json::object defaultsBody(const std::string& model, const std::string& reasoning, double limit, double window) { return {{"defaults", json::object{{"model", model}, {"reasoning_effort", reasoning}, {"kernel_policy_limit_requests", limit}, {"kernel_policy_window_ms", window}}}}; }
  rt::Response invalid(const std::string& message, double status = 400, const std::string& code = "invalid_request_error") { return http::openaiError(status, message, code); }
  bool importsCodexAuth(const std::string& line) {
    try {
      return migration::parseKvMigrationEntryLine(line).key == ::uos::codex::auth::CODEX_AUTH_POOL_KV_KEY;
    } catch (...) {
      return false; // Invalid lines cannot write a record.
    }
  }
  struct AdminDefaultsIntent {
    bool writesModel; bool writesReasoning; bool wantsModelUpdate; bool writesKernelLimit; bool writesKernelWindow;
    std::optional<double> requestedKernelLimit; std::optional<double> requestedKernelWindow;
  };
  rt::Task<rt::Response> adminDefaultsGetResponse(std::shared_ptr<rt::Kv> database, AdminDefaultsDependencies dependencies) {
    rt::Promise<std::optional<runtime_config::RuntimeConfigV2>> runtime(runtime_config::loadRuntimeConfig(database));
    rt::Promise<rt::KvEntryMaybe<>> limit(database->get(base::DEFAULT_KERNEL_POLICY_LIMIT_KEY)); rt::Promise<rt::KvEntryMaybe<>> window(database->get(base::DEFAULT_KERNEL_POLICY_WINDOW_KEY));
    rt::Promise<metered_quota::MeteredQuotaDiagnostics> metered(dependencies.getMeteredQuotaDiagnostics
      ? detail::invokeOwnedTask(std::move(dependencies.getMeteredQuotaDiagnostics)) : metered_quota::getMeteredQuotaDiagnostics());
    const auto all = co_await rt::all(runtime, limit, window, metered).wait(); const auto& config = std::get<0>(all);
    auto response = defaultsBody(config ? config->default_model : "", config ? config->default_reasoning_effort : base::DEFAULT_REASONING_EFFORT, api_keys::normalizeKernelUsageLimitInput(rt::storedJsonView(std::get<1>(all).value)).value_or(base::DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS), api_keys::normalizeKernelWindowMsInput(rt::storedJsonView(std::get<2>(all).value)).value_or(base::DEFAULT_KERNEL_POLICY_WINDOW_MS));
    response["metered_quota"] = json::value_from(std::get<3>(all)); co_return http::json(200, response);
  }
  rt::Task<std::optional<rt::Response>> applyDefaultsUpdateAttempt(std::shared_ptr<rt::Kv> database, json::object raw, AdminDefaultsIntent intent) {
    rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
    rt::Promise<rt::KvEntryMaybe<>> r(database->get(runtime_config::RUNTIME_CONFIG_V2_KEY, strong)), l(database->get(base::DEFAULT_KERNEL_POLICY_LIMIT_KEY, strong)), w(database->get(base::DEFAULT_KERNEL_POLICY_WINDOW_KEY, strong));
    const auto all = co_await rt::all(r, l, w).wait(); const auto& runtimeEntry = std::get<0>(all); const auto& limitEntry = std::get<1>(all); const auto& windowEntry = std::get<2>(all);
    const auto runtime = runtime_config::normalizeRuntimeConfig(runtimeEntry.value); std::string model = runtime ? runtime->default_model : "", reasoning = runtime ? runtime->default_reasoning_effort : base::DEFAULT_REASONING_EFFORT;
    const double limit = intent.requestedKernelLimit.value_or(api_keys::normalizeKernelUsageLimitInput(rt::storedJsonView(limitEntry.value)).value_or(base::DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS));
    const double window = intent.requestedKernelWindow.value_or(api_keys::normalizeKernelWindowMsInput(rt::storedJsonView(windowEntry.value)).value_or(base::DEFAULT_KERNEL_POLICY_WINDOW_MS));
    std::optional<runtime_config::RuntimeConfigV2> nextRuntime;
    if (intent.wantsModelUpdate) {
      if (!runtime) co_return invalid("Runtime configuration is unavailable", 503, "server_error");
      constexpr auto unknownModel = "model is not in the stored Codex model list";
      auto nextModel = model;
      if (intent.writesModel) {
        nextModel = rt::trim(rt::getString(raw, "model").value_or(""));
        if (static_cast<std::size_t>(rt::length(nextModel)) > rt::limits::kModelIdCodeUnits) co_return invalid(unknownModel);
        static const rt::Regex whitespace("\\s");
        if (nextModel.empty() || whitespace.test(nextModel)) co_return invalid("model must be a non-empty string");
      }
      const auto snapshot = co_await models::codex_models_fetch::loadCodexModelsSnapshot(); const auto* models = snapshot ? rt::getArray(*snapshot, "models") : nullptr;
      if (!snapshot || !models || models->empty()) co_return invalid("No Codex model snapshot stored", 409);
      std::optional<json::object> record; for (const auto& row : *models) if (utils::isRecord(row) && row.is_object() && rt::getString(row, "slug") == nextModel) { record = row.as_object(); break; }
      if (!record) co_return invalid(unknownModel);
      const auto* effort = rt::get(*record, "default_reasoning_level"); const auto modelDefault = effort && effort->is_null() ? std::optional<std::string>("none") : base::normalizeReasoningEffort(effort); const auto levels = api_keys::extractModelReasoningLevels(record);
      const auto nextReasoning = intent.writesReasoning ? base::normalizeReasoningEffort(rt::get(raw, "reasoning_effort")) : std::optional(modelDefault.value_or(levels.empty() ? "none" : levels.front()));
      if (!nextReasoning) co_return invalid("reasoning_effort must be a non-empty string");
      runtime_config::BuildRuntimeConfigOptions options; options.defaultModel = nextModel; options.defaultReasoningEffort = nextReasoning;
      try { nextRuntime = runtime_config::buildRuntimeConfig(*snapshot, options); } catch (...) { auto response = codex::runtimeConfigErrorResponse(std::current_exception()); if (response) co_return *response; throw; }
      model = std::move(nextModel); reasoning = *nextReasoning;
    }
    if (!nextRuntime && !intent.writesKernelLimit && !intent.writesKernelWindow) co_return http::json(200, defaultsBody(model, reasoning, limit, window));
    std::optional<::uos::kernel::quota_v2::KernelDefaultWindowCutoverGuard> guard;
    const double currentWindow = api_keys::normalizeKernelWindowMsInput(rt::storedJsonView(windowEntry.value)).value_or(base::DEFAULT_KERNEL_POLICY_WINDOW_MS);
    if (intent.writesKernelWindow && window != currentWindow) {
      auto task = kernel::acquireKernelDefaultWindowCutover(database, limitEntry, windowEntry); const auto decision = co_await std::move(task);
      if (const auto* accepted = std::get_if<::uos::kernel::quota_v2::KernelDefaultWindowCutoverDecisionOk>(&decision)) guard = accepted->guard;
      else {
        const auto& reason = std::get<::uos::kernel::quota_v2::KernelDefaultWindowCutoverDecisionNotOk>(decision).reason;
        if (reason == "active_reservations") co_return invalid("Active Kernel quota reservations must settle before changing the default window", 409);
        if (reason == "concurrent_change") co_return std::nullopt;
        co_return invalid("Kernel quota ledger is unavailable", 503, "server_error");
      }
    }
    auto atomic = database->atomic(); atomic->check(runtimeEntry).check(limitEntry).check(windowEntry);
    if (guard) atomic->check(guard->entry).delete_(guard->key);
    if (nextRuntime) atomic->set(runtime_config::RUNTIME_CONFIG_V2_KEY, rt::KvValue(runtime_config::toJson(*nextRuntime)));
    if (intent.writesKernelLimit) atomic->set(base::DEFAULT_KERNEL_POLICY_LIMIT_KEY, rt::KvValue(limit));
    if (intent.writesKernelWindow) atomic->set(base::DEFAULT_KERNEL_POLICY_WINDOW_KEY, rt::KvValue(window));
    bool committed = false;
    rt::Reason failure;
    try {
      const auto result = co_await atomic->commit();
      committed = result.ok;
    } catch (...) { failure = std::current_exception(); }
    if (!committed && guard) { auto task = kernel::releaseKernelDefaultWindowCutover(database, *guard); co_await std::move(task); }
    if (failure) std::rethrow_exception(failure);
    if (!committed) co_return std::nullopt;
    if (nextRuntime) runtime_config::cacheRuntimeConfig(*nextRuntime);
    co_return http::json(200, defaultsBody(model, reasoning, limit, window));
  }
  rt::Task<rt::Response> adminDefaultsPostResponse(std::shared_ptr<rt::Kv> database, rt::Request req) {
    const auto parsed = co_await request::readJsonBody(req); if (!rt::truthy(parsed) || !utils::isRecord(parsed)) co_return invalid("Invalid JSON body");
    json::object raw; if (parsed.is_object()) raw = parsed.as_object(); else if (parsed.is_array()) for (std::size_t i = 0; i < parsed.as_array().size(); ++i) rt::objectDefine(raw, rt::numberToString(static_cast<double>(i)), parsed.as_array()[i]);
    for (const auto& field : rt::objectKeys(raw)) if (field != "model" && field != "reasoning_effort" && field != "kernel_policy_limit_requests" && field != "kernel_policy_window_ms") { http::OpenaiErrorOptions options; options.param = std::optional(field); co_return http::openaiError(400, "Unknown defaults field: " + field, "invalid_request_error", options); }
    AdminDefaultsIntent intent; intent.writesModel = rt::hasOwn(raw, "model"); intent.writesReasoning = rt::hasOwn(raw, "reasoning_effort"); intent.wantsModelUpdate = intent.writesModel || intent.writesReasoning; intent.writesKernelLimit = rt::hasOwn(raw, "kernel_policy_limit_requests"); intent.writesKernelWindow = rt::hasOwn(raw, "kernel_policy_window_ms");
    if (intent.writesKernelLimit) { intent.requestedKernelLimit = api_keys::normalizeKernelUsageLimitInput(rt::get(raw, "kernel_policy_limit_requests")); if (!intent.requestedKernelLimit) co_return invalid("kernel_policy_limit_requests must be a non-negative number or -1 for unlimited"); }
    if (intent.writesKernelWindow) { intent.requestedKernelWindow = api_keys::normalizeKernelWindowMsInput(rt::get(raw, "kernel_policy_window_ms")); if (!intent.requestedKernelWindow) co_return invalid("kernel_policy_window_ms must be a positive number"); }
    for (int attempt = 0; attempt < 3; ++attempt) { auto task = applyDefaultsUpdateAttempt(database, raw, intent); const auto response = co_await std::move(task); if (response) co_return *response; }
    co_return invalid("Defaults were modified concurrently; retry", 409);
  }
}
rt::Task<rt::Response> handleAdminKvMigrationImport(rt::Request req) {
  const auto database = co_await databaseForRequest(); if (!database) co_return invalid("Deno KV is not available; cannot import migration", 500, "server_error");
  const rt::URL url(req.url()); const auto profile = parseMigrationProfile(url); if (!profile) co_return invalid("profile must be local or prod");
  const double length = rt::stringToNumber(req.headers().get("content-length").value_or("0")); if (rt::Number::isFinite(length) && length > codex::MAX_KV_MIGRATION_BODY_BYTES) co_return invalid("Migration body is too large", 413);
  const auto body = co_await req.text(); if (static_cast<double>(rt::utf8ByteLength(body)) > codex::MAX_KV_MIGRATION_BODY_BYTES) co_return invalid("Migration body is too large", 413);
  const bool includeCache = parseBooleanParam(url, "include_cache").value_or(false), includeLegacy = parseBooleanParam(url, "include_legacy").value_or(migration::defaultIncludeLegacyForProfile(*profile)), overwrite = parseBooleanParam(url, "overwrite").value_or(false), write = parseBooleanParam(url, "write").value_or(false);
  if (write && parseBooleanParam(url, "dry_run").value_or(false)) co_return invalid("dry_run and write are mutually exclusive");
  migration::KvMigrationImportOptions options{{*profile, includeCache, includeLegacy}, overwrite, !write}; const auto lines = splitNdjsonLines(body);
  if (write && *profile == "local" && upstream_auth::followerMode() && std::any_of(lines.begin(), lines.end(), importsCodexAuth))
    co_return invalid(std::string(upstream_auth::FOLLOWER_READ_ONLY), 409, "upstream_auth_follower");
  auto task = migration::importKvMigrationLines(database, lines, options); const auto result = co_await std::move(task); const auto encoded = json::value_from(result);
  json::object summary{{"profile", *profile}, {"include_cache", includeCache}, {"include_legacy", includeLegacy}, {"overwrite", overwrite}, {"dry_run", !write}}; rt::objectAssign(summary, encoded.as_object());
  co_return http::json(result.errors > 0 ? 422 : 200, summary);
}
rt::Task<rt::Response> handleAdminKvMigrationValidate() { const auto database = co_await databaseForRequest(); if (!database) co_return invalid("Deno KV is not available; cannot validate migration", 500, "server_error"); const auto result = co_await cache::kv_migration_validation::validateKvMigrationTarget(database); co_return http::json(200, json::value_from(result)); }
rt::Task<rt::Response> handleAdminDefaults(rt::Request req) { return handleAdminDefaults(req, AdminDefaultsDependencies{}); }
rt::Task<rt::Response> handleAdminDefaults(rt::Request req, AdminDefaultsDependencies dependencies) {
  auto database = co_await databaseForRequest(); if (!database) co_return invalid("Deno KV is not available; cannot manage defaults", 500, "server_error");
  if (req.method() == "GET") { auto task = adminDefaultsGetResponse(database, dependencies); auto response = co_await std::move(task); co_return response; }
  if (req.method() == "POST") { auto task = adminDefaultsPostResponse(database, req); auto response = co_await std::move(task); co_return response; }
  co_return invalid("Method not allowed", 405, "method_not_allowed");
}
}
