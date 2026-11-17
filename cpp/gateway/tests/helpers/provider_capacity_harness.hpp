#pragma once
// Port of tests/helpers/provider-capacity-harness.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::tests::helpers::provider_capacity_harness {
  struct StoredValue { rt::KvValue value; std::string versionstamp; };
  std::string keyToString(const rt::KvKey& key);
  std::string requestUrl(const rt::RequestInfo& input);

  class CapacityKvStore : public rt::OrderedMap<std::string, StoredValue> {
  public:
    void clearStore();
    void put(const rt::KvKey& key, const rt::KvValue& value);
    void remove(const rt::KvKey& key);
    std::optional<std::string> version(const rt::KvKey& key) const;
  private:
    double nextVersion = 0;
  };
  extern CapacityKvStore kvStore;

  class CapacityKv : public rt::Kv {
  public:
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override;
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options = {}) override;
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override;
    rt::Task<void> delete_(rt::KvKey key) override;
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override;
    std::unique_ptr<rt::AtomicOperation> atomic() override;
    void close() override {}
  };
  extern std::shared_ptr<CapacityKv> kvStub;
  inline constexpr double nowMs = 1'800'000'000'000;
  void seed();

  struct FetchCall {
    std::optional<std::string> account;
    std::optional<std::string> authorization;
    std::string url;
  };
  struct MeteredUsage {
    std::optional<double> total_available{};
    std::optional<double> total_granted{};
    std::optional<double> total_used{};
    std::optional<bool> unlimited_quota{};
  };
  using UsageForAccount = std::function<std::array<double, 2>(std::optional<std::string>)>;
  rt::Fetch createFetcher(std::vector<FetchCall>& calls, std::optional<std::string> failureAccount = std::nullopt,
                          MeteredUsage metered = {}, UsageForAccount codexUsage = {}, bool codexSpark = false,
                          double codexSparkResetAt = 1'800'011'000, int failureStatus = 503,
                          std::function<bool(std::optional<std::string>)> codexSparkForAccount = {}, UsageForAccount codexResetAt = {});
  void installHarness();
  void resetHarness();
  void cleanupHarness();
  class HarnessScope {
  public:
    rt::env::ScopedEnvironment environment;
    std::string savedSeed;
    HarnessScope();
    ~HarnessScope();
    HarnessScope(const HarnessScope&) = delete;
    HarnessScope& operator=(const HarnessScope&) = delete;
  };
}
