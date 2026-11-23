#pragma once
// Port of src/paid-fallback/health.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uos::paid_fallback::health {
  void warnPaidFallbackBookkeepingFailure(const std::string& operation, const rt::Reason& error);
  void logPaidProviderSelected(const std::string& requestId, const openai_telemetry::InferenceFallbackReason& reason, const std::string& provider);
  bool canAttemptPaidFallback(const openai_telemetry::UsageContext* context, const std::optional<provider::selection::ProviderSelection>& selection);
  rt::Task<void> bestEffortPaidFallbackBookkeeping(std::string operation, std::function<rt::Task<void>()> run);
  std::optional<double> paidProviderErrorStatus(const rt::Reason& error);
  std::optional<openai_telemetry::PaidProviderHealthClassification> paidProviderHealthClassification(const std::string& provider,
    std::optional<double> status, bool meteredQuotaExhausted, bool surplusQuotaExhausted = false);
  rt::Task<bool> meteredForbiddenIndicatesQuotaExhaustion(rt::Response response, std::optional<rt::AbortSignal> signal);
  rt::Task<bool> surplus402IndicatesQuotaExhaustion(rt::Response response, std::optional<rt::AbortSignal> signal);
  rt::Task<void> recordPaidProviderResponseHealth(std::string provider, std::optional<double> status,
    std::optional<std::string> providerRequestId = std::nullopt, bool meteredQuotaExhausted = false, bool surplusQuotaExhausted = false);
  openai_telemetry::MeteredTransportLifecycle createMeteredTransportLifecycle(std::optional<index::PaidFallbackReservation> reservation,
    openai_telemetry::UpstreamProvider provider = "metered", std::optional<std::string> providerRequestId = std::nullopt,
    std::optional<index::SurplusBillingPricing> surplusBilling = std::nullopt, std::optional<std::string> model = std::nullopt,
    bool providerHealthOnly = false, std::optional<openai_telemetry::PaidProviderHealthClassification> deliveredHealth = std::nullopt);
  bool paidCatalogNeedsRefresh(const std::optional<provider::metered::MeteredModelsSnapshot>& catalog, double ttlMs);
  bool paidCatalogNeedsRefresh(const std::optional<provider::surplus::SurplusModelsSnapshot>& catalog, double ttlMs);
  using PaidCatalogs = std::pair<std::optional<provider::metered::MeteredModelsSnapshot>, std::optional<provider::surplus::SurplusModelsSnapshot>>;
  rt::Task<PaidCatalogs> refreshPaidCatalogs(std::optional<provider::metered::MeteredModelsSnapshot> meteredCatalog,
    std::optional<provider::surplus::SurplusModelsSnapshot> surplusCatalog, std::optional<rt::AbortSignal> signal = std::nullopt);
  void refreshStalePaidCatalogsInBackground(const std::optional<provider::metered::MeteredModelsSnapshot>& meteredCatalog,
    const std::optional<provider::surplus::SurplusModelsSnapshot>& surplusCatalog);
  struct PaidRoutingStateInput {
    std::optional<provider::metered::MeteredModelsSnapshot> meteredCatalog;
    std::optional<provider::surplus::SurplusModelsSnapshot> surplusCatalog;
    bool codexModelKnown;
    std::string endpointType;
    bool requestUsesTools;
    std::string model;
    std::optional<provider::selection::ProviderSelection> selection;
    std::optional<std::vector<std::string>> allowedPaidProviders{};
    std::optional<std::string> deepSeekWaterfallPaidHop{};
  };
  struct PaidRoutingState {
    std::optional<index::SurplusBillingPricing> surplusBilling;
    std::vector<std::string> paidProviders;
    bool paidModelKnown;
    bool meteredOnly;
    bool codexEnabled;
    friend bool operator==(const PaidRoutingState&, const PaidRoutingState&) = default;
  };
  PaidRoutingState resolvePaidRoutingState(const PaidRoutingStateInput& input);
  bool isIntermediatePaidProviderAttempt(double providerIndex, double providerCount, std::optional<double> status);
  rt::Reason paidFallbackAbortReason(const std::optional<rt::AbortSignal>& fallbackSignal);
  struct TemporaryFreeSurplusRoutedResponsesOptions {
    std::string model;
    const openai_telemetry::UsageContext* usageContext = nullptr;
    std::optional<rt::AbortSignal> signal{};
  };
  rt::Task<openai_telemetry::RoutedResponsesUpstream> fetchTemporaryFreeSurplusRoutedResponses(json::object body,
    TemporaryFreeSurplusRoutedResponsesOptions options);
  struct LoadPaidResponsesCatalogsOptions {
    std::string model;
    std::string route;
    std::optional<rt::AbortSignal> signal{};
    std::optional<std::vector<std::string>> allowedPaidProviders{};
    std::optional<std::string> deepSeekWaterfallPaidHop{};
  };
  struct PaidResponsesCatalogs {
    bool codexModelKnown;
    std::string endpointType;
    bool requestUsesTools;
    std::optional<provider::metered::MeteredModelsSnapshot> meteredCatalog;
    std::optional<provider::surplus::SurplusModelsSnapshot> surplusCatalog;
    std::optional<provider::selection::ProviderSelection> selection;
    PaidRoutingState routing;
  };
  rt::Task<PaidResponsesCatalogs> loadPaidResponsesCatalogs(json::object body, LoadPaidResponsesCatalogsOptions options,
    std::optional<provider::selection::ProviderSelection> selection);
  openai_telemetry::RoutedResponsesUpstream rejectPaidAdmission(const std::string& provider, const std::string& errorReason,
    const std::shared_ptr<openai_telemetry::ResponseTelemetryState>& telemetry, const openai_telemetry::UsageContext* usageContext,
    const std::string& model);
  openai_telemetry::RoutedResponsesUpstream rejectPaidAdmission(const std::string& provider, const std::string& errorReason,
    const std::shared_ptr<openai_telemetry::ResponseTelemetryState>& telemetry, const openai_telemetry::UsageContext* usageContext,
    const std::string& model, const std::string& logReason);
  openai_telemetry::RoutedResponsesUpstream rejectDisabledCodexProvider(const std::string& model,
    const std::shared_ptr<openai_telemetry::ResponseTelemetryState>& telemetry, const openai_telemetry::UsageContext* usageContext);
  std::optional<openai_telemetry::RoutedResponsesUpstream> rejectUnroutablePaidModel(bool codexModelKnown, const PaidRoutingState& routing,
    const std::optional<provider::surplus::SurplusModelsSnapshot>& surplusCatalog, const std::string& endpointType, bool requestUsesTools,
    const openai_telemetry::UsageContext* usageContext, const std::string& model,
    const std::shared_ptr<openai_telemetry::ResponseTelemetryState>& telemetry);
  std::optional<double> forcedDebugCodexStatus(const std::string& debugScenario);
  std::optional<std::string> codexCacheScopeForUsageContext(const openai_telemetry::UsageContext* usageContext);
}
