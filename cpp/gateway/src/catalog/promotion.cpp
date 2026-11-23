#include "uos/catalog/promotion.hpp"
#include "uos/catalog/types.hpp"
#include "uos/codex/auth.hpp"
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/runtime_config.hpp"
#include "uos/utils.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include <vector>
namespace uos::catalog::promotion {
  namespace {
    namespace models = ::uos::models::codex_models;
    namespace runtime = ::uos::runtime_config;
    namespace contracts = ::uos::authority::native_codex_contracts;
    struct PromptCacheScopePromotionReady { models::CodexModelsSnapshot nextSnapshot; runtime::RuntimeConfigV2 nextRuntime; };
    using Plan = std::variant<PromptCacheScopePromotionReady, PromptCacheScopePromotionResult>;
    struct PromptCacheScopePromotionEntries { rt::KvEntryMaybe<> snapshot; rt::KvEntryMaybe<> runtime; rt::KvEntryMaybe<> lease; rt::KvEntryMaybe<> authPool; };
    bool ownsPromptCacheScopePromotionLease(const json::value& value, const std::string& owner) {
      const auto until = rt::getNumber(value, "lease_until_ms");
      return utils::isRecord(value) && rt::getString(value, "owner") == owner && until && rt::Number::isFinite(*until) && *until > rt::dateNow();
    }
    PromptCacheScopePromotionResult inconclusive(std::string reason) { return {"inconclusive", std::move(reason)}; }
    Plan planPromptCacheScopePromotion(const PromptCacheScopePromotionInput& input, const std::string& model, const PromptCacheScopePromotionEntries& entries) {
      if (!ownsPromptCacheScopePromotionLease(rt::storedJsonView(entries.lease.value), input.lease.owner)) return inconclusive("lease_lost");
      if (entries.authPool.versionstamp != input.authPoolVersionstamp) return inconclusive("auth_pool_drift");
      const auto snapshotValue = rt::storedJsonView(entries.snapshot.value);
      const auto source = rt::getString(snapshotValue, "source"); const auto updated = rt::getNumber(snapshotValue, "updated_at_ms");
      const auto* list = rt::get(snapshotValue, "models");
      if (!entries.snapshot.value.truthy() || !snapshotValue.is_object() || !list || !list->is_array() || !source || rt::trim(*source).empty() || !updated || !rt::Number::isSafeInteger(*updated) || *updated <= 0) return inconclusive("snapshot_unavailable");
      const auto& snapshot = snapshotValue.as_object();
      if (entries.snapshot.versionstamp != input.catalogVersionstamp) return inconclusive("catalog_drift");
      if (!models::getUniqueCodexModelBySlug(snapshot, model)) return inconclusive("model_drift");
      if (!models::isCodexModelPromptCacheScopeExperimentEligible(snapshot, model)) return inconclusive("capability_changed");
      const auto currentRuntime = runtime::normalizeRuntimeConfig(entries.runtime.value);
      if (!currentRuntime) return inconclusive("runtime_unavailable");
      if (entries.runtime.versionstamp != input.runtimeVersionstamp) return inconclusive("runtime_drift");
      const auto defaultModel = ::uos::models::codex_models_fetch::preserveCodexDefaultModel(snapshot, currentRuntime->default_model);
      if (!defaultModel) return inconclusive("runtime_unavailable");
      const auto nextSnapshot = models::withCodexModelPromptCacheScope(snapshot, model, models::CODEX_CHATGPT_PROMPT_CACHE_PROVIDER, input.scope);
      if (!nextSnapshot) return inconclusive("model_drift");
      try {
        const runtime::BuildRuntimeConfigOptions options{.defaultModel = defaultModel, .defaultReasoningEffort = currentRuntime->default_reasoning_effort, .nowMs = input.scope.verified_at_ms};
        const auto nextRuntime = runtime::buildRuntimeConfig(*nextSnapshot, options);
        return PromptCacheScopePromotionReady{*nextSnapshot, nextRuntime};
      } catch (...) { return inconclusive("runtime_unavailable"); }
    }
  }
  rt::Task<PromptCacheScopePromotionResult> promoteCodexPromptCacheScope(std::shared_ptr<rt::Kv> kv, PromptCacheScopePromotionInput input) {
    const std::string model = rt::trim(input.model);
    if (model.empty() || rt::trim(input.authPoolVersionstamp).empty() || rt::trim(input.catalogVersionstamp).empty() || rt::trim(input.runtimeVersionstamp).empty() || !input.scope.effective_model || rt::trim(*input.scope.effective_model) != model || !models::isConcretePromptCacheScope(input.scope, 3)) co_return inconclusive("invalid_scope");
    for (int attempt = 0; attempt < 3; ++attempt) {
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
      reads.emplace_back(kv->get(codex::auth::CODEX_MODELS_KV_KEY, {.consistency = rt::KvConsistencyLevel::strong})); reads.emplace_back(kv->get(runtime::RUNTIME_CONFIG_V2_KEY, {.consistency = rt::KvConsistencyLevel::strong})); reads.emplace_back(kv->get(input.lease.key, {.consistency = rt::KvConsistencyLevel::strong})); reads.emplace_back(kv->get(contracts::CODEX_AUTH_POOL_KV_KEY, {.consistency = rt::KvConsistencyLevel::strong}));
      const auto values = co_await rt::all(std::move(reads)).wait();
      const PromptCacheScopePromotionEntries entries{values[0], values[1], values[2], values[3]};
      const auto plan = planPromptCacheScopePromotion(input, model, entries);
      if (const auto* failed = std::get_if<PromptCacheScopePromotionResult>(&plan)) co_return *failed;
      const auto& ready = std::get<PromptCacheScopePromotionReady>(plan);
      const types::RefreshLease renewedLease{input.lease.owner, rt::dateNow() + types::PROMPT_CACHE_SCOPE_PROMOTION_LEASE_MS};
      const rt::KvValue snapshotValue(ready.nextSnapshot); const rt::KvValue runtimeValue(json::value_from(ready.nextRuntime)); const rt::KvValue leaseValue(json::value_from(renewedLease));
      auto operation = kv->atomic(); operation->check(entries.snapshot).check(entries.runtime).check(entries.lease).check(entries.authPool).set(codex::auth::CODEX_MODELS_KV_KEY, snapshotValue).set(runtime::RUNTIME_CONFIG_V2_KEY, runtimeValue).set(input.lease.key, leaseValue, {.expireIn = types::PROMPT_CACHE_SCOPE_PROMOTION_LEASE_MS * 2});
      const auto commit = co_await operation->commit();
      if (!commit.ok) continue;
      runtime::cacheRuntimeConfig(ready.nextRuntime);
      co_return PromptCacheScopePromotionResult{"promoted", std::nullopt};
    }
    co_return inconclusive("cas_conflict");
  }
}
