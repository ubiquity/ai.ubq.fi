// Differential cases for src/kernel/quota-write.ts, which has no direct bun test: each case runs the
// scenario the bun oracle ran (the TS module on tests/helpers/counting-kv.ts with Date.now fixed at
// NOW) and compares the result and every stored entry (key, value with its member order,
// versionstamp) with the recorded output in kernel_quota_write_differential_vectors.inc.
// Links: tests/kernel_quota_write_differential_test.cpp src/kernel/quota_write.cpp src/kernel/quota_reservations.cpp
//        src/kernel/quota_v2.cpp src/types.cpp src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp src/http.cpp
//        src/auth/relay.cpp src/config.cpp src/provider/cerebras_rate_limits.cpp src/release.cpp src/utils.cpp src/kv.cpp
//        src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
//        tests/helpers/counting_kv.cpp
#include "uos/kernel/quota_v2.hpp"
#include "uos/kernel/quota_write.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/defaults.hpp"
#include "uos/kv.hpp"
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
#include <variant>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace kvfacade = uos::kv;
  namespace quota_v2 = uos::kernel::quota_v2;
  namespace quota_write = uos::kernel::quota_write;
  using uos::authority::native_kernel_contracts::KernelQuotaPolicySetOptions;
  using uos::tests::helpers::counting_kv::CountingKv;

  constexpr std::string_view ORACLE =
#include "kernel_quota_write_differential_vectors.inc"
    ;

  constexpr double NOW = 1'800'000'000'000;

  class ConflictKv;

  class ConflictAtomicOperation final : public rt::ForwardingAtomicOperation {
  public:
    ConflictAtomicOperation(ConflictKv& owner, std::unique_ptr<rt::AtomicOperation> operation) : rt::ForwardingAtomicOperation(std::move(operation)), owner_(owner) {}
    rt::Task<rt::KvCommitResult> commit() override;

  private:
    ConflictKv& owner_;
  };

  // The oracle's `ConflictKv extends CountingKv` with `commitsToFail` lost compare-and-sets.
  class ConflictKv final : public rt::ForwardingKv {
  public:
    ConflictKv() : ConflictKv(std::make_shared<CountingKv>()) {}
    double commitsToFail = 0;
    void seed(const rt::KvKey& key, const rt::KvValue& value) { counting_->seed(key, value); }
    const CountingKv& counting() const { return *counting_; }
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<ConflictAtomicOperation>(*this, rt::ForwardingKv::atomic()); }

  private:
    explicit ConflictKv(std::shared_ptr<CountingKv> counting) : rt::ForwardingKv(*counting), counting_(std::move(counting)) {}
    std::shared_ptr<CountingKv> counting_;
  };

  rt::Task<rt::KvCommitResult> ConflictAtomicOperation::commit() {
    if (owner_.commitsToFail > 0) {
      owner_.commitsToFail -= 1;
      co_return rt::KvCommitResult{};
    }
    rt::Task<rt::KvCommitResult> committing = rt::ForwardingAtomicOperation::commit();
    rt::KvCommitResult result = co_await std::move(committing);
    co_return result;
  }

  // `{ ...base, ...overrides }`
  json::object spread(json::object base, const json::object& overrides) {
    for (const auto& member : overrides) rt::objectDefine(base, member.key(), member.value());
    return base;
  }

  json::object members(std::initializer_list<std::pair<std::string_view, json::value>> entries) {
    json::object out;
    for (const auto& [key, value] : entries) rt::objectDefine(out, key, value);
    return out;
  }

  json::object identity(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo) {
    json::object out;
    out["v"] = 2;
    out["scope"] = scope;
    out["owner"] = owner;
    if (repo) out["repo"] = *repo;
    return out;
  }

  rt::KvValue policy(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo, const json::object& overrides = {}) {
    json::object row = identity(scope, owner, repo);
    row["usage_limit_requests"] = 5;
    row["window_ms"] = 60'000;
    row["expires_at_ms"] = -1;
    row["created_at_ms"] = 1'000;
    row["updated_at_ms"] = 2'000;
    return rt::KvValue(json::value(spread(std::move(row), overrides)));
  }

  json::object windowRowObject(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo, const json::object& overrides = {}) {
    json::object row = identity(scope, owner, repo);
    row["usage_requests"] = 2;
    row["reserved_requests"] = 0;
    row["usage_reset_at_ms"] = rt::makeNumber(NOW + 30'000);
    row["applied_window_ms"] = 60'000;
    row["created_at_ms"] = rt::makeNumber(NOW - 30'000);
    row["updated_at_ms"] = rt::makeNumber(NOW - 1'000);
    return spread(std::move(row), overrides);
  }

  rt::KvValue windowRow(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo, const json::object& overrides = {}) {
    return rt::KvValue(json::value(windowRowObject(scope, owner, repo, overrides)));
  }

  rt::KvValue reservation(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo, const json::object& overrides = {}) {
    json::object row = identity(scope, owner, repo);
    row["request_id"] = "req-1";
    row["route"] = "/v1/responses";
    row["window_created_at_ms"] = rt::makeNumber(NOW - 30'000);
    row["window_reset_at_ms"] = rt::makeNumber(NOW + 30'000);
    row["state"] = "reserved";
    row["terminal_intent"] = nullptr;
    row["reserved_at_ms"] = rt::makeNumber(NOW - 10'000);
    row["lease_expires_at_ms"] = rt::makeNumber(NOW + 290'000);
    row["committed_at_ms"] = nullptr;
    row["released_at_ms"] = nullptr;
    row["release_reason"] = nullptr;
    return rt::KvValue(json::value(spread(std::move(row), overrides)));
  }

  rt::KvValue cutoverMarker() {
    return rt::KvValue(json::value(members({{"v", 2}, {"id", "lease"}, {"created_at_ms", rt::makeNumber(NOW)}, {"expires_at_ms", rt::makeNumber(NOW + 300'000)}})));
  }

  const rt::KvKey& repoPolicyKey() {
    static const rt::KvKey key = quota_v2::kernelRepoPolicyKey("acme", "demo");
    return key;
  }
  const rt::KvKey& repoWindowKey() {
    static const rt::KvKey key = quota_v2::kernelRepoWindowKey("acme", "demo");
    return key;
  }
  const rt::KvKey& orgPolicyKey() {
    static const rt::KvKey key = quota_v2::kernelOrgPolicyKey("acme");
    return key;
  }
  const rt::KvKey& orgWindowKey() {
    static const rt::KvKey key = quota_v2::kernelOrgWindowKey("acme");
    return key;
  }

  template <class Record> json::value recordJson(const std::optional<Record>& record) { return record ? json::value_from(*record) : json::value(nullptr); }

  json::value deleteJson(const quota_write::KernelQuotaDeleteResult& result) {
    if (!result) return nullptr;
    if (const bool* deleted = std::get_if<bool>(&*result)) return json::value(*deleted);
    return json::value(json::string(std::get<std::string>(*result)));
  }

  json::value entriesJson(const ConflictKv& kv) {
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

  // Runs one scenario on a fresh store and compares it with the oracle's entry of that name.
  template <class Scenario> rt::Task<void> expectOracle(std::string name, Scenario scenario) {
    rt::globalDateNow() = [] { return NOW; };
    const rt::console::Capture quiet({rt::console::Method::warn});
    const auto kv = std::make_shared<ConflictKv>();
    kvfacade::setKvForTest(kv);
    rt::Task<json::value> running = scenario(kv);
    json::value result = co_await std::move(running);
    kvfacade::setKvForTest(nullptr);
    json::object actual;
    actual["name"] = name;
    actual["result"] = std::move(result);
    actual["entries"] = entriesJson(*kv);
    const json::value oracle = rt::jsonParse(ORACLE);
    const json::value* expected = nullptr;
    for (const json::value& entry : oracle.as_array()) {
      if (rt::getString(entry, "name") == std::optional<std::string_view>(name)) expected = &entry;
    }
    ASSERT(expected != nullptr);
    EXPECT_EQ(rt::jsonStringify(json::value(std::move(actual))), rt::jsonStringify(*expected));
  }
}

ASYNC_TEST("repo create on an empty store") {
  auto scenario = [](std::shared_ptr<ConflictKv>) -> rt::Task<json::value> {
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 5);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("repo create on an empty store", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("repo update keeps the current window") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), policy("repo", "acme", "demo"));
    kv->seed(repoWindowKey(), windowRow("repo", "acme", "demo"));
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 9);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("repo update keeps the current window", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("repo window change resets usage") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), policy("repo", "acme", "demo"));
    kv->seed(repoWindowKey(), windowRow("repo", "acme", "demo"));
    KernelQuotaPolicySetOptions options;
    options.windowMs = 120'000.7;
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 9, options);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("repo window change resets usage", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("repo reset with a live reservation aborts") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), policy("repo", "acme", "demo"));
    kv->seed(repoWindowKey(), windowRow("repo", "acme", "demo", members({{"reserved_requests", 1}})));
    kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", NOW - 30'000, "req-1"), reservation("repo", "acme", "demo"));
    KernelQuotaPolicySetOptions options;
    options.resetUsage = true;
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 9, options);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("repo reset with a live reservation aborts", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("repo reset reclaims an expired reservation and retries") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), policy("repo", "acme", "demo"));
    kv->seed(repoWindowKey(), windowRow("repo", "acme", "demo", members({{"reserved_requests", 1}})));
    kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", NOW - 30'000, "req-1"),
             reservation("repo", "acme", "demo", members({{"lease_expires_at_ms", rt::makeNumber(NOW - 1)}})));
    KernelQuotaPolicySetOptions options;
    options.resetUsage = true;
    options.expiresAtMs = NOW + 5'000;
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 9, options);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("repo reset reclaims an expired reservation and retries", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("repo reset settles a committed intent as committed") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), policy("repo", "acme", "demo"));
    kv->seed(repoWindowKey(), windowRow("repo", "acme", "demo", members({{"reserved_requests", 1}})));
    kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", NOW - 30'000, "req-1"),
             reservation("repo", "acme", "demo", members({{"lease_expires_at_ms", rt::makeNumber(NOW - 1)}, {"terminal_intent", "committed"}})));
    KernelQuotaPolicySetOptions options;
    options.resetUsage = true;
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 9, options);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("repo reset settles a committed intent as committed", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("org create normalizes the limit, window and expiry") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(uos::defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY, rt::KvValue(40));
    kv->seed(uos::defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY, rt::KvValue("90000"));
    KernelQuotaPolicySetOptions options;
    options.expiresAtMs = NOW + 1'000.9;
    options.windowMs = 0;
    auto setting = quota_write::setKernelOrgUsageLimit("acme", -5, options);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("org create normalizes the limit, window and expiry", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("org update with a string limit coerced") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(orgPolicyKey(), policy("org", "acme", std::nullopt, members({{"usage_limit_requests", 7}})));
    kv->seed(orgWindowKey(), windowRow("org", "acme", std::nullopt, members({{"usage_requests", 3}, {"created_at_ms", 0}})));
    KernelQuotaPolicySetOptions options;
    options.expiresAtMs = -7;
    auto setting = quota_write::setKernelOrgUsageLimit("acme", 12.9, options);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("org update with a string limit coerced", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a default cutover marker blocks a default-backed write") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, cutoverMarker());
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 5);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("a default cutover marker blocks a default-backed write", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a malformed window aborts the write") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoWindowKey(), rt::KvValue(json::value(members({{"v", 2}, {"scope", "repo"}}))));
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 5);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("a malformed window aborts the write", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a malformed policy fails closed") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), rt::KvValue(json::value(members({{"v", 2}}))));
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 5);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("a malformed policy fails closed", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a repo policy over live org reservations aborts") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(orgPolicyKey(), policy("org", "acme", std::nullopt));
    kv->seed(orgWindowKey(), windowRow("org", "acme", std::nullopt, members({{"reserved_requests", 1}})));
    kv->seed(quota_v2::kernelOrgReservationKey("acme", NOW - 30'000, "req-1"), reservation("org", "acme", std::nullopt));
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 3);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("a repo policy over live org reservations aborts", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a repo policy over an expired org reservation reclaims and retries") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(orgPolicyKey(), policy("org", "acme", std::nullopt));
    kv->seed(orgWindowKey(), windowRow("org", "acme", std::nullopt, members({{"reserved_requests", 1}})));
    kv->seed(quota_v2::kernelOrgReservationKey("acme", NOW - 30'000, "req-1"),
             reservation("org", "acme", std::nullopt, members({{"lease_expires_at_ms", rt::makeNumber(NOW - 1)}})));
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 3);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("a repo policy over an expired org reservation reclaims and retries", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a repo policy over a legacy org window counts its reservations") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    json::object legacy = windowRowObject("org", "acme", std::nullopt);
    rt::objectDelete(legacy, "reserved_requests");
    kv->seed(orgPolicyKey(), policy("org", "acme", std::nullopt));
    kv->seed(orgWindowKey(), rt::KvValue(json::value(std::move(legacy))));
    kv->seed(quota_v2::kernelOrgReservationKey("acme", NOW - 30'000, "req-1"),
             reservation("org", "acme", std::nullopt,
                         members({{"state", "committed"}, {"committed_at_ms", rt::makeNumber(NOW - 5)}, {"terminal_intent", "committed"}})));
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 3);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("a repo policy over a legacy org window counts its reservations", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("set exhausts its compare-and-set attempts") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->commitsToFail = 3;
    auto setting = quota_write::setKernelUsageLimit("acme", "demo", 5);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("set exhausts its compare-and-set attempts", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("set succeeds after two lost compare-and-sets") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->commitsToFail = 2;
    auto setting = quota_write::setKernelOrgUsageLimit("acme", 0);
    const auto record = co_await std::move(setting);
    co_return recordJson(record);
  };
  auto checking = expectOracle("set succeeds after two lost compare-and-sets", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("delete of an absent policy") {
  auto scenario = [](std::shared_ptr<ConflictKv>) -> rt::Task<json::value> {
    auto deleting = quota_write::deleteKernelUsageLimit("acme", "demo");
    const quota_write::KernelQuotaDeleteResult deleted = co_await std::move(deleting);
    co_return deleteJson(deleted);
  };
  auto checking = expectOracle("delete of an absent policy", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("delete of a present policy restores the default window") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), policy("repo", "acme", "demo", members({{"window_ms", 120'000}})));
    kv->seed(repoWindowKey(), windowRow("repo", "acme", "demo", members({{"applied_window_ms", 120'000}, {"usage_requests", 4}})));
    auto deleting = quota_write::deleteKernelUsageLimit("acme", "demo");
    const quota_write::KernelQuotaDeleteResult deleted = co_await std::move(deleting);
    co_return deleteJson(deleted);
  };
  auto checking = expectOracle("delete of a present policy restores the default window", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("delete keeps a window matching the default") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(uos::defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY, rt::KvValue(60'000));
    kv->seed(orgPolicyKey(), policy("org", "acme", std::nullopt));
    kv->seed(orgWindowKey(), windowRow("org", "acme", std::nullopt));
    auto deleting = quota_write::deleteKernelOrgUsageLimit("acme");
    const quota_write::KernelQuotaDeleteResult deleted = co_await std::move(deleting);
    co_return deleteJson(deleted);
  };
  auto checking = expectOracle("delete keeps a window matching the default", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("delete during a default cutover is a conflict") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), policy("repo", "acme", "demo"));
    kv->seed(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, cutoverMarker());
    auto deleting = quota_write::deleteKernelUsageLimit("acme", "demo");
    const quota_write::KernelQuotaDeleteResult deleted = co_await std::move(deleting);
    co_return deleteJson(deleted);
  };
  auto checking = expectOracle("delete during a default cutover is a conflict", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("delete with a live reservation is a conflict") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), policy("repo", "acme", "demo"));
    kv->seed(repoWindowKey(), windowRow("repo", "acme", "demo", members({{"reserved_requests", 1}})));
    kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", NOW - 30'000, "req-1"), reservation("repo", "acme", "demo"));
    auto deleting = quota_write::deleteKernelUsageLimit("acme", "demo");
    const quota_write::KernelQuotaDeleteResult deleted = co_await std::move(deleting);
    co_return deleteJson(deleted);
  };
  auto checking = expectOracle("delete with a live reservation is a conflict", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("delete reclaims an expired reservation and retries") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), policy("repo", "acme", "demo"));
    kv->seed(repoWindowKey(), windowRow("repo", "acme", "demo", members({{"reserved_requests", 1}})));
    kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", NOW - 30'000, "req-1"),
             reservation("repo", "acme", "demo", members({{"lease_expires_at_ms", rt::makeNumber(NOW - 1)}})));
    auto deleting = quota_write::deleteKernelUsageLimit("acme", "demo");
    const quota_write::KernelQuotaDeleteResult deleted = co_await std::move(deleting);
    co_return deleteJson(deleted);
  };
  auto checking = expectOracle("delete reclaims an expired reservation and retries", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("delete with a malformed window aborts") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), policy("repo", "acme", "demo"));
    kv->seed(repoWindowKey(), rt::KvValue(json::value(members({{"v", 2}}))));
    auto deleting = quota_write::deleteKernelUsageLimit("acme", "demo");
    const quota_write::KernelQuotaDeleteResult deleted = co_await std::move(deleting);
    co_return deleteJson(deleted);
  };
  auto checking = expectOracle("delete with a malformed window aborts", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("delete exhausts its compare-and-set attempts") {
  auto scenario = [](std::shared_ptr<ConflictKv> kv) -> rt::Task<json::value> {
    kv->seed(repoPolicyKey(), policy("repo", "acme", "demo"));
    kv->commitsToFail = 3;
    auto deleting = quota_write::deleteKernelUsageLimit("acme", "demo");
    const quota_write::KernelQuotaDeleteResult deleted = co_await std::move(deleting);
    co_return deleteJson(deleted);
  };
  auto checking = expectOracle("delete exhausts its compare-and-set attempts", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("set and delete without KV") {
  auto scenario = [](std::shared_ptr<ConflictKv>) -> rt::Task<json::value> {
    kvfacade::setKvForTest(nullptr);
    auto setting = quota_write::setKernelOrgUsageLimit("acme", 5);
    const auto record = co_await std::move(setting);
    auto deleting = quota_write::deleteKernelOrgUsageLimit("acme");
    const quota_write::KernelQuotaDeleteResult deleted = co_await std::move(deleting);
    json::object both;
    both["set"] = recordJson(record);
    both["deleted"] = deleteJson(deleted);
    co_return json::value(std::move(both));
  };
  auto checking = expectOracle("set and delete without KV", scenario);
  co_await std::move(checking);
}
