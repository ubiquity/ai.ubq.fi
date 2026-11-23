#pragma once
// Credential-free SQLite replay harness for the seven native authority module ports.
#include "uos/authority/native_authority_runtime.hpp"
#include "uos/authority/native_authority_principals.hpp"
#include "uos/authority/native_authority_kernel.hpp"
#include "uos/authority/native_authority_codex.hpp"
#include "uos/authority/native_api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/config.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/test_async.hpp"
#include <algorithm>
#include <cstdio>
#include <memory>

namespace uos::tests::helpers::native_authority_harness {
  namespace contracts = authority::native_authority_contracts;
  namespace runtime = authority::native_runtime_contracts;
  namespace principal = authority::native_principal_contracts;
  namespace sql_module = authority::native_authority_sql;
  namespace codex = authority::native_codex_contracts;
  inline constexpr double NOW = 1800000000000;
  inline const json::value& vectors() {
    static const auto value = [] {
      const std::string encoded(
#include "tests/native_authority_engine_vectors.inc"
      );
      const auto bytes = rt::gzip::gunzipSync(rt::fromBase64(encoded));
      return rt::jsonParse(std::string(bytes.begin(), bytes.end()));
    }();
    return value;
  }
  inline contracts::NativeSqlStorage storage(rt::sqlite::Database database) {
    contracts::NativeSqlStorage result;
    result.sql.exec = [database](std::string query, std::vector<contracts::NativeSqlStorage::Binding> bindings) {
      std::vector<rt::sqlite::Binding> converted;
      for (const auto& binding : bindings) converted.push_back(std::visit([](const auto& value) { return rt::sqlite::Binding(value); }, binding));
      contracts::NativeSqlStorage::Result output;
      for (const auto& row : database.query(query).all(converted)) output.rows.push_back(rt::sqlite::toJson(row));
      return output;
    };
    result.transaction = [database](std::function<void()> callback) { database.transaction(callback)(); };
    return result;
  }
  template<class T> T decode(const json::value& value) { return json::value_to<T>(value); }
  inline std::string text(const json::value& value, const std::string& key, const std::string& fallback = "") { const auto found = rt::getString(value, key); return found ? std::string(*found) : fallback; }
  inline std::optional<std::string> maybeText(const json::value& value, const std::string& key) { const auto found = rt::getString(value, key); return found ? std::optional<std::string>(std::string(*found)) : std::nullopt; }
  inline json::value nullable(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }
  inline json::value nullable(const std::optional<double>& value) { return value ? json::value(rt::makeNumber(*value)) : json::value(nullptr); }
  inline contracts::NativeKey keyOf(const json::value& value) {
    contracts::NativeKey key;
    for (const auto& part : value.as_array()) {
      if (part.is_string()) key.emplace_back(std::string(part.as_string()));
      else if (rt::isNumber(part)) key.emplace_back(rt::toNumber(part));
      else if (part.is_bool()) key.emplace_back(part.as_bool());
      else if (const auto bigint = rt::getString(part, "$bigint")) key.emplace_back(rt::BigInt::fromString(*bigint));
      else throw rt::JsError("unsupported fixture key part");
    }
    return key;
  }
  inline json::array keyJson(const contracts::NativeKey& key) {
    json::array result;
    for (const auto& part : key) std::visit([&](const auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, rt::BigInt>) result.push_back(json::object{{"$bigint", value.toString()}});
      else if constexpr (std::is_same_v<T, rt::KvBytes>) result.push_back(json::value_from(value));
      else result.push_back(json::value_from(value));
    }, part.value);
    return result;
  }
  inline json::object readJson(const contracts::NativeRead<json::value>& row) { return {{"value", row.value.value_or(nullptr)}, {"revision", nullable(row.revision)}, {"versionstamp", nullable(row.versionstamp)}, {"expiresAtMs", nullable(row.expiresAtMs)}}; }
  inline json::array pageJson(const std::vector<sql_module::NativePageEntry<>>& page) { json::array result; for (const auto& entry : page) result.push_back(json::object{{"key", keyJson(entry.key)}, {"encoded", entry.encoded}, {"row", readJson(entry.row)}}); return result; }
  inline sql_module::NativeIndex indexOf(const json::value* value) { if (!value) return {}; return {maybeText(*value, "scope"), maybeText(*value, "parent"), rt::getNumber(*value, "due")}; }
  struct Isolated {
    rt::env::ScopedEnvironment environment;
    std::string previous;
    Isolated() : environment({{"HOME", "/nonexistent/uos-native-authority-fixture"}}), previous(config::config.codexAuthJsonB64) { config::config.codexAuthJsonB64.clear(); }
    ~Isolated() { config::config.codexAuthJsonB64 = previous; }
  };
  class Fixture {
    Isolated isolated;
    std::uint64_t randomCounter = 0;
    rt::crypto::ScopedRandomSource random;
  public:
    json::value spec;
    json::value options;
    json::object vars;
    json::array events;
    double now = NOW;
    ::uos::types::ApiKeyRecord record;
    contracts::NativeAuthorityInitial initial;
    contracts::NativeCapability cap;
    runtime::NativeRuntimeInitial runtimeInitial;
    principal::NativePrincipalControlInitial principalInitial;
    runtime::NativeTrustedRoutingDecisionWithoutIntent routingDecision;
    rt::sqlite::Database database;
    sql_module::NativeAuthoritySql sql;
    authority::native_authority_api::NativeAuthorityApi api;
    authority::native_authority_paid::NativeAuthorityPaid paid;
    authority::native_authority_kernel::NativeAuthorityKernel kernel;
    authority::native_authority_codex::NativeAuthorityCodex codexAuthority;
    authority::native_authority_runtime::NativeAuthorityRuntime runtimeAuthority;
    std::unique_ptr<authority::native_authority_principals::NativeAuthorityPrincipals> principals;
    explicit Fixture(json::value scenario) : random([this](std::span<std::uint8_t> bytes) { std::fill(bytes.begin(), bytes.end(), 0); const auto count = ++randomCounter; for (std::size_t i = 0; i < std::min<std::size_t>(6, bytes.size()); ++i) bytes[bytes.size() - 1 - i] = static_cast<std::uint8_t>(count >> (i * 8)); }), spec(std::move(scenario)), options(*rt::get(spec, "options")), record(decode<::uos::types::ApiKeyRecord>(*rt::get(spec, "record"))), initial(decode<contracts::NativeAuthorityInitial>(*rt::get(spec, "initial"))), cap(decode<contracts::NativeCapability>(*rt::get(spec, "cap"))), routingDecision(decode<runtime::NativeTrustedRoutingDecisionWithoutIntent>(*rt::get(spec, "routingDecision"))), database(":memory:"), sql(storage(database), initial.namespace_, initial.epoch), api(sql), paid(sql, api), kernel(sql, api, initial), codexAuthority(sql, api), runtimeAuthority(sql, api, runtimeInitial) {
      runtimeInitial.grants = decode<std::vector<runtime::NativeRuntimeGrant>>(*rt::get(spec, "runtimeGrants")); runtimeInitial.records = {{record, authority::native_api_key_policy::apiKeyHashRecordFor(record)}};
      runtimeInitial.verifier.verify = [this](json::value context, std::string intent, double) {
        if (const auto message = rt::getString(context, "error")) throw rt::JsError(std::string(*message));
        runtime::NativeRuntimeIdentity identity = cap.keyId ? runtime::NativeRuntimeIdentity(runtime::NativeRuntimeIdentityKvApiKey{"key"}) : runtime::NativeRuntimeIdentity(runtime::NativeRuntimeIdentityDisabledLocal{std::nullopt});
        if (const auto* configured = rt::get(options, "identity")) identity = decode<runtime::NativeRuntimeIdentity>(*configured);
        const runtime::VerifiedNativeRuntime base{cap.id, cap.subjectId, cap.contextBinding, intent, identity, cap.keyId ? std::optional<runtime::NativeRuntimeApiScope>({{"ref", cap.principalId, "key", "e"}, "synthetic-hash"}) : std::nullopt, cap.kernel ? std::optional<runtime::NativeRuntimeKernel>({cap.kernel->owner, cap.kernel->repo}) : std::nullopt};
        const auto encoded = json::value_from(base); const auto merged = rt::objectSpread(encoded.as_object(), context.as_object());
        return rt::fulfilledTask(decode<runtime::VerifiedNativeRuntime>(merged));
      };
      runtimeInitial.verifier.verifyRouting = [this](json::value context, runtime::NativeValidatedRequest, std::string intent, double) {
        const auto encoded = json::value_from(routingDecision); const auto value = rt::objectSpread(encoded.as_object(), json::object{{"intentDigest", intent}}, context.as_object()); return rt::fulfilledTask(decode<runtime::NativeTrustedRoutingDecision>(value));
      };
      if (rt::getBool(options, "retained") == true) {
        runtime::NativeRetainedHistoryInitial retained;
        retained.verifier.verify = [this](json::value context, std::string intent, double) {
          const runtime::VerifiedNativeRetainedHistory base{initial.namespace_, "binding", "api-key:key", "key", {}, "retained", "e", "retainer", "retained-context", intent};
          const auto encoded = json::value_from(base); const auto value = rt::objectSpread(encoded.as_object(), context.as_object()); return rt::fulfilledTask(decode<runtime::VerifiedNativeRetainedHistory>(value));
        };
        runtimeInitial.retainedHistory = retained;
      }
      principalInitial.grants = decode<std::vector<principal::NativeKeyControlGrant>>(*rt::get(spec, "controlGrants")); principalInitial.records = {{record, authority::native_api_key_policy::apiKeyHashRecordFor(record)}};
      principalInitial.verifier.verify = [](json::value context, std::string intent, double) {
        principal::VerifiedControlIdentity value;
        if (text(context, "kind", "key_admin") == "principal_resolver") value = principal::VerifiedKeyAuth{text(context, "subjectId", "resolver"), text(context, "contextBinding", "resolver-context"), text(context, "intentDigest", intent), text(context, "principalId", "api-key:key"), text(context, "tokenHash", "synthetic-hash")};
        else {
          principal::VerifiedKeyAdmin admin{text(context, "subjectId", "operator"), text(context, "contextBinding", "admin-context"), text(context, "intentDigest", intent), rt::hasOwn(context.as_object(), "targetKeyId") ? maybeText(context, "targetKeyId") : std::optional<std::string>("key"), {.request_id = "fixture-control"}};
          if (const auto* token = rt::get(context, "createToken")) { if (token->is_null()) admin.createToken = nullptr; else admin.createToken = std::string(token->as_string()); }
          value = admin;
        }
        return rt::fulfilledTask(value);
      };
      auto& sources = principalInitial.sources; sources.localDevelopmentKeyId = rt::getBool(options, "local") == true ? "key" : "local-key";
      sources.defaults = [this](double time) { events.push_back(json::array{"defaults", rt::makeNumber(time)}); return principal::NativePrincipalControlInitial::Sources::Defaults{86400000, 20}; };
      sources.defaultPaidPolicy = [this] { events.push_back(json::array{"defaultPaid"}); return paid_fallback::index::PaidFallbackPolicyFields{false, 0, 0, 0, std::nullopt, {}, 0, json::object(), std::nullopt}; };
      sources.generateToken = [this] { events.push_back(json::array{"token"}); return "u_" + std::string(64, 'a'); };
      sources.hashToken = [this](std::string token) { events.push_back(json::array{"hash"}); if (rt::getBool(vars, "failHash") == true) { vars["failHash"] = false; throw rt::JsError("hash-source-fault"); } return rt::fulfilledTask(std::string("hash:") + token); };
      sources.randomUUID = [this] { events.push_back(json::array{"uuid"}); return rt::crypto::randomUUID(); };
      sources.audit.randomUUID = [this] { events.push_back(json::array{"audit.uuid"}); return rt::crypto::randomUUID(); };
      sources.audit.now = [this] { events.push_back(json::array{"audit.now"}); return now; };
      sources.audit.release = [this] { events.push_back(json::array{"release"}); return std::string("fixture-release"); };
      sources.initializePricing = [this] { events.push_back(json::array{"pricing"}); if (rt::getBool(vars, "failPricing") == true) throw rt::JsError("pricing-fault"); return rt::fulfilledTask(paid_fallback::index::InitializePaidFallbackPolicyResult{{"fixture"}, 100, json::object{{"fixture", 40}}, now}); };
      sources.pricingFailure = [](rt::Reason error) { return principal::NativeControlOutcomeRefused{503, "pricing_error", rt::errorMessage(error).value_or("")}; };
      sources.allowUnrosteredUnlimited = [this](principal::VerifiedKeyAdmin, ::uos::types::ApiKeyRecord) -> std::optional<bool> { if (const auto* value = rt::get(options, "allowUnrostered")) return rt::getBool(*value); return false; };
      sources.cleanupApiLogs = [this](std::string keyId) { events.push_back(json::array{"cleanup", keyId}); if (rt::getBool(vars, "failCleanup") == true) { vars["failCleanup"] = false; throw rt::JsError("cleanup-fault"); } return rt::fulfilledTask(); };
      principals = std::make_unique<authority::native_authority_principals::NativeAuthorityPrincipals>(sql, api, paid, principalInitial, [this] { return now; });
      sql.transaction([&] {
        sql.write("kernel_default", {"default", "kernel_policy_limit_requests"}, initial.kernelDefaults.limit); sql.write("kernel_default", {"default", "kernel_policy_window_ms"}, 60000.0); sql.write("authority_config", {"config"}, contracts::NativeAuthorityConfig{initial.providerHops}); sql.write("api_policy", api_keys::apiKeyHashKey(record.hash), initial.keys.front()); sql.write("codex_pool", codex::CODEX_AUTH_POOL_KV_KEY, initial.pool); sql.write("codex_capacity", codex::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, initial.capacity);
        for (const auto& owner : initial.owners) sql.write("codex_owner", {owner.routingAccountIdHash}, owner);
        runtimeAuthority.initialize(); principals->initialize();
        sql.write("principal_resolution", {"e", "resolver", "api-key:key", "key"}, principal::NativePrincipalResolution{"ref", "api-key:key", "key", "e", "synthetic-hash", *sql.read("api_key_hash", api_keys::apiKeyHashKey(record.hash), now).revision, "resolver"}, std::nullopt, {.scope = "key"});
      });
      vars["record"] = json::value_from(record); vars["keyPolicy"] = json::value_from(initial.keys.front()); vars["cap"] = json::value_from(cap); vars["initial"] = json::value_from(initial); vars["routingDecision"] = json::value_from(routingDecision);
    }
    json::value resolve(const json::value& value) const {
      if (value.is_string() && value.as_string().starts_with('$')) {
        const auto path = rt::split(std::string(value.as_string()).substr(1), "."); const auto* current = rt::get(vars, path.front());
        for (std::size_t i = 1; current && i < path.size(); ++i) { if (current->is_array()) current = rt::get(current->as_array(), static_cast<std::size_t>(rt::parseInt(path[i], 10))); else current = rt::get(*current, path[i]); }
        if (!current) throw rt::JsError("missing fixture reference " + std::string(value.as_string()));
        return *current;
      }
      if (value.is_array()) { json::array result; for (const auto& item : value.as_array()) result.push_back(resolve(item)); return result; }
      if (value.is_object()) { json::object result; for (const auto& field : value.as_object()) result[field.key()] = resolve(field.value()); return result; }
      return value;
    }
    json::object snapshot() const {
      json::object rows; const auto tables = database.query("SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'uos_native_%' AND name<>'uos_native_authority_meta' ORDER BY name").all();
      for (const auto& table : tables) { const auto name = *rt::sqlite::getString(table.get(std::string_view("name"))); json::array entries; for (const auto& row : database.query("SELECT * FROM " + name + " ORDER BY row_key").all()) { auto encoded = rt::sqlite::toJson(row); encoded["row_json"] = rt::jsonParse(*rt::getString(encoded, "row_json")); entries.push_back(encoded); } if (!entries.empty()) rows[name] = entries; }
      json::array meta; for (const auto& row : database.query("SELECT * FROM uos_native_authority_meta").all()) meta.push_back(rt::sqlite::toJson(row));
      return {{"meta", meta}, {"rows", rows}, {"events", events}};
    }
    json::value sync(const json::value& action) {
      const auto op = text(action, "op"); const auto args = rt::getObject(action, "args") ? *rt::getObject(action, "args") : json::object(); const auto time = rt::getNumber(action, "now").value_or(now); auto capability = cap;
      if (const auto* patch = rt::getObject(action, "cap")) { const auto encoded = json::value_from(cap); capability = decode<contracts::NativeCapability>(rt::objectSpread(encoded.as_object(), *patch)); }
      const auto handle = text(args, "handle");
      if (op == "clock") { now = *rt::getNumber(action, "value"); return nullptr; }
      if (op == "state") { vars[text(action, "key")] = *rt::get(action, "value"); return nullptr; }
      if (op == "sql.write" || op == "sql.patch") { auto value = *rt::get(action, op == "sql.patch" ? "patch" : "value"); const auto key = keyOf(*rt::get(action, "key")); const auto table = text(action, "table"); if (op == "sql.patch") value = rt::objectSpread(sql.read(table, key, time).value->as_object(), value.as_object()); sql.write(table, key, value, rt::getNumber(action, "expiry"), indexOf(rt::get(action, "index"))); return nullptr; }
      if (op == "sql.delete") { sql.delete_(text(action, "table"), keyOf(*rt::get(action, "key"))); return nullptr; }
      if (op == "sql.exec") { json::array output; std::vector<rt::sqlite::Binding> bindings; if (const auto* values = rt::getArray(action, "bindings")) for (const auto& value : *values) bindings.emplace_back(value); for (const auto& row : database.query(text(action, "query")).all(bindings)) output.push_back(rt::sqlite::toJson(row)); return output; }
      if (op == "sql.initialize") { sql.initialize([&] { sql.write("kernel_default", {"initialized-callback"}, rt::getNumber(action, "value").value_or(1)); if (rt::getBool(action, "fail") == true) throw rt::JsError("initialize-fault"); }); return nullptr; }
      if (op == "sql.paidAttempt") { const auto value = sql.paidAttempt(text(action, "keyId", "key"), text(action, "billingId"), time); return value ? json::value_from(*value) : json::value(nullptr); }
      if (op == "sql.principalReference") { const auto value = sql.principalReference(text(action, "id", "ref"), text(action, "keyId", "key"), time); return value ? json::value_from(*value) : json::value(nullptr); }
      if (op == "sql.read") return readJson(sql.read(text(action, "table"), keyOf(*rt::get(action, "key")), time));
      if (op == "sql.page") return pageJson(sql.page(text(action, "table"), time, text(action, "after"), rt::getNumber(action, "limit").value_or(100), indexOf(rt::get(action, "filter"))));
      if (op == "sql.restartPage") { sql_module::NativeAuthoritySql restarted(storage(database), initial.namespace_, "e"); return pageJson(restarted.page(text(action, "table"), time, text(action, "after"), rt::getNumber(action, "limit").value_or(100))); }
      if (op == "sql.newEpoch") { sql_module::NativeAuthoritySql restarted(storage(database), initial.namespace_, text(action, "epoch")); return nullptr; }
      if (op == "sql.throwAfterOpen") { const auto opened = runtimeAuthority.open(capability, {"rollback", "/v1/responses", "/v1/responses"}, time); api.reserve(capability, {text(opened, "handle")}, time); throw rt::JsError("synthetic commit fault"); }
      if (op == "sql.async") { sql.transaction([] { return rt::Promise<int>::resolve(1); }); return nullptr; }
      if (op == "api.contenders") { json::array outcomes; for (int i = 0; i < 20; ++i) { const auto opened = runtimeAuthority.open(capability, {"contender-" + std::to_string(i), "/v1/responses", "/v1/responses"}, time); if (i % 2) { sql_module::NativeAuthoritySql second(storage(database), initial.namespace_, "e"); authority::native_authority_api::NativeAuthorityApi other(second); outcomes.push_back(other.reserve(capability, {text(opened, "handle")}, time)); } else outcomes.push_back(api.reserve(capability, {text(opened, "handle")}, time)); } return outcomes; }
      if (op == "open") return runtimeAuthority.open(capability, decode<contracts::NativeArguments::openAuthenticatedRequest>(args), time);
      if (op == "api.reserve") return api.reserve(capability, decode<contracts::NativeArguments::reserveApiUsage>(args), time);
      if (op == "api.dispatch") return api.dispatch(capability, decode<contracts::NativeArguments::markApiDispatch>(args), time);
      if (op == "api.start") return api.start(capability, decode<contracts::NativeArguments::recordTransportStarted>(args), text(action, "operationId", "start"), time);
      if (op == "api.release") return api.release(capability, decode<contracts::NativeArguments::releaseApiUndispatched>(args), time);
      if (op == "api.cancel") return api.cancel(capability, handle, time);
      if (op == "api.status") return api.status(capability, handle, time);
      if (op == "api.reset") return api.inspectKeyReset(text(action, "keyId", "key"), time);
      if (op == "api.usage") return rt::makeNumber(api.usage(initial.keys.front().api, time));
      if (op == "metadata") return runtimeAuthority.metadata(capability, decode<contracts::NativeArguments::bindValidatedRequestMetadata>(args), time);
      if (op == "runtime.authorizeRetained") {
        const auto binding = rt::get(action, "binding") ? decode<runtime::NativeRetainedHistoryBinding>(*rt::get(action, "binding")) : runtime::NativeRetainedHistoryBinding{"retained_history", "retained", "retained-context", json::object()};
        const auto handles = rt::get(action, "handles") ? decode<std::vector<std::string>>(*rt::get(action, "handles")) : std::vector<std::string>();
        const runtime::VerifiedNativeRetainedHistory base{initial.namespace_, "binding", "api-key:key", "key", handles, "retained", "e", "retainer", "retained-context", "intent"}; const auto encoded = json::value_from(base); auto verified = encoded;
        if (const auto* patch = rt::getObject(action, "verified")) verified = rt::objectSpread(encoded.as_object(), *patch);
        const contracts::NativeCommand command{"e", "retained", text(action, "method", "releaseApiUndispatched"), args};
        return json::value_from(runtimeAuthority.authorizeRetained(binding, decode<runtime::VerifiedNativeRetainedHistory>(verified), command, time));
      }
      if (op == "runtime.authorize") { const auto encoded = json::value_from(capability); runtime::VerifiedNativeRuntime verified; if (const auto* configured = rt::get(action, "verified")) verified = decode<runtime::VerifiedNativeRuntime>(*configured); else { runtime::NativeRuntimeIdentity identity = capability.keyId ? runtime::NativeRuntimeIdentity(runtime::NativeRuntimeIdentityKvApiKey{"key"}) : runtime::NativeRuntimeIdentity(runtime::NativeRuntimeIdentityDisabledLocal{std::nullopt}); if (const auto* configuredIdentity = rt::get(options, "identity")) identity = decode<runtime::NativeRuntimeIdentity>(*configuredIdentity); verified = {"binding", "subject", "context", "intent", identity, capability.keyId ? std::optional<runtime::NativeRuntimeApiScope>({{"ref", capability.principalId, "key", "e"}, "synthetic-hash"}) : std::nullopt, capability.kernel ? std::optional<runtime::NativeRuntimeKernel>({capability.kernel->owner, capability.kernel->repo}) : std::nullopt}; } return json::value_from(runtimeAuthority.authorize({{"runtime", "context", json::object()}, verified}, time)); }
      if (op == "routing" || op == "runtime.assertRouting") { const auto encoded = json::value_from(routingDecision); auto decision = rt::objectSpread(encoded.as_object(), json::object{{"intentDigest", "intent"}}); if (const auto* patch = rt::getObject(action, "decision")) decision = rt::objectSpread(decision, *patch); const auto parsed = decode<runtime::NativeTrustedRoutingDecision>(decision); const auto request = api.validated(capability, handle, time); if (op == "routing") return runtimeAuthority.bindRouting(capability, request, parsed, time); runtimeAuthority.assertRouting(capability, request, parsed, time); return nullptr; }
      if (op == "kernel.reserve") return kernel.reserve(capability, handle, time);
      if (op == "kernel.intent") return kernel.intent(capability, decode<contracts::NativeArguments::recordKernelTerminalIntent>(args), time);
      if (op == "kernel.settle") return kernel.settle(capability, decode<contracts::NativeArguments::settleKernelUsage>(args), time);
      if (op == "kernel.renew") return kernel.renew(capability, handle, time);
      if (op == "kernel.reclaim") return kernel.reclaim(capability, decode<contracts::NativeArguments::reclaimExpiredKernelReservations>(args), time);
      if (op == "kernel.policy") { if (rt::hasOwn(args, "usageLimitRequests")) return kernel.policy(capability, decode<contracts::NativeArguments::setKernelQuota>(args), time); return kernel.policy(capability, decode<contracts::NativeArguments::deleteKernelQuota>(args), time); }
      if (op == "kernel.cutover") return kernel.cutover(time);
      if (op == "paid.reserve") return paid.reserve(capability, decode<contracts::NativeArguments::reservePaidAttempt>(args), time);
      if (op == "paid.lifecycle") return paid.lifecycle(capability, decode<contracts::NativeArguments::recordPaidAttemptLifecycle>(args), time);
      if (op == "paid.release") return paid.release(capability, text(args, "attemptHandle"), time);
      if (op == "paid.defer") return paid.defer(capability, text(args, "attemptHandle"), time);
      if (op == "paid.billing") return paid.billing(capability, decode<contracts::NativeArguments::recordVerifiedPaidBilling>(args), time);
      if (op == "paid.claim") return paid.claim(capability, decode<contracts::NativeArguments::claimPaidReconciliation>(args), time);
      if (op == "paid.finish") return paid.finish(capability, text(args, "lease"), time);
      if (op == "paid.backfill") return paid.backfill(capability, decode<contracts::NativeArguments::foldPaidBackfillPage>(args), time);
      if (op == "paid.deletion") return paid.deletion(capability, time);
      if (op == "paid.assertStart") { const auto value = paid.assertStart(capability, decode<contracts::NativeArguments::recordTransportStarted>(args), time); return value ? json::value_from(*value) : json::value(nullptr); }
      if (op == "codex.proof") {
        const auto request = sql.read("request_lifetime", {handle}, time).value.value(); const auto& selection = request.as_object().at("selection").as_object(); auto account = selection.at("blockedAccounts").as_array().front().as_object(); const auto reset = account.at("quotaResetAtMs"); rt::objectDelete(account, "quotaResetAtMs"); const auto pool = sql.read("codex_pool", codex::CODEX_AUTH_POOL_KV_KEY, time), capacity = sql.read("codex_capacity", codex::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, time);
        const json::object proof{{"id", "proof"}, {"account", account}, {"quotaResetAtMs", reset}, {"routingGeneration", account.at("routingGeneration")}, {"activeSnapshot", selection.at("activeSnapshot").as_object().at("selection")}, {"poolRevision", nullable(pool.revision)}, {"capacityRevision", nullable(capacity.revision)}, {"verified", true}}; sql.write("codex_reset", {"proof"}, proof); return proof;
      }
      if (op == "codex.select") return codexAuthority.select(capability, handle, time);
      if (op == "codex.signal") { std::optional<authority::native_codex_429::Codex429Classification> parsed; if (const auto* input = rt::get(action, "parsed")) parsed = authority::native_codex_429::Codex429Classification{rt::Response(), rt::getBool(*input, "usageLimitReached").value_or(false), rt::getNumber(*input, "retryAtMs"), maybeText(*input, "quotaBlockSource"), rt::getBool(*input, "resetDeadlineIsStable").value_or(false), rt::getBool(*input, "resetDeadlineConflict").value_or(false)}; return codexAuthority.signal(capability, decode<contracts::NativeArguments::recordCodexAttemptSignal>(args), parsed ? &*parsed : nullptr, time); }
      if (op == "codex.fence") { const auto authorization = rt::get(action, "authorization") ? std::optional<contracts::NativeCodexAuthorization>(decode<contracts::NativeCodexAuthorization>(*rt::get(action, "authorization"))) : std::nullopt; codexAuthority.fence(capability, handle, time, authorization); return nullptr; }
      if (op == "codex.authorization") return json::value_from(codexAuthority.authorization(capability, handle, time));
      if (op == "codex.refresh") return codexAuthority.refresh(capability, handle, time);
      if (op == "codex.release") return codexAuthority.release(capability, handle, time);
      if (op == "codex.elect") return codexAuthority.elect(capability, decode<contracts::NativeArguments::electCodexResetProbe>(args), time);
      throw rt::JsError("unknown fixture action " + op);
    }
    rt::Task<json::value> run(json::value input) {
      const auto action = resolve(input); const auto op = text(action, "op"); const auto args = rt::getObject(action, "args") ? *rt::getObject(action, "args") : json::object(); const auto time = rt::getNumber(action, "now").value_or(now); json::value result;
      if (op == "control" || op == "control.concurrent") {
        const auto context = rt::get(action, "context") ? *rt::get(action, "context") : json::value(json::object()); const auto binding = rt::get(action, "binding") ? decode<principal::NativeControlBinding>(*rt::get(action, "binding")) : principal::NativeControlBinding{"admin", "admin-context", context};
        const principal::NativeControlCommand command{"e", text(action, "operationId", "control"), text(action, "method"), args};
        if (op == "control.concurrent") { rt::Promise<principal::NativeControlReply> left(principals->control(binding, command)), right(principals->control(binding, command)); const auto a = co_await left.wait(); const auto b = co_await right.wait(); result = json::array{json::value_from(a), json::value_from(b)}; }
        else { auto task = principals->control(binding, command); const auto value = co_await std::move(task); result = json::value_from(value); }
      } else if (op == "runtime.verifyRetained") {
        const auto context = rt::get(action, "context") ? *rt::get(action, "context") : json::value(json::object()); const auto binding = rt::get(action, "binding") ? decode<runtime::NativeRetainedHistoryBinding>(*rt::get(action, "binding")) : runtime::NativeRetainedHistoryBinding{"retained_history", "retained", "retained-context", context}; const auto intent = text(action, "intent", "intent"); auto task = runtimeAuthority.verifyRetained(binding, intent, time); const auto value = co_await std::move(task); result = json::value_from(value);
      } else if (op == "runtime.verify" || op == "runtime.routing") {
        const auto context = rt::get(action, "context") ? *rt::get(action, "context") : json::value(json::object()); const auto binding = rt::get(action, "binding") ? decode<runtime::NativeRuntimeBinding>(*rt::get(action, "binding")) : runtime::NativeRuntimeBinding{"runtime", "context", context}; const auto intent = text(action, "intent", "intent");
        if (op == "runtime.verify") { auto task = runtimeAuthority.verify(binding, intent, time); const auto value = co_await std::move(task); result = json::value_from(value); }
        else { const auto request = api.validated(cap, text(args, "handle"), time); auto task = runtimeAuthority.routing(binding, request, intent, time); const auto value = co_await std::move(task); result = json::value_from(value); }
      } else result = sql.transaction([&] { return sync(action); });
      co_return rt::jsonParse(rt::jsonStringify(result));
    }
  };
  inline json::object errorJson(rt::Reason reason) {
    try { std::rethrow_exception(reason); }
    catch (const contracts::NativeAuthorityError& error) { return {{"name", error.name}, {"message", error.message}, {"code", error.code}, {"status", rt::makeNumber(error.status)}}; }
    catch (const rt::sqlite::SQLiteError& error) { return {{"name", error.name}, {"message", error.message}, {"code", nullable(error.code)}, {"status", nullptr}}; }
    catch (const rt::JsError& error) { return {{"name", error.name}, {"message", error.message}, {"code", nullable(error.code)}, {"status", nullptr}}; }
    catch (const std::exception& error) { return {{"name", "Error"}, {"message", error.what()}, {"code", nullptr}, {"status", nullptr}}; }
  }
  inline std::string difference(const json::value& actual, const json::value& expected, const std::string& path = "$") {
    if (rt::deepEqual(actual, expected)) return "";
    if (actual.is_object() && expected.is_object()) {
      for (const auto& field : expected.as_object()) { const auto* value = rt::get(actual, field.key()); if (!value) return path + "." + std::string(field.key()) + " missing"; const auto mismatch = difference(*value, field.value(), path + "." + std::string(field.key())); if (!mismatch.empty()) return mismatch; }
      for (const auto& field : actual.as_object()) if (!rt::get(expected, field.key())) return path + "." + std::string(field.key()) + " unexpected=" + rt::jsonStringify(field.value());
    }
    if (actual.is_array() && expected.is_array() && actual.as_array().size() == expected.as_array().size()) for (std::size_t i = 0; i < actual.as_array().size(); ++i) { const auto mismatch = difference(actual.as_array()[i], expected.as_array()[i], path + "[" + std::to_string(i) + "]"); if (!mismatch.empty()) return mismatch; }
    return path + " actual=" + rt::jsonStringify(actual) + " expected=" + rt::jsonStringify(expected);
  }
  inline rt::Task<void> replay(json::value scenario) {
    Fixture fixture(scenario); std::size_t step = 0;
    for (const auto& expected : *rt::getArray(scenario, "steps")) {
      const auto action = *rt::get(expected, "action"); json::object actual;
      try { auto task = fixture.run(action); const auto result = co_await std::move(task); actual["result"] = result; if (const auto save = rt::getString(action, "save")) fixture.vars[*save] = result; }
      catch (...) { actual["error"] = errorJson(std::current_exception()); }
      const auto* expectedError = rt::get(expected, "error"); const auto* expectedResult = rt::get(expected, "result");
      if ((expectedError && (!rt::get(actual, "error") || !rt::deepEqual(*rt::get(actual, "error"), *expectedError))) || (expectedResult && (!rt::get(actual, "result") || !rt::deepEqual(*rt::get(actual, "result"), *expectedResult)))) {
        std::fprintf(stderr, "      step %zu %s: actual=%s expected=%s\n", step, text(action, "op").c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(expectedError ? *expectedError : *expectedResult).c_str());
        rt::test::report(__FILE__, __LINE__, "Bun reply/error trace differs", true);
      }
      const auto state = fixture.snapshot();
      if (!rt::deepEqual(state, *rt::get(expected, "state"))) {
        std::fprintf(stderr, "      step %zu %s state difference: %s\n", step, text(action, "op").c_str(), difference(state, *rt::get(expected, "state")).c_str());
        rt::test::report(__FILE__, __LINE__, "Bun complete SQL state/revisions/expiry/index/event trace differs", true);
      }
      ++step;
    }
    fixture.database.close();
  }
  inline bool registerScenarios(const std::string& group) {
    for (const auto& scenario : vectors().as_array()) if (rt::getString(scenario, "group") == group) {
      rt::test::registry().push_back({text(scenario, "name"), [scenario] { rt::FakeClock clock(NOW); rt::Loop loop(clock); rt::blockOn(loop, replay(scenario)); }});
    }
    return true;
  }
}
