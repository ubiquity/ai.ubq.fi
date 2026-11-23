#include "uos/paid_fallback/removed_provider.hpp"

#include <utility>

namespace uos::paid_fallback::removed_provider {
  namespace {
    std::optional<std::string> apiKeyForTest;
    std::optional<RemovedProviderTestAdapter> testAdapter;
  }

  std::optional<std::string> readRemovedProviderApiKey() { return apiKeyForTest; }

  void setRemovedProviderApiKeyForTest(std::optional<std::string> value) { apiKeyForTest = std::move(value); }

  void setRemovedProviderTestAdapterForTest(std::optional<RemovedProviderTestAdapter> value) { testAdapter = std::move(value); }

  rt::Promise<std::nullptr_t> deriveRemovedProviderSessionId() { return rt::Promise<std::nullptr_t>::resolve(nullptr); }

  rt::Promise<RemovedProviderResponsesResult> fetchRemovedProviderResponses(const json::object& body, const RemovedProviderResponsesOptions& options) {
    if (testAdapter) {
      const auto fetchResponses = testAdapter->fetchResponses;
      return fetchResponses(body, options);
    }
    return rt::Promise<RemovedProviderResponsesResult>::reject(rt::makeReason(rt::JsError("The removed provider is disabled.")));
  }

  bool isEligibleRemovedProviderModel(const std::string& model) {
    if (!testAdapter) return false;
    const auto isEligibleModel = testAdapter->isEligibleModel;
    return isEligibleModel(model);
  }

  std::optional<std::string> removedProviderModelFromEvent(const json::object& value) {
    if (!testAdapter) return std::nullopt;
    const auto modelFromEvent = testAdapter->modelFromEvent;
    return modelFromEvent(value);
  }

  std::optional<std::string> removedProviderTaskTypeFromResponse(const json::object&) { return std::nullopt; }

  const json::object& stripRemovedProviderMetadata(const json::object& value) { return value; }
}
