#include "uos/cache/scope_targets.hpp"
#include "uos/codex/auth.hpp"
#include "uos/kv.hpp"
#include "uos/utils.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace uos::cache::scope_targets {
namespace {
  namespace cm = models::codex_models;
  using rt::record::Reader;
  using rt::record::Writer;
  constexpr std::string_view FINGERPRINT_VERSION = "prompt-cache-scope-targets-v3";
  constexpr std::string_view AUTH_POOL_IDENTITY_FINGERPRINT_VERSION = "codex-auth-pool-identity-v1";
  struct CatalogModel { std::string id; std::optional<json::object> record; bool ambiguous; };
  struct Catalog { cm::CodexModelsSnapshot snapshot; std::vector<CatalogModel> models; };
  struct CodexAuthPoolBinding { double configuredSlotCount; bool usableTwoSlotBinding; std::optional<std::string> identityFingerprint; };
  std::optional<std::string> normalizedString(const json::value* value) {
    auto text = utils::getString(value);
    if (!text) return std::nullopt;
    auto normalized = rt::trim(*text);
    return normalized.empty() ? std::nullopt : std::optional(normalized);
  }
  std::optional<std::string> normalizedVersionstamp(const std::optional<std::string>& value) {
    if (!value) return std::nullopt;
    json::value raw(*value);
    return normalizedString(&raw);
  }
  std::optional<std::string> canonicalModelId(const json::object& value) {
    for (std::string_view key : {"slug", "id", "model", "name"}) if (auto id = normalizedString(rt::get(value, key))) return id;
    return std::nullopt;
  }
  /** JSON-like canonicalization keeps fingerprints independent of object key insertion order. */
  std::string canonicalJson(const json::value& value, std::size_t depth = 0) {
    if (depth > rt::kJsonMaxDepth) throw rt::RangeError("Maximum call stack size exceeded.");
    if (value.is_null() || value.is_string() || value.is_bool()) return rt::jsonStringify(value);
    if (rt::isNumber(value)) return rt::Number::isFinite(value) ? rt::jsonStringify(value) : rt::jsonQuote(rt::toString(value));
    std::vector<std::string> parts;
    if (value.is_array()) {
      for (const auto& entry : value.as_array()) parts.push_back(canonicalJson(entry, depth + 1));
      return "[" + rt::join(parts, ",") + "]";
    }
    auto keys = rt::objectKeys(value.as_object());
    rt::sort(keys);
    for (const auto& key : keys) parts.push_back(rt::jsonQuote(key) + ":" + canonicalJson(*rt::get(value, key), depth + 1));
    return "{" + rt::join(parts, ",") + "}";
  }
  MeteredFallbackRoster normalizeMeteredFallbackRoster(const std::optional<MeteredFallbackRoster>& value) {
    if (!value || !std::holds_alternative<MeteredFallbackRosterAuthoritative>(*value)) return MeteredFallbackRosterUnknown{};
    std::vector<std::string> ids;
    for (const auto& raw : std::get<MeteredFallbackRosterAuthoritative>(*value).model_ids) {
      auto id = rt::trim(raw);
      if (id.empty()) return MeteredFallbackRosterUnknown{};
      if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    rt::sort(ids);
    return MeteredFallbackRosterAuthoritative{std::move(ids)};
  }
  std::optional<Catalog> normalizeCatalog(const std::optional<cm::CodexModelsSnapshot>& value) {
    if (!value) return std::nullopt;
    const auto* raw = rt::getArray(*value, "models");
    const auto updated = rt::getNumber(*value, "updated_at_ms");
    if (!raw || !normalizedString(rt::get(*value, "source")) || !updated || !rt::Number::isSafeInteger(*updated) || *updated < 0) return std::nullopt;
    std::vector<CatalogModel> rows;
    for (const auto& model : *raw) {
      if (!model.is_object()) return std::nullopt;
      auto id = canonicalModelId(model.as_object());
      if (!id) return std::nullopt;
      auto existing = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == *id; });
      if (existing == rows.end()) rows.push_back({*id, model.as_object(), false});
      else { existing->record.reset(); existing->ambiguous = true; }
    }
    if (rows.empty()) return std::nullopt;
    rt::sort(rows, [](const auto& a, const auto& b) { return rt::compare(a.id, b.id); });
    return Catalog{*value, std::move(rows)};
  }
  rt::Task<CodexAuthPoolBinding> inspectCodexAuthPoolBinding(json::value value) {
    const auto* accounts = rt::getArray(value, "accounts");
    const auto size = accounts ? accounts->size() : 0;
    const double count = size == 1 || size == 2 ? static_cast<double>(size) : 0;
    const auto parsed = codex::auth::parseCodexAuthPool(value);
    std::optional<std::string> fingerprint;
    if (parsed) {
      json::array ids;
      for (const auto& account : parsed->accounts) ids.push_back(json::value(account.account_id));
      json::object material{{"v", AUTH_POOL_IDENTITY_FINGERPRINT_VERSION}, {"ordered_account_ids", std::move(ids)}};
      auto text = canonicalJson(material);
      auto hashing = utils::sha256Hex(std::move(text));
      auto hash = co_await std::move(hashing);
      fingerprint = "sha256:" + hash;
    }
    co_return CodexAuthPoolBinding{count, parsed && parsed->accounts.size() == 2, std::move(fingerprint)};
  }
  PromptCacheScopeTargetProbeability codexProbeability(const std::string& qualification, const std::optional<std::string>& version,
    const CodexAuthPoolBinding& pool, const std::optional<std::string>& poolVersion) {
    if (!version) return PromptCacheScopeTargetProbeabilityUnprobeable{"catalog_binding_unavailable"};
    if (qualification != "qualified") return PromptCacheScopeTargetProbeabilityUnprobeable{"codex_cache_unqualified"};
    if (pool.configuredSlotCount != 2) return PromptCacheScopeTargetProbeabilityUnprobeable{"two_codex_slots_required"};
    if (!pool.usableTwoSlotBinding || !poolVersion || !pool.identityFingerprint) return PromptCacheScopeTargetProbeabilityUnprobeable{"codex_auth_pool_binding_unavailable"};
    return PromptCacheScopeTargetProbeabilityProbeable{};
  }
  PromptCacheScopeTargetInventory unavailableInventory(const std::string& reason, const std::optional<MeteredFallbackRoster>& roster) {
    auto normalized = normalizeMeteredFallbackRoster(roster);
    const auto* authoritative = std::get_if<MeteredFallbackRosterAuthoritative>(&normalized);
    return {"unavailable", reason, {}, {authoritative ? "authoritative" : "unknown", authoritative ? authoritative->model_ids : std::vector<std::string>{}, {}}, std::nullopt, std::nullopt};
  }
  rt::Task<std::string> targetCapabilityFingerprint(std::string provider, std::string telemetryProvider, std::string topologyKind,
    CatalogModel model, std::string qualification, cm::CodexModelsSnapshot snapshot) {
    const auto capability = model.record ? cm::getCodexModelPromptCacheProvider(snapshot, model.id, cm::CODEX_CHATGPT_PROMPT_CACHE_PROVIDER) : std::nullopt;
    json::value capabilityInput = nullptr;
    if (capability) capabilityInput = json::object{{"id", capability->id}, {"controls", capability->controls ? cm::toJson(*capability->controls) : json::value(nullptr)}};
    json::object material{{"version", FINGERPRINT_VERSION}, {"provider", provider}, {"telemetry_provider", telemetryProvider}, {"topology_kind", topologyKind},
      {"model", model.id}, {"probe_profile", cm::PROMPT_CACHE_SCOPE_PROBE_PROFILE}, {"catalog_entry", model.ambiguous ? "ambiguous" : "unique"},
      {"codex_cache_qualification", qualification}, {"codex_prompt_cache_capability", capabilityInput}};
    auto text = canonicalJson(material);
    auto hashing = utils::sha256Hex(std::move(text));
    auto hash = co_await std::move(hashing);
    co_return "sha256:" + hash;
  }
  rt::Task<rt::KvEntryMaybe<>> readScopeEntry(std::shared_ptr<rt::Kv> store, rt::KvKey key) {
    const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
    auto task = store->get(key, strong);
    auto value = co_await std::move(task);
    co_return value;
  }
  /**
   * Catalog-owned Codex prompt-cache qualification for one canonical catalog
   * model. An ambiguous catalog entry is never qualified, whatever the published
   * capability says.
   */
  std::string codexCacheQualification(const cm::CodexModelsSnapshot& snapshot, const CatalogModel& model) {
    return !model.ambiguous && cm::isCodexModelPromptCacheScopeExperimentEligible(snapshot, model.id) ? "qualified" : "unqualified";
  }
}
std::string buildPromptCacheScopeTargetId(const PromptCacheScopeTargetProvider& provider, const PromptCacheScopeTargetTelemetryProvider& telemetryProvider,
  const PromptCacheScopeTargetTopologyKind& topologyKind, const std::string& model, const cm::PromptCacheScopeProbeProfile& probeProfile) {
  return std::string(FINGERPRINT_VERSION) + ":" + provider + ":" + telemetryProvider + ":" + topologyKind + ":" + probeProfile + ":" + rt::encodeURIComponent(model);
}
/**
 * Pure, read-only target derivation. Identity and capability fingerprints are
 * stable across refreshes; versionstamps stay separate so a future dispatcher
 * can fence every transition without silently retargeting a campaign.
 */
rt::Task<PromptCacheScopeTargetInventory> derivePromptCacheScopeTargetInventory(DerivePromptCacheScopeTargetInventoryInput input) {
  const auto roster = normalizeMeteredFallbackRoster(input.meteredFallbackRoster);
  const auto catalog = normalizeCatalog(input.snapshot);
  if (!catalog) co_return unavailableInventory(input.snapshot ? "catalog_invalid" : "catalog_unavailable", roster);
  const auto catalogVersion = normalizedVersionstamp(input.catalogVersionstamp);
  const auto clientVersion = normalizedString(rt::get(catalog->snapshot, "client_version"));
  const auto authVersion = normalizedVersionstamp(input.codexAuthPoolVersionstamp);
  auto poolTask = inspectCodexAuthPoolBinding(input.codexAuthPool);
  const auto pool = co_await std::move(poolTask);
  const auto* authoritative = std::get_if<MeteredFallbackRosterAuthoritative>(&roster);
  std::vector<std::string> meteredIds, nonCatalogIds;
  if (authoritative) for (const auto& id : authoritative->model_ids) {
    const bool present = std::any_of(catalog->models.begin(), catalog->models.end(), [&](const auto& row) { return row.id == id; });
    (present ? meteredIds : nonCatalogIds).push_back(id);
  }
  std::vector<PromptCacheScopeTarget> targets;
  for (int providerIndex = 0; providerIndex < 2; ++providerIndex) {
    const bool codexTarget = providerIndex == 0;
    for (const auto& model : catalog->models) {
      if (!codexTarget && std::find(meteredIds.begin(), meteredIds.end(), model.id) == meteredIds.end()) continue;
      /**
       * This is intentionally a catalog capability identity, not the terminal
       * telemetry transport identity. The latter is exposed on each target too.
       */
      const std::string provider(codexTarget ? PROMPT_CACHE_SCOPE_TARGET_CODEX_PROVIDER : PROMPT_CACHE_SCOPE_TARGET_METERED_PROVIDER);
      const std::string telemetry(codexTarget ? PROMPT_CACHE_SCOPE_TARGET_CODEX_TELEMETRY_PROVIDER : PROMPT_CACHE_SCOPE_TARGET_METERED_TELEMETRY_PROVIDER);
      const std::string topologyKind = codexTarget ? "codex_account_pool" : "single_credential";
      const auto qualification = codexCacheQualification(catalog->snapshot, model);
      auto fingerprintTask = targetCapabilityFingerprint(provider, telemetry, topologyKind, model, qualification, catalog->snapshot);
      const auto fingerprint = co_await std::move(fingerprintTask);
      PromptCacheScopeTargetTopology topology = PromptCacheScopeTargetTopologySingleCredential{};
      PromptCacheScopeTargetProbeability probeability = PromptCacheScopeTargetProbeabilityUnprobeable{"current_two_slot_adapter_does_not_apply"};
      if (codexTarget) { topology = PromptCacheScopeTargetTopologyCodexAccountPool{pool.configuredSlotCount, authVersion}; probeability = codexProbeability(qualification, catalogVersion, pool, authVersion); }
      targets.push_back({buildPromptCacheScopeTargetId(provider, telemetry, topologyKind, model.id), provider, telemetry, model.id, model.id, "exact_model",
        std::string(cm::PROMPT_CACHE_SCOPE_PROBE_PROFILE), true, qualification, std::move(topology), std::move(probeability), catalogVersion, clientVersion,
        codexTarget ? authVersion : std::nullopt, codexTarget ? pool.identityFingerprint : std::nullopt, fingerprint});
    }
  }
  rt::sort(targets, [](const auto& a, const auto& b) { return rt::compare(a.id, b.id); });
  const PromptCacheScopeTargetMeteredRosterDiagnostic diagnostic{authoritative ? "authoritative" : "unknown", authoritative ? authoritative->model_ids : std::vector<std::string>{}, nonCatalogIds};
  json::array inventoryTargets, bindingTargets;
  for (const auto& target : targets) {
    json::object relevant = json::value_from(target).as_object();
    for (std::string_view key : {"probeability", "catalog_versionstamp", "catalog_client_version", "codex_auth_pool_versionstamp", "codex_auth_pool_identity_fingerprint"}) rt::objectDelete(relevant, key);
    if (auto* topology = rt::getObject(relevant, "topology")) {
      json::object topologyCopy = *topology;
      rt::objectDelete(topologyCopy, "auth_pool_versionstamp"); relevant["topology"] = std::move(topologyCopy);
    }
    inventoryTargets.push_back(std::move(relevant));
    json::object binding{{"id", target.id}};
    rt::objectSet(binding, "catalog_versionstamp", rt::orNull(target.catalog_versionstamp));
    rt::objectSet(binding, "catalog_client_version", rt::orNull(target.catalog_client_version));
    rt::objectSet(binding, "codex_auth_pool_versionstamp", rt::orNull(target.codex_auth_pool_versionstamp));
    rt::objectSet(binding, "codex_auth_pool_identity_fingerprint", rt::orNull(target.codex_auth_pool_identity_fingerprint));
    bindingTargets.push_back(std::move(binding));
  }
  json::object inventoryMaterial{{"version", FINGERPRINT_VERSION}, {"probe_profile", cm::PROMPT_CACHE_SCOPE_PROBE_PROFILE},
    {"metered_fallback_roster", json::value_from(diagnostic)}, {"targets", std::move(inventoryTargets)}};
  json::object bindingMaterial{{"version", FINGERPRINT_VERSION}, {"targets", std::move(bindingTargets)}};
  auto inventoryText = canonicalJson(inventoryMaterial);
  auto inventoryHashing = utils::sha256Hex(std::move(inventoryText));
  const auto inventoryHash = co_await std::move(inventoryHashing);
  auto bindingText = canonicalJson(bindingMaterial);
  auto bindingHashing = utils::sha256Hex(std::move(bindingText));
  const auto bindingHash = co_await std::move(bindingHashing);
  co_return PromptCacheScopeTargetInventory{"ready", "ready", std::move(targets), diagnostic, "sha256:" + inventoryHash, "sha256:" + bindingHash};
}
/**
 * The loader is deliberately limited to two strong point reads. In particular,
 * it does not list tenant API-key policy records to invent a metered roster.
 */
rt::Task<PromptCacheScopeTargetInventory> loadPromptCacheScopeTargetInventory() { return loadPromptCacheScopeTargetInventory(LoadPromptCacheScopeTargetInventoryOptions{}); }
rt::Task<PromptCacheScopeTargetInventory> loadPromptCacheScopeTargetInventory(LoadPromptCacheScopeTargetInventoryOptions options) {
  std::shared_ptr<rt::Kv> store;
  if (options.kv) store = *options.kv;
  else {
    auto pending = kv::getKv();
    store = co_await pending.wait();
  }
  if (!store) co_return unavailableInventory("kv_unavailable", options.meteredFallbackRoster);
  rt::KvEntryMaybe<> catalogEntry, authEntry;
  bool failed = false;
  try {
    rt::Promise<rt::KvEntryMaybe<>> catalogRead(readScopeEntry(store, codex::auth::CODEX_MODELS_KV_KEY));
    rt::Promise<rt::KvEntryMaybe<>> authRead(readScopeEntry(store, codex::auth::CODEX_AUTH_POOL_KV_KEY));
    const auto entries = co_await rt::all(catalogRead, authRead).wait();
    catalogEntry = std::get<0>(entries); authEntry = std::get<1>(entries);
  } catch (...) { failed = true; }
  if (failed) co_return unavailableInventory("auth_pool_unavailable", options.meteredFallbackRoster);
  auto catalog = rt::storedJsonView(catalogEntry.value);
  if (!rt::truthy(catalog)) co_return unavailableInventory("catalog_unavailable", options.meteredFallbackRoster);
  if (!catalog.is_object()) co_return unavailableInventory("catalog_invalid", options.meteredFallbackRoster);
  DerivePromptCacheScopeTargetInventoryInput input{catalog.as_object(), catalogEntry.versionstamp, rt::storedJsonView(authEntry.value), authEntry.versionstamp, options.meteredFallbackRoster};
  auto task = derivePromptCacheScopeTargetInventory(input);
  auto result = co_await std::move(task);
  co_return result;
}
void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredFallbackRoster& value) {
  if (const auto* row = std::get_if<MeteredFallbackRosterAuthoritative>(&value)) out = Writer().required("status", std::string(row->status)).required("model_ids", row->model_ids).done();
  else out = json::object{{"status", "unknown"}};
}
MeteredFallbackRoster tag_invoke(const json::value_to_tag<MeteredFallbackRoster>&, const json::value& value) {
  /**
   * metered model policy is tenant-key-specific. The inventory must only consume
   * an explicitly supplied, authoritative roster; it never discovers one.
   */
  const Reader in(value, "MeteredFallbackRoster");
  if (in.required<std::string>("status") == "authoritative") return MeteredFallbackRosterAuthoritative{in.required<std::vector<std::string>>("model_ids")};
  if (in.required<std::string>("status") == "unknown") return MeteredFallbackRosterUnknown{};
  in.fail("status", rt::get(value, "status"), "unknown or authoritative");
}
void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheScopeTargetTopology& value) {
  if (const auto* row = std::get_if<PromptCacheScopeTargetTopologyCodexAccountPool>(&value)) out = Writer().required("kind", std::string(row->kind)).required("configured_slot_count", row->configured_slot_count).orNull("auth_pool_versionstamp", row->auth_pool_versionstamp).done();
  else out = json::object{{"kind", "single_credential"}};
}
PromptCacheScopeTargetTopology tag_invoke(const json::value_to_tag<PromptCacheScopeTargetTopology>&, const json::value& value) {
  const Reader in(value, "PromptCacheScopeTargetTopology");
  if (in.required<std::string>("kind") == "codex_account_pool") return PromptCacheScopeTargetTopologyCodexAccountPool{in.required<double>("configured_slot_count"), in.orNull<std::string>("auth_pool_versionstamp")};
  if (in.required<std::string>("kind") == "single_credential") return PromptCacheScopeTargetTopologySingleCredential{};
  in.fail("kind", rt::get(value, "kind"), "codex_account_pool or single_credential");
}
void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheScopeTargetProbeability& value) {
  if (const auto* row = std::get_if<PromptCacheScopeTargetProbeabilityUnprobeable>(&value)) out = Writer().required("status", std::string(row->status)).required("reason", row->reason).done();
  else out = Writer().required("status", std::string("probeable")).required("adapter", std::get<PromptCacheScopeTargetProbeabilityProbeable>(value).adapter).done();
}
PromptCacheScopeTargetProbeability tag_invoke(const json::value_to_tag<PromptCacheScopeTargetProbeability>&, const json::value& value) {
  const Reader in(value, "PromptCacheScopeTargetProbeability");
  if (in.required<std::string>("status") == "unprobeable") return PromptCacheScopeTargetProbeabilityUnprobeable{in.required<std::string>("reason")};
  if (in.required<std::string>("status") == "probeable") return PromptCacheScopeTargetProbeabilityProbeable{in.required<std::string>("adapter")};
  in.fail("status", rt::get(value, "status"), "probeable or unprobeable");
}
void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheScopeTarget& value) {
  out = Writer().required("id", value.id).required("provider", value.provider).required("telemetry_provider", value.telemetry_provider).required("model", value.model).required("model_family_id", value.model_family_id).required("model_family_source", value.model_family_source).required("probe_profile", value.probe_profile).required("catalog_model_present", value.catalog_model_present).required("codex_cache_qualification", value.codex_cache_qualification).required("topology", value.topology).required("probeability", value.probeability).orNull("catalog_versionstamp", value.catalog_versionstamp).orNull("catalog_client_version", value.catalog_client_version).orNull("codex_auth_pool_versionstamp", value.codex_auth_pool_versionstamp).orNull("codex_auth_pool_identity_fingerprint", value.codex_auth_pool_identity_fingerprint).required("capability_fingerprint", value.capability_fingerprint).done();
}
PromptCacheScopeTarget tag_invoke(const json::value_to_tag<PromptCacheScopeTarget>&, const json::value& value) {
  const Reader in(value, "PromptCacheScopeTarget");
  return {in.required<std::string>("id"), in.required<std::string>("provider"), in.required<std::string>("telemetry_provider"), in.required<std::string>("model"), in.required<std::string>("model_family_id"), in.required<std::string>("model_family_source"), in.required<std::string>("probe_profile"), in.required<bool>("catalog_model_present"), in.required<std::string>("codex_cache_qualification"), in.required<PromptCacheScopeTargetTopology>("topology"), in.required<PromptCacheScopeTargetProbeability>("probeability"), in.orNull<std::string>("catalog_versionstamp"), in.orNull<std::string>("catalog_client_version"), in.orNull<std::string>("codex_auth_pool_versionstamp"), in.orNull<std::string>("codex_auth_pool_identity_fingerprint"), in.required<std::string>("capability_fingerprint")};
}
void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheScopeTargetMeteredRosterDiagnostic& value) {
  out = Writer().required("status", value.status).required("model_ids", value.model_ids).required("non_catalog_model_ids", value.non_catalog_model_ids).done();
}
PromptCacheScopeTargetMeteredRosterDiagnostic tag_invoke(const json::value_to_tag<PromptCacheScopeTargetMeteredRosterDiagnostic>&, const json::value& value) {
  const Reader in(value, "PromptCacheScopeTargetMeteredRosterDiagnostic");
  return {in.required<std::string>("status"), in.required<std::vector<std::string>>("model_ids"), in.required<std::vector<std::string>>("non_catalog_model_ids")};
}
void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheScopeTargetInventory& value) {
  out = Writer().required("status", value.status).required("reason", value.reason).required("targets", value.targets).required("metered_fallback_roster", value.metered_fallback_roster).orNull("inventory_fingerprint", value.inventory_fingerprint).orNull("binding_fingerprint", value.binding_fingerprint).done();
}
PromptCacheScopeTargetInventory tag_invoke(const json::value_to_tag<PromptCacheScopeTargetInventory>&, const json::value& value) {
  const Reader in(value, "PromptCacheScopeTargetInventory");
  return {in.required<std::string>("status"), in.required<std::string>("reason"), in.required<std::vector<PromptCacheScopeTarget>>("targets"), in.required<PromptCacheScopeTargetMeteredRosterDiagnostic>("metered_fallback_roster"), in.orNull<std::string>("inventory_fingerprint"), in.orNull<std::string>("binding_fingerprint")};
}
}
