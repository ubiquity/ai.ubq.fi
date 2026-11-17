#pragma once
// Port of src/admin/codex.ts.
#include "uos/cache/prompt_analytics_core.hpp"
#include "uos/cache/telemetry_gate.hpp"
#include "uos/models/catalog.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/lithos_models.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include <functional>
#include <optional>
namespace uos::admin::codex {
  inline const rt::KvKey UOS_KERNEL_PUBKEYS_KEY{"uos_ai", "kernel_pubkeys"};
  inline constexpr double MAX_KV_MIGRATION_BODY_BYTES = 5 * 1024 * 1024;
  std::optional<rt::Response> runtimeConfigErrorResponse(rt::Reason error);
  rt::Task<rt::Response> handleAdminDebugRouting(rt::Request req);
  rt::Task<rt::Response> handleAdminCodexRecheck(double slot);
  rt::Task<rt::Response> handleAdminCodexBankedResetShadowDecisions();
  rt::Task<rt::Response> handleAdminCodexCacheScopeExperiment(rt::Request req);
  using ReadBaseline = std::function<rt::Task<cache::telemetry_gate::PromptCacheTelemetryBaselineResult>()>;
  rt::Task<rt::Response> handleAdminCodexCacheScopeExperimentTelemetryBaseline(ReadBaseline readBaseline = {});
  using ReadBaselineJson = std::function<rt::Task<json::value>()>;
  rt::Task<rt::Response> handleAdminCodexCacheScopeExperimentTelemetryBaseline(ReadBaselineJson readBaseline);
  using ReadAnalytics = std::function<rt::Task<cache::prompt_analytics_core::PromptCacheAnalyticsView>(cache::prompt_analytics_core::PromptCacheAnalyticsReadOptions)>;
  rt::Task<rt::Response> handleAdminPromptCacheAnalytics(rt::Request req, ReadAnalytics readAnalytics = {});
  rt::Task<rt::Response> handleAdminCodexAuth(rt::Request req);
  rt::Task<rt::Response> handleAdminCodexModelsGet();
  rt::Task<rt::Response> handleAdminCodexModelsSet(rt::Request req);
  rt::Task<rt::Response> handleAdminCodexPromptsPurge();
  rt::Task<rt::Response> handleAdminCodexModelsWhitelistGet();
  struct CatalogDependencies { std::function<rt::Task<models::catalog::ModelCatalogSnapshot>()> buildCatalog{}; };
  rt::Task<rt::Response> handleAdminModelsCatalogGet();
  rt::Task<rt::Response> handleAdminModelsCatalogGet(CatalogDependencies dependencies);
  struct ModelsRefreshDependencies {
    std::function<rt::Task<std::shared_ptr<const models::openrouter_models::OpenRouterModelsSnapshot>>()> refreshEnrichment{};
    std::function<rt::Task<std::optional<provider::metered::MeteredModelsSnapshot>>(provider::metered::FetchMeteredModelsOptions)> refreshOpenlux{};
    std::function<rt::Task<std::optional<provider::surplus::SurplusModelsSnapshot>>(provider::surplus::FetchSurplusModelsOptions)> refreshSurplus{};
    std::function<rt::Task<std::optional<provider::lithos_models::LithosModelsSnapshot>>(provider::lithos_models::FetchLithosModelsOptions)> refreshLithos{};
    std::function<rt::Task<std::optional<provider::claude_code_models::ClaudeCodeModelsSnapshot>>(provider::claude_code_models::CatalogOptions)> refreshClaude{};
  };
  rt::Task<rt::Response> handleAdminModelsRefresh();
  rt::Task<rt::Response> handleAdminModelsRefresh(ModelsRefreshDependencies dependencies);
  rt::Task<rt::Response> handleAdminCodexModelsWhitelistSet(rt::Request req);
  rt::Task<rt::Response> handleAdminProviderSelectionGet();
  rt::Task<rt::Response> handleAdminProviderSelectionGet(CatalogDependencies dependencies);
  rt::Task<rt::Response> handleAdminProviderSelectionSet(rt::Request req);
}
