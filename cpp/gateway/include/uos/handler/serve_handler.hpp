#pragma once
// Port of src/handler/serve-handler.ts.
#include "uos/handler/http.hpp"
#include "uos/runtime/http.hpp"

#include <functional>

namespace uos::handler::serve_handler {
  struct RequestDeliveryLifecycle {
    rt::AbortSignal signal;
    std::function<void()> handoff;
  };

  RequestDeliveryLifecycle createRequestDeliveryLifecycle(rt::AbortSignal requestSignal, rt::Promise<void> completed);

  using DeliveryAwareHandler = std::function<rt::Task<rt::Response>(rt::Request request, http::RequestDeliveryInfo delivery)>;

  runtime::http::ServeHandler createServeHandler();
  runtime::http::ServeHandler createServeHandler(DeliveryAwareHandler requestHandler);

  runtime::http_upgrade::ServeUpgradeHandler createServeUpgradeHandler();
}
