// Differential cases for the timers of kernelReservationContext (src/kernel/quota-reservations.ts):
// lease renewal, lease expiry and settlement retry. The bun oracle replaced the global timer
// functions with a manual scheduler and fired each timer at the clock its deadline implies; here
// the fake loop clock fires them (rt::sleep advances it) while Date.now reads `clockMs`, set to
// the same values. The oracle lists the armed timers; these cases compare their number
// (Loop::timerCount), the settlement outcomes, the abort reason and every stored entry.
// Links: tests/kernel_quota_context_differential_test.cpp src/kernel/quota_reservations.cpp src/kernel/quota_v2.cpp
//        src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp src/http.cpp src/auth/relay.cpp src/config.cpp
//        src/provider/cerebras_rate_limits.cpp src/release.cpp src/utils.cpp src/kv.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/native_kv.cpp tests/helpers/counting_kv.cpp
#include "uos/kernel/quota_reservations.hpp"
#include "uos/kernel/quota_v2.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/kv.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace kvfacade = uos::kv;
  namespace quota_v2 = uos::kernel::quota_v2;
  namespace reservations = uos::kernel::quota_reservations;
  using uos::tests::helpers::counting_kv::CountingKv;

  constexpr std::string_view ORACLE =
#include "kernel_quota_context_differential_vectors.inc"
    ;

  constexpr double NOW = 1'800'000'000'000;
  constexpr double W = NOW - 30'000;
  const std::string ROUTE = "/v1/responses";

  // `let clock = NOW; Date.now = () => clock;`
  double clockMs = NOW;

  class ThrowingKv;

  class ThrowingAtomicOperation final : public rt::ForwardingAtomicOperation {
  public:
    ThrowingAtomicOperation(ThrowingKv& owner, std::unique_ptr<rt::AtomicOperation> operation) : rt::ForwardingAtomicOperation(std::move(operation)), owner_(owner) {}
    rt::Task<rt::KvCommitResult> commit() override;

  private:
    ThrowingKv& owner_;
  };

  // The oracle's `ThrowingKv extends CountingKv`: commits reject while `throwOnCommit` is set.
  class ThrowingKv final : public rt::ForwardingKv {
  public:
    ThrowingKv() : ThrowingKv(std::make_shared<CountingKv>()) {}
    bool throwOnCommit = false;
    void seed(const rt::KvKey& key, const rt::KvValue& value) { counting_->seed(key, value); }
    const CountingKv& counting() const { return *counting_; }
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<ThrowingAtomicOperation>(*this, rt::ForwardingKv::atomic()); }

  private:
    explicit ThrowingKv(std::shared_ptr<CountingKv> counting) : rt::ForwardingKv(*counting), counting_(std::move(counting)) {}
    std::shared_ptr<CountingKv> counting_;
  };

  rt::Task<rt::KvCommitResult> ThrowingAtomicOperation::commit() {
    if (owner_.throwOnCommit) throw rt::JsError("kv commit failed");
    rt::Task<rt::KvCommitResult> committing = rt::ForwardingAtomicOperation::commit();
    rt::KvCommitResult result = co_await std::move(committing);
    co_return result;
  }

  const rt::KvKey& reservationKey() {
    static const rt::KvKey key = quota_v2::kernelOrgReservationKey("acme", W, "req-1");
    return key;
  }

  void seedRows(ThrowingKv& kv, double leaseExpiresAtMs) {
    json::object window;
    window["v"] = 2;
    window["scope"] = "org";
    window["owner"] = "acme";
    window["usage_requests"] = 0;
    window["reserved_requests"] = 1;
    window["usage_reset_at_ms"] = rt::makeNumber(NOW + 600'000);
    window["applied_window_ms"] = 630'000;
    window["created_at_ms"] = rt::makeNumber(W);
    window["updated_at_ms"] = rt::makeNumber(W);
    kv.seed(quota_v2::kernelOrgWindowKey("acme"), rt::KvValue(json::value(std::move(window))));
    json::object row;
    row["v"] = 2;
    row["scope"] = "org";
    row["owner"] = "acme";
    row["request_id"] = "req-1";
    row["route"] = ROUTE;
    row["window_created_at_ms"] = rt::makeNumber(W);
    row["window_reset_at_ms"] = rt::makeNumber(NOW + 600'000);
    row["state"] = "reserved";
    row["terminal_intent"] = nullptr;
    row["reserved_at_ms"] = rt::makeNumber(NOW - 1);
    row["lease_expires_at_ms"] = rt::makeNumber(leaseExpiresAtMs);
    row["committed_at_ms"] = nullptr;
    row["released_at_ms"] = nullptr;
    row["release_reason"] = nullptr;
    kv.seed(reservationKey(), rt::KvValue(json::value(std::move(row))));
  }

  // The number of armed timers (the oracle's `active()` list, compared by its length).
  json::value timers() { return rt::makeNumber(static_cast<double>(rt::loop().timerCount())); }

  rt::Task<json::value> rejectionOf(rt::Promise<void> promise) {
    std::optional<std::string> message;
    try {
      co_await promise.wait();
    } catch (const rt::JsError& error) {
      message = error.message;
    }
    co_return message ? json::value(json::string(*message)) : json::value(nullptr);
  }

  rt::Task<json::value> leaseOf(std::shared_ptr<ThrowingKv> kv) {
    const rt::KvKey key = reservationKey();
    const rt::KvEntryMaybe<> entry = co_await kv->get(key);
    co_return rt::makeNumber(entry.value["lease_expires_at_ms"].asNumber());
  }

  json::value entriesJson(const ThrowingKv& kv) {
    json::array entries;
    for (const auto& [encoded, entry] : kv.counting().entries) {
      json::array key;
      for (const rt::KvKeyPart& part : entry.key) key.push_back(part.isString() ? json::value(json::string(part.string())) : rt::makeNumber(part.number()));
      json::object row;
      row["key"] = std::move(key);
      row["value"] = rt::canonicalNumbers(entry.value.toJson());
      row["versionstamp"] = entry.versionstamp;
      entries.push_back(std::move(row));
    }
    return json::value(std::move(entries));
  }

  // The oracle's entry of that name with every `timers` list replaced by its length.
  json::value expectedOf(std::string_view name) {
    json::value oracle = rt::jsonParse(ORACLE);
    for (json::value& scenario : oracle.as_array()) {
      if (rt::getString(scenario, "name") != std::optional<std::string_view>(name)) continue;
      for (json::value& step : scenario.as_object()["steps"].as_array()) {
        json::value* armed = rt::get(step.as_object(), "timers");
        if (armed != nullptr) *armed = rt::makeNumber(static_cast<double>(armed->as_array().size()));
      }
      return scenario;
    }
    return nullptr;
  }

  // A fresh store installed as the facade KV, Date.now at NOW, warnings silenced.
  struct Fixture {
    std::shared_ptr<ThrowingKv> kv = std::make_shared<ThrowingKv>();
    rt::console::Capture quiet{{rt::console::Method::warn}};
    Fixture() {
      clockMs = NOW;
      rt::globalDateNow() = [] { return clockMs; };
      kvfacade::setKvForTest(kv);
    }
    ~Fixture() { kvfacade::setKvForTest(nullptr); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void expectOracle(std::string_view name, json::array steps) const {
      json::object actual;
      actual["name"] = name;
      actual["steps"] = std::move(steps);
      actual["entries"] = entriesJson(*kv);
      EXPECT_EQ(rt::jsonStringify(json::value(std::move(actual))), rt::jsonStringify(expectedOf(name)));
    }
  };
}

ASYNC_TEST("renewal extends the lease and reschedules its expiry") {
  const Fixture fixture;
  const std::shared_ptr<ThrowingKv> kv = fixture.kv;
  seedRows(*kv, NOW + 300'000);
  const quota_v2::KernelQuotaReservation context = reservations::kernelReservationContext(kv, "org", "acme", std::nullopt, W, "req-1", ROUTE, NOW + 300'000, 1'000.0);
  json::array steps;
  json::object first;
  first["timers"] = timers();
  steps.push_back(std::move(first));
  clockMs = NOW + 1'000;
  co_await rt::sleep(1'000);
  json::object renewed;
  renewed["timers"] = timers();
  renewed["lease"] = co_await leaseOf(kv);
  steps.push_back(std::move(renewed));
  clockMs = NOW + 2'000;
  co_await rt::sleep(1'000);
  json::object renewedAgain;
  renewedAgain["timers"] = timers();
  renewedAgain["lease"] = co_await leaseOf(kv);
  steps.push_back(std::move(renewedAgain));
  json::object committed;
  auto committing = rejectionOf(context.commit());
  committed["commit"] = co_await std::move(committing);
  committed["timers"] = timers();
  committed["aborted"] = context.signal.aborted();
  steps.push_back(std::move(committed));
  fixture.expectOracle("renewal extends the lease and reschedules its expiry", std::move(steps));
}

ASYNC_TEST("a kernel reservation stops renewing when its last settlement handle is dropped") {
  const Fixture fixture;
  const auto kv = fixture.kv;
  constexpr double interval = 60'000;
  constexpr double initialExpiry = NOW + 300'000;
  seedRows(*kv, initialExpiry);
  auto context = reservations::kernelReservationContext(kv, "org", "acme", std::nullopt, W, "req-1", ROUTE, initialExpiry, interval);
  auto commit = context.commit;
  context.commit = {};
  context.release = {};

  clockMs = NOW + interval;
  co_await rt::sleep(interval);
  auto reading = leaseOf(kv);
  const auto renewed = co_await std::move(reading);
  EXPECT_EQ(renewed, rt::makeNumber(initialExpiry + interval));
  EXPECT(!context.signal.aborted());

  commit = {};
  clockMs = NOW + 2 * interval;
  co_await rt::sleep(interval);
  auto checking = leaseOf(kv);
  const auto lease = co_await std::move(checking);
  EXPECT_EQ(lease, renewed);
  EXPECT(!context.signal.aborted());

  const double untilExpiry = initialExpiry + interval - clockMs;
  clockMs += untilExpiry;
  co_await rt::sleep(untilExpiry);
  EXPECT(context.signal.aborted());
  EXPECT_EQ(rt::errorName(context.signal.reason()), "TimeoutError");
}

ASYNC_TEST("an expired lease aborts and stops renewal until release") {
  const Fixture fixture;
  const std::shared_ptr<ThrowingKv> kv = fixture.kv;
  seedRows(*kv, NOW + 100);
  const quota_v2::KernelQuotaReservation context = reservations::kernelReservationContext(kv, "org", "acme", std::nullopt, W, "req-1", ROUTE, NOW + 100, 1'000.0);
  json::array steps;
  json::object first;
  first["timers"] = timers();
  steps.push_back(std::move(first));
  clockMs = NOW + 100;
  co_await rt::sleep(100);
  const rt::Reason reason = context.signal.reason();
  json::object expired;
  expired["timers"] = timers();
  expired["aborted"] = context.signal.aborted();
  expired["name"] = rt::errorName(reason).value_or("");
  expired["message"] = rt::errorMessage(reason).value_or("");
  steps.push_back(std::move(expired));
  clockMs = NOW + 200;
  json::object released;
  auto releasing = rejectionOf(context.release(std::string("client_gone")));
  released["release"] = co_await std::move(releasing);
  released["timers"] = timers();
  steps.push_back(std::move(released));
  fixture.expectOracle("an expired lease aborts and stops renewal until release", std::move(steps));
}

ASYNC_TEST("an early lease timer reschedules for the remaining time") {
  const Fixture fixture;
  const std::shared_ptr<ThrowingKv> kv = fixture.kv;
  seedRows(*kv, NOW + 100);
  const quota_v2::KernelQuotaReservation context = reservations::kernelReservationContext(kv, "org", "acme", std::nullopt, W, "req-1", ROUTE, NOW + 100, 1'000.0);
  json::array steps;
  clockMs = NOW + 50;
  co_await rt::sleep(100);
  json::object early;
  early["timers"] = timers();
  early["aborted"] = context.signal.aborted();
  steps.push_back(std::move(early));
  clockMs = NOW + 100;
  co_await rt::sleep(50);
  json::object expired;
  expired["timers"] = timers();
  expired["aborted"] = context.signal.aborted();
  steps.push_back(std::move(expired));
  json::object committed;
  auto committing = rejectionOf(context.commit());
  committed["commit"] = co_await std::move(committing);
  committed["timers"] = timers();
  steps.push_back(std::move(committed));
  fixture.expectOracle("an early lease timer reschedules for the remaining time", std::move(steps));
}

ASYNC_TEST("a terminal intent keeps renewing after the lease expires") {
  const Fixture fixture;
  const std::shared_ptr<ThrowingKv> kv = fixture.kv;
  seedRows(*kv, NOW + 100);
  kv->throwOnCommit = true;
  const quota_v2::KernelQuotaReservation context = reservations::kernelReservationContext(kv, "org", "acme", std::nullopt, W, "req-1", ROUTE, NOW + 100, 700.0);
  json::array steps;
  json::object failed;
  auto committing = rejectionOf(context.commit());
  failed["commit"] = co_await std::move(committing);
  failed["timers"] = timers();
  steps.push_back(std::move(failed));
  clockMs = NOW + 100;
  co_await rt::sleep(100);
  json::object expired;
  expired["timers"] = timers();
  expired["aborted"] = context.signal.aborted();
  steps.push_back(std::move(expired));
  kv->throwOnCommit = false;
  clockMs = NOW + 700;
  co_await rt::sleep(600);
  json::object renewed;
  renewed["timers"] = timers();
  renewed["lease"] = co_await leaseOf(kv);
  steps.push_back(std::move(renewed));
  clockMs = NOW + 1'000;
  co_await rt::sleep(300);
  json::object settled;
  settled["timers"] = timers();
  auto committingAgain = rejectionOf(context.commit());
  settled["again"] = co_await std::move(committingAgain);
  steps.push_back(std::move(settled));
  fixture.expectOracle("a terminal intent keeps renewing after the lease expires", std::move(steps));
}

ASYNC_TEST("settlement retries back off until the store recovers") {
  const Fixture fixture;
  const std::shared_ptr<ThrowingKv> kv = fixture.kv;
  seedRows(*kv, NOW + 300'000);
  kv->throwOnCommit = true;
  const quota_v2::KernelQuotaReservation context = reservations::kernelReservationContext(kv, "org", "acme", std::nullopt, W, "req-1", ROUTE, NOW + 300'000, 60'000.0);
  json::array steps;
  json::object failed;
  auto releasing = rejectionOf(context.release(std::string("upstream_failed")));
  failed["release"] = co_await std::move(releasing);
  failed["timers"] = timers();
  steps.push_back(std::move(failed));
  clockMs = NOW + 1'000;
  co_await rt::sleep(1'000);
  json::object retried;
  retried["timers"] = timers();
  steps.push_back(std::move(retried));
  clockMs = NOW + 3'000;
  kv->throwOnCommit = false;
  co_await rt::sleep(2'000);
  json::object recovered;
  recovered["timers"] = timers();
  auto releasingAgain = rejectionOf(context.release(std::string("ignored")));
  recovered["again"] = co_await std::move(releasingAgain);
  auto committingLate = rejectionOf(context.commit());
  recovered["late"] = co_await std::move(committingLate);
  steps.push_back(std::move(recovered));
  fixture.expectOracle("settlement retries back off until the store recovers", std::move(steps));
}
