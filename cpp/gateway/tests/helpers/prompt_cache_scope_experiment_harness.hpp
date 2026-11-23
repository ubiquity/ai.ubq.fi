#pragma once
// Port of tests/helpers/prompt-cache-scope-experiment-harness.ts.
// Reuses real runner/promotion fixtures and the admin endpoint exports.
#include "tests/helpers/codex_diff_kv.hpp"
#include "uos/admin/codex.hpp"
#include "uos/cache/scope_experiment.hpp"
#include "uos/cache/scope_targets.hpp"
#include "uos/catalog/promotion.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/provider/health.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include <array>
#include <functional>
#include <memory>

namespace uos::tests::helpers::prompt_cache_scope_experiment_harness {
  inline constexpr std::array<double, 10> CACHE_READ_CYCLE{0, 2560, 0, 2560, 2560, 2560, 2560, 2560, 2560, 2560};
  inline constexpr std::array<double, 10> CACHE_WRITE_CYCLE_LEADING{2560, 2560, 2560, 0, 0, 0, 0, 0, 0, 0};
  inline constexpr std::array<double, 10> CACHE_WRITE_CYCLE_ALTERNATING{2560, 0, 2560, 0, 0, 0, 0, 0, 0, 0};
  double compareStrings(const std::string& left, const std::string& right);
  double cycleValue(std::span<const double> cycle, double step);
  std::string fetchInputUrl(const rt::RequestInfo& input);
  std::string requestBodyText(const rt::RequestInit& init = {});
  std::string encodeKey(const rt::KvKey& key);
  struct AtomicWrite { std::string type; rt::KvKey key; std::optional<rt::KvValue> value{}; };
  class ExperimentKv : public codex_diff_kv::DiffKv {
  public:
    double atomicCalls = 0;
    std::function<rt::Task<void>(rt::KvKey)> beforeGet;
    std::function<void(const std::vector<AtomicWrite>&)> afterAtomicCommit;
    std::function<void(const std::vector<AtomicWrite>&)> beforeAtomicCommit;
    std::function<bool(const std::vector<AtomicWrite>&)> refuseAtomicCommit;
    void clear();
    void put(const rt::KvKey& key, const rt::KvValue& value);
    using rt::Kv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override;
    std::unique_ptr<rt::AtomicOperation> atomic() override;
  };
  extern std::shared_ptr<ExperimentKv> kv;
  inline constexpr std::string_view MODEL = "gpt-5.6-cache-scope-fixture";
  inline constexpr std::string_view TELEMETRY_RELEASE = "0123456789abcdef0123456789abcdef01234567";
  std::vector<std::string> targetKeyParts(const std::string& model);
  rt::KvKey evidenceKeyFor(const std::string& model);
  rt::KvKey stateKeyFor(const std::string& model);
  inline const rt::KvKey evidenceKey = evidenceKeyFor(std::string(MODEL));
  inline const rt::KvKey stateKey = stateKeyFor(std::string(MODEL));
  rt::KvKey leaseKey(const std::string& model);
  inline const rt::KvKey campaignLeaseKey{"uos_ai", "prompt_cache_scope_experiment", "v5", "campaign", "codex_chatgpt", "chatgpt_codex",
    "responses_implicit_input_text_keyed_cycle_isolated_v5"};
  json::object makeAuth(const std::string& label);
  rt::Task<std::string> authPoolVersionstamp();
  rt::Task<std::string> catalogVersionstamp();
  rt::Task<std::string> runtimeVersionstamp();
  struct PromotionBinding { std::string catalogVersionstamp; std::string runtimeVersionstamp; };
  rt::Task<PromotionBinding> promotionBinding();
  const json::object& cacheControls();
  json::object modelRecord(const std::string& model);
  json::object modelRecord(const std::string& model, const json::object& controls);
  struct SeedOptions { std::optional<std::vector<std::string>> models{}; std::optional<std::string> defaultModel{}; };
  void seed();
  void seed(const SeedOptions& options);
  rt::Task<void> seedStage0Baseline(std::string model);
  rt::Task<cache::scope_experiment_model::PromptCacheScopeExperimentTelemetryBaseline> scopeBaselineFor(std::string model = std::string(MODEL));
  rt::Task<cache::scope_experiment_model::PromptCacheScopeExperimentResult> runExperiment(std::string model = std::string(MODEL));
  rt::Response sseCompleted(double cachedTokens, double cacheWriteTokens, const std::string& model = std::string(MODEL));
  rt::Task<provider::health::ProviderHealthView> waitForCodexHealth(std::string accountId);
  struct CapturedScopeRequest { std::optional<std::string> account; std::optional<std::string> conversation; std::string body; };
  struct ScopeTransport {
    std::vector<CapturedScopeRequest> requests;
    std::size_t refreshes = 0;
    std::function<rt::Response(std::size_t, const rt::RequestInfo&, const rt::RequestInit&)> onInference;
    std::function<rt::Response(std::size_t)> onRefresh;
    rt::Fetch fetcher();
  };
  void installHarness();
  void resetHarness();
  void cleanupHarness();
  struct HarnessScope {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w06c3-scope"}}};
    std::string saved = config::config.codexAuthJsonB64;
    HarnessScope();
    ~HarnessScope();
  };
  using codex::auth::CODEX_AUTH_POOL_KV_KEY;
  using codex::auth::CODEX_MODELS_KV_KEY;
  using codex::auth::resetCodexAuthCacheForTest;
  using runtime_config::RUNTIME_CONFIG_V2_KEY;
  using runtime_config::resetRuntimeConfigCacheForTest;
  using cache::scope_experiment::PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX;
  using cache::scope_experiment::PromptCacheScopeExperimentBusyError;
  using cache::scope_experiment::PromptCacheScopeExperimentUnavailableError;
  using cache::scope_experiment::readPromptCacheScopeExperimentCompletedUsage;
  using cache::scope_experiment::readPromptCacheScopeExperimentTelemetryBaseline;
  using cache::scope_experiment::assertPromptCacheScopeExperimentTelemetryBaseline;
  using cache::scope_experiment::runPromptCacheScopeExperiment;
  using cache::scope_targets::loadPromptCacheScopeTargetInventory;
  using cache::telemetry_gate::resolvePromptCacheTelemetryCounterKeys;
  using catalog::promotion::promoteCodexPromptCacheScope;
  using provider::health::getCodexProviderHealth;
  using provider::health::resetProviderHealthThrottleForTest;
  using models::codex_models_fetch::storeCodexModelsSnapshot;
  using ::uos::admin::codex::handleAdminCodexCacheScopeExperiment;
  using ::uos::admin::codex::handleAdminCodexCacheScopeExperimentTelemetryBaseline;
}
