#pragma once
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace uos::paid_fallback::removed_provider {
  // An empty std::function is an absent (undefined) callback.
  struct RemovedProviderResponseTiming {
    std::function<void()> onDispatch;
    std::function<void()> onHeaders;
  };

  // `beforeDispatch?: () => Promise<unknown>`: the settled value is never read.
  struct RemovedProviderResponsesOptions {
    std::string apiKey;
    std::optional<std::string> sessionId;
    rt::AbortSignal signal;
    std::optional<RemovedProviderResponseTiming> timing;
    std::function<rt::Promise<void>()> beforeDispatch;
  };

  // `{ response: Response }`
  struct RemovedProviderResponsesResult {
    rt::Response response;
  };

  /** Test-only adapter for exercising the disabled fallback boundary. */
  struct RemovedProviderTestAdapter {
    std::function<rt::Promise<RemovedProviderResponsesResult>(json::object body, RemovedProviderResponsesOptions options)> fetchResponses;
    std::function<std::optional<std::string>(const json::object& value)> modelFromEvent;
    std::function<bool(const std::string& model)> isEligibleModel;
  };

  // `string | null | undefined` setters: both null and undefined read back as null.
  std::optional<std::string> readRemovedProviderApiKey();
  void setRemovedProviderApiKeyForTest(std::optional<std::string> value);
  void setRemovedProviderTestAdapterForTest(std::optional<RemovedProviderTestAdapter> value);

  // `(..._args: unknown[]): Promise<null>`
  rt::Promise<std::nullptr_t> deriveRemovedProviderSessionId();
  template <class... Args>
  rt::Promise<std::nullptr_t> deriveRemovedProviderSessionId(const Args&...) {
    return deriveRemovedProviderSessionId();
  }

  rt::Promise<RemovedProviderResponsesResult> fetchRemovedProviderResponses(const json::object& body, const RemovedProviderResponsesOptions& options);

  bool isEligibleRemovedProviderModel(const std::string& model);
  std::optional<std::string> removedProviderModelFromEvent(const json::object& value);
  std::optional<std::string> removedProviderTaskTypeFromResponse(const json::object& value);
  // Returns its argument (the same object, so `stripped === value` holds).
  const json::object& stripRemovedProviderMetadata(const json::object& value);
  const json::object& stripRemovedProviderMetadata(json::object&& value) = delete;
}
