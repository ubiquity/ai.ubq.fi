// Differential case for src/provider/removed-provider-telemetry.ts recorded from Bun 1.4.0
// (the module has no TS test): the promise is already fulfilled with undefined, so a
// `then` callback, a queued microtask and an `await` run in that order.
#include "uos/provider/removed_provider_telemetry.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace uos;
using provider::removed_provider_telemetry::recordRemovedProviderTelemetry;

ASYNC_TEST("recordRemovedProviderTelemetry returns an already fulfilled promise") {
  auto order = std::make_shared<std::vector<std::string>>();
  json::object input;
  input["provider"] = "removed";
  input["tokens"] = 3;
  const rt::Promise<void> p = recordRemovedProviderTelemetry(input);
  EXPECT_EQ(p.fulfilled(), true);
  auto then = p.then([order] { order->push_back("then:undefined"); });
  rt::queueMicrotask([order] { order->push_back("microtask"); });
  co_await p.wait();
  order->push_back("awaited:undefined");
  const json::object empty;
  auto caught = recordRemovedProviderTelemetry(empty).catch_(rt::ignoreRejection);
  co_await caught.wait();
  order->push_back("catch:undefined");
  EXPECT_EQ(*order, (std::vector<std::string>{"then:undefined", "microtask", "awaited:undefined", "catch:undefined"}));
}
