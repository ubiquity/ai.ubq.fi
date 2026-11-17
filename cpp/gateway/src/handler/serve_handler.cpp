#include "uos/handler/serve_handler.hpp"
#include "uos/handler/index.hpp"


#include <memory>
#include <utility>

namespace uos::handler::serve_handler {
  RequestDeliveryLifecycle createRequestDeliveryLifecycle(rt::AbortSignal requestSignal, rt::Promise<void> completed) {
    auto downstream = std::make_shared<rt::AbortController>();
    auto handedOff = std::make_shared<bool>(false);
    rt::EventListener abortBeforeHandoff([requestSignal, downstream, handedOff]() {
      if (*handedOff || downstream->signal().aborted()) return;
      downstream->abort(requestSignal.reason());
    });
    if (requestSignal.aborted()) {
      if (!*handedOff && !downstream->signal().aborted()) downstream->abort(requestSignal.reason());
    } else {
      requestSignal.addEventListener("abort", abortBeforeHandoff, {.once = true});
    }
    completed.catch_([downstream](rt::Reason reason) {
      if (!downstream->signal().aborted()) downstream->abort(reason);
    });
    return {downstream->signal(), [handedOff, requestSignal, abortBeforeHandoff] {
      if (*handedOff) return;
      *handedOff = true;
      requestSignal.removeEventListener("abort", abortBeforeHandoff);
    }};
  }

  namespace {
    rt::Task<rt::Response> serveRequest(DeliveryAwareHandler requestHandler, rt::Request request, runtime::http::ServeHandlerInfo info) {
      auto delivery = createRequestDeliveryLifecycle(request.signal(), info.completed);
      auto handoff = rt::scopeExit(delivery.handoff);
      http::RequestDeliveryInfo deliveryInfo{info.completed, delivery.signal};
      rt::Promise<rt::Response> response(requestHandler(request, deliveryInfo));
      auto result = co_await response.wait();
      co_return result;
    }
  }

  runtime::http::ServeHandler createServeHandler() {
    DeliveryAwareHandler requestHandler = [](rt::Request request, http::RequestDeliveryInfo delivery) {
      return index::handler(request, delivery);
    };
    return createServeHandler(std::move(requestHandler));
  }

  runtime::http::ServeHandler createServeHandler(DeliveryAwareHandler requestHandler) {
    return [requestHandler = std::move(requestHandler)](rt::Request request, runtime::http::ServeHandlerInfo info) {
      return serveRequest(requestHandler, request, info);
    };
  }

  runtime::http_upgrade::ServeUpgradeHandler createServeUpgradeHandler() { return index::upgradeHandler; }
}
