#include "tests/helpers/codex_account_routing_harness.hpp"
#include "tests/helpers/counting_kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

namespace uos::tests::helpers::codex_account_routing_harness {
  namespace {
    std::string stamp(double version) { return rt::padStart(rt::numberToString(version), 20, "0"); }
    class RoutingAtomic final : public rt::AtomicOperation {
    public:
      explicit RoutingAtomic(RoutingKv& owner_) : owner(owner_) {}
      using rt::AtomicOperation::check;
      using rt::AtomicOperation::max;
      using rt::AtomicOperation::min;
      using rt::AtomicOperation::sum;
      rt::AtomicOperation& check(std::vector<rt::AtomicCheck> entries) override {
        for (auto& entry : entries) checks.push_back(std::move(entry));
        return *this;
      }
      rt::AtomicOperation& set(rt::KvKey kvKey, rt::KvValue value, rt::KvSetOptions = {}) override {
        writes.emplace_back(std::move(kvKey), std::move(value));
        return *this;
      }
      rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("op.mutate is not a function"); }
      rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("op.sum is not a function"); }
      rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("op.min is not a function"); }
      rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("op.max is not a function"); }
      rt::AtomicOperation& delete_(rt::KvKey) override { throw rt::TypeError("op.delete is not a function"); }
      rt::Task<rt::KvCommitResult> commit() override {
        for (const auto& entry : checks) {
          const double* version = owner.versions.get(key(entry.key));
          const auto versionstamp = version ? std::optional<std::string>(stamp(*version)) : std::nullopt;
          if (versionstamp != entry.versionstamp) {
            rt::KvCommitResult failed;
            return rt::Promise<rt::KvCommitResult>::resolve(failed).wait();
          }
        }
        double last = 0;
        for (const auto& write : writes) owner.values.set(key(write.first), write.second);
        for (const auto& write : writes) {
          const std::string encoded = key(write.first);
          const double* current = owner.versions.get(encoded);
          last = (current ? *current : 0) + 1;
          owner.versions.set(encoded, last);
        }
        rt::KvCommitResult result{true, stamp(last)};
        return rt::Promise<rt::KvCommitResult>::resolve(result).wait();
      }
    private:
      RoutingKv& owner;
      std::vector<std::pair<rt::KvKey, rt::KvValue>> writes;
      std::vector<rt::AtomicCheck> checks;
    };
  }

  std::string key(const rt::KvKey& value) { return counting_kv::encodeKey(value); }

  rt::Response httpDateQuotaResponse(double deadline) {
    rt::ResponseInit init;
    init.status = 429;
    init.headers = rt::Headers{{"Content-Type", "application/json"}, {"Retry-After", rt::Date(deadline).toUTCString()}};
    return rt::Response(rt::BodyInit(std::string("{\"error\":{\"type\":\"usage_limit_reached\"}}")), init);
  }

  rt::Response deltaSecondsQuotaResponse(const std::string& retryAfter) {
    rt::ResponseInit init;
    init.status = 429;
    init.headers = rt::Headers{{"Content-Type", "application/json"}, {"Retry-After", retryAfter}};
    return rt::Response(rt::BodyInit(std::string("{\"error\":{\"type\":\"usage_limit_reached\"}}")), init);
  }

  json::object syntheticLegacyBlock(double blockedUntilMs, double quotaSignalObservedAtMs) {
    return {{"blocked_until_ms", rt::makeNumber(blockedUntilMs)}, {"source", "header_retry_after"},
            {"quota_signal_observed_at_ms", rt::makeNumber(quotaSignalObservedAtMs)}, {"observed_reset_at_ms", nullptr},
            {"observed_reset_at_is_stable", false}, {"banked_reset_generation_ambiguous", false}, {"banked_reset_recovery_probe_pending", false}};
  }

  std::string requestUrl(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->toString();
    return std::get<rt::Request>(input).url();
  }

  rt::KvEntryMaybe<> RoutingKv::entryOf(const rt::KvKey& kvKey) const {
    const std::string encoded = key(kvKey);
    const rt::KvValue* value = values.get(encoded);
    const double* version = versions.get(encoded);
    return {kvKey, value && !value->isUndefined() ? *value : rt::KvValue(nullptr),
            version ? std::optional<std::string>(stamp(*version)) : std::nullopt};
  }
  rt::Task<rt::KvEntryMaybe<>> RoutingKv::get(rt::KvKey kvKey, rt::KvGetOptions) {
    return rt::Promise<rt::KvEntryMaybe<>>::resolve(entryOf(kvKey)).wait();
  }
  rt::Task<std::vector<rt::KvEntryMaybe<>>> RoutingKv::getMany(std::vector<rt::KvKey> kvKeys, rt::KvGetOptions) {
    std::vector<rt::KvEntryMaybe<>> snapshot;
    for (const auto& kvKey : kvKeys) snapshot.push_back(entryOf(kvKey));
    return rt::Promise<std::vector<rt::KvEntryMaybe<>>>::resolve(std::move(snapshot)).wait();
  }
  rt::Task<rt::KvCommitResult> RoutingKv::set(rt::KvKey kvKey, rt::KvValue value, rt::KvSetOptions) {
    const std::string encoded = key(kvKey);
    const double* current = versions.get(encoded);
    const double version = (current ? *current : 0) + 1;
    values.set(encoded, std::move(value));
    versions.set(encoded, version);
    rt::KvCommitResult result{true, stamp(version)};
    return rt::Promise<rt::KvCommitResult>::resolve(result).wait();
  }
  rt::Task<void> RoutingKv::delete_(rt::KvKey) { throw rt::TypeError("kv.delete is not a function"); }
  std::unique_ptr<rt::KvListIterator> RoutingKv::list(rt::KvListSelector, rt::KvListOptions) { throw rt::TypeError("kv.list is not a function"); }
  std::unique_ptr<rt::AtomicOperation> RoutingKv::atomic() { return std::make_unique<RoutingAtomic>(*this); }
  void RoutingKv::close() { throw rt::TypeError("kv.close is not a function"); }
  json::value RoutingKv::row(const rt::KvKey& kvKey) const {
    const rt::KvValue* value = values.get(key(kvKey));
    return value ? value->toJson() : json::value(nullptr);
  }

  const ::uos::types::CodexAuthPoolState pool{
    .accounts = {{.access_token = "access-one", .refresh_token = "refresh-one", .account_id = "one", .updated_at_ms = 1},
                 {.access_token = "access-two", .refresh_token = "refresh-two", .account_id = "two", .updated_at_ms = 1}}, .updated_at_ms = 1};
  const ::uos::types::CodexAuthPoolState singlePool{.accounts = {pool.accounts[0]}, .updated_at_ms = pool.updated_at_ms};

  rt::Task<void> seedSubscriptionSelection(std::shared_ptr<RoutingKv> kv, std::vector<std::string> accountIds) {
    std::vector<rt::Promise<std::string>> pendingIds;
    for (const auto& id : accountIds) {
      auto identify = [id]() -> rt::Task<std::string> {
        rt::Promise<std::string> hashing(provider::selection::codexSubscriptionHash(id));
        const std::string hash = co_await hashing.wait();
        co_return provider::selection::codexSubscriptionSelectionId(hash);
      };
      pendingIds.emplace_back(rt::async(identify));
    }
    auto allIds = rt::all(std::move(pendingIds));
    const auto selectedIds = co_await allIds.wait();
    json::array providerIds;
    for (const auto& id : selectedIds) providerIds.push_back(json::value(id));
    const rt::KvValue value(json::object{{"provider_ids", providerIds}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}});
    auto writing = kv->set(PROVIDER_SELECTION_KV_KEY, value);
    co_await std::move(writing);
    resetProviderSelectionCacheForTest();
  }
}
