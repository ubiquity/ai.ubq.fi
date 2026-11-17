// Differential cases for src/provider/removed-provider-circuit.ts (no bun test imports it): the
// expected values were recorded from Bun 1.4.0 awaiting the TypeScript module's promises.
#include "uos/provider/removed_provider_circuit.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/test_async.hpp"
#include <memory>

namespace {
  using namespace uos::provider::removed_provider_circuit;
  namespace rt = uos::rt;
}

ASYNC_TEST("every retired circuit operation resolves to its fixed value") {
  rt::Promise<RemovedProviderCircuitRoute> selecting = selectRemovedProviderCircuitRoute();
  const RemovedProviderCircuitRoute route = co_await selecting.wait();
  EXPECT_EQ(route.route, "codex");
  EXPECT(route.probe == nullptr);
  EXPECT_EQ(route.transition, "none");
  rt::Promise<std::nullptr_t> claiming = claimRemovedProviderEarlyRecoveryProbe();
  const std::nullptr_t claimed = co_await claiming.wait();
  EXPECT(claimed == nullptr);
  rt::Promise<std::string> closing = closeRemovedProviderCircuit(std::nullopt);
  const std::string closed = co_await closing.wait();
  EXPECT_EQ(closed, "none");
  rt::Promise<std::string> recording = recordRemovedProviderEligibleFailure(RemovedProviderCircuitProbe{});
  const std::string recorded = co_await recording.wait();
  EXPECT_EQ(recorded, "none");
  rt::Promise<std::string> releasing = releaseRemovedProviderCircuitProbe(std::nullopt);
  const std::string released = co_await releasing.wait();
  EXPECT_EQ(released, "none");
  rt::Promise<void> renewing = renewRemovedProviderCircuitProbe(RemovedProviderCircuitProbe{});
  EXPECT(renewing.fulfilled());
  co_await renewing.wait();
}

ASYNC_TEST("each call returns a new promise that is already fulfilled") {
  const rt::Promise<RemovedProviderCircuitRoute> first = selectRemovedProviderCircuitRoute();
  const rt::Promise<RemovedProviderCircuitRoute> second = selectRemovedProviderCircuitRoute();
  EXPECT(!(first == second));
  EXPECT(first.fulfilled() && second.fulfilled());
  co_return;
}

ASYNC_TEST("awaiting a result takes one microtask turn, as awaiting Promise.resolve does") {
  auto order = std::make_shared<std::vector<std::string>>();
  rt::Promise<void> awaiting = rt::Promise<void>(rt::async([order]() -> rt::Task<void> {
    rt::Promise<std::string> closing = closeRemovedProviderCircuit(std::nullopt);
    co_await closing.wait();
    order->push_back("await-close");
  }));
  rt::queueMicrotask([order] { order->push_back("m1"); });
  rt::queueMicrotask([order] { rt::queueMicrotask([order] { order->push_back("m2"); }); });
  co_await awaiting.wait();
  co_await rt::microtask();
  co_await rt::microtask();
  EXPECT_EQ(*order, (std::vector<std::string>{"await-close", "m1", "m2"}));
}
