#pragma once
// Port of src/images.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace uos::images {
  void setImageBaseModelForTest(std::optional<std::string> model);
  using ImageRouteKind = std::string;
  struct ImageHandlerOptions {
    std::function<rt::Task<rt::Response>(rt::Request)> dispatch{};
  };
  rt::Task<std::optional<std::string>> resolveImageBaseModel();
  json::object buildImageResponsesRequest(const json::object& body, const std::string& baseModel, const ImageRouteKind& kind);
  struct ImageFanoutDispatchCoordinator {
    std::function<api_key_policy::ApiKeyUsageReservation::BeforeProviderDispatch(double)> beforeProviderDispatchFor;
    std::function<rt::Task<void>()> cancelBeforeTransport;
    std::function<void(double)> settled;
  };
  ImageFanoutDispatchCoordinator createImageFanoutDispatchCoordinator(double callCount,
    api_key_policy::ApiKeyUsageReservation::BeforeProviderDispatch beforeProviderDispatch);
  std::vector<json::object> extractImagesFromResponses(const json::value* payload);
  std::vector<json::object> extractImagesFromResponses(const json::value& payload);
  rt::Task<rt::Response> handleImages(rt::Request req, ImageRouteKind kind,
    const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<rt::Response> handleImages(rt::Request req, ImageRouteKind kind,
    const openai_telemetry::UsageContext* usageContext, ImageHandlerOptions options);
}
