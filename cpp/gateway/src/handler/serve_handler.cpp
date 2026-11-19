#include "uos/handler/serve_handler.hpp"
#include "uos/handler/index.hpp"

#include <memory>
#include <utility>

namespace uos::handler::serve_handler {
  RequestDeliveryLifecycle createRequestDeliveryLifecycle(rt::AbortSignal requestSignal, rt::Promise<void> completed) {
    rt::AbortController downstream;
    rt::EventListener abortBeforeHandoff;
    if (requestSignal.aborted()) {
      downstream.abort(requestSignal.reason());
    } else {
      const std::weak_ptr source = requestSignal.state();
      abortBeforeHandoff = rt::EventListener([source, downstream] {
        if (auto signal = source.lock()) downstream.abort(rt::AbortSignal(std::move(signal)).reason());
      });
      requestSignal.addEventListener("abort", abortBeforeHandoff, {.once = true});
    }
    completed.catch_([downstream](rt::Reason reason) { downstream.abort(std::move(reason)); });
    return {downstream.signal(), [requestSignal = std::move(requestSignal), abortBeforeHandoff = std::move(abortBeforeHandoff)] {
      requestSignal.removeEventListener("abort", abortBeforeHandoff);
    }};
  }

  namespace {
    rt::Task<rt::Response> serveRequest(DeliveryAwareHandler requestHandler, rt::Request request, runtime::http::ServeHandlerInfo info) {
      auto delivery = createRequestDeliveryLifecycle(request.signal(), info.completed);
      auto handoff = rt::scopeExit(std::move(delivery.handoff));
      http::RequestDeliveryInfo deliveryInfo{std::move(info.completed), std::move(delivery.signal)};
      rt::Promise<rt::Response> response(requestHandler(std::move(request), std::move(deliveryInfo)));
      auto result = co_await response.wait();
      co_return result;
    }
  }

  runtime::http::ServeHandler createServeHandler() {
    DeliveryAwareHandler requestHandler = [](rt::Request request, http::RequestDeliveryInfo delivery) {
      return index::handler(std::move(request), std::move(delivery));
    };
    return createServeHandler(std::move(requestHandler));
  }

  runtime::http::ServeHandler createServeHandler(DeliveryAwareHandler requestHandler) {
    return [requestHandler = std::move(requestHandler)](rt::Request request, runtime::http::ServeHandlerInfo info) {
      return serveRequest(requestHandler, std::move(request), std::move(info));
    };
  }

  runtime::http_upgrade::ServeUpgradeHandler createServeUpgradeHandler() { return index::upgradeHandler; }
}
