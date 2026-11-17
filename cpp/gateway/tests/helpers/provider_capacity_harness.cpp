#include "tests/helpers/provider_capacity_harness.hpp"
#include "tests/helpers/counting_kv.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/index.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/metered_quota.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"

namespace uos::tests::helpers::provider_capacity_harness {
  namespace {
    std::optional<std::optional<std::string>> harnessEnvironment;
    rt::KvEntryMaybe<> entryOf(const rt::KvKey& key) {
      const StoredValue* stored = kvStore.get(keyToString(key));
      return {key, stored && !stored->value.isNullish() ? stored->value : rt::KvValue(nullptr),
              stored ? std::optional<std::string>(stored->versionstamp) : std::nullopt};
    }
    class CapacityList final : public rt::KvListIterator {
    public:
      explicit CapacityList(rt::KvListSelector selector_) : selector(std::move(selector_)) {}
      rt::Task<std::optional<rt::KvEntry<>>> next() override {
        std::optional<rt::KvEntry<>> result;
        for (const auto& [encoded, stored] : kvStore) {
          if (seen.has(encoded)) continue;
          seen.add(encoded);
          const rt::KvKey key = rt::kvKeyFromValue(rt::KvValue(rt::jsonParse(encoded)));
          bool matches = true;
          if (selector.prefix) {
            for (std::size_t i = 0; i < selector.prefix->size(); ++i)
              if (i >= key.size() || !rt::kvKeyPartStrictEquals(key[i], (*selector.prefix)[i])) matches = false;
          }
          if (!matches) continue;
          result = rt::KvEntry<>{key, stored.value, stored.versionstamp};
          break;
        }
        return rt::Promise<std::optional<rt::KvEntry<>>>::resolve(std::move(result)).wait();
      }
      std::string cursor() const override { throw rt::TypeError("list.cursor is undefined"); }
      rt::Task<void> return_() override { return rt::Promise<void>::resolve().wait(); }
    private:
      rt::KvListSelector selector;
      rt::OrderedSet<std::string> seen;
    };
    json::object window(double seconds, double used, double resetAt) {
      return {{"limit_window_seconds", rt::makeNumber(seconds)}, {"used_percent", rt::makeNumber(used)}, {"reset_at", rt::makeNumber(resetAt)}};
    }
    json::object codexUsageBody(double primaryUsed, double secondaryUsed, double primaryResetAt, double secondaryResetAt) {
      return {{"rate_limit", json::object{{"primary_window", window(10'800, primaryUsed, primaryResetAt)},
                                         {"secondary_window", window(86'400, secondaryUsed, secondaryResetAt)}}},
              {"private_account_field", "must-not-escape"}};
    }
    json::object codexSparkUsageBody(double primaryUsed, double sparkUsed, double sparkResetAt) {
      return {{"rate_limit", json::object{{"primary_window", window(604'800, primaryUsed, 1'800'010'000)}, {"secondary_window", nullptr}}},
              {"additional_rate_limits", json::array{json::object{{"limit_name", "GPT-5.3-Codex-Spark"}, {"metered_feature", "codex_bengalfox"},
                {"rate_limit", json::object{{"primary_window", window(18'000, sparkUsed, sparkResetAt)}, {"secondary_window", nullptr}}}}}},
              {"private_account_field", "must-not-escape"}};
    }
  }
  CapacityKvStore kvStore;
  std::shared_ptr<CapacityKv> kvStub = std::make_shared<CapacityKv>();
  std::string keyToString(const rt::KvKey& key) { return counting_kv::encodeKey(key); }
  std::string requestUrl(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->href();
    return std::get<rt::Request>(input).url();
  }
  void CapacityKvStore::clearStore() {
    clear(); nextVersion = 0;
    codex::index::resetCodexAuthCacheForTest();
    codex::account_routing::resetCodexAccountRoutingForTest();
  }
  void CapacityKvStore::put(const rt::KvKey& key, const rt::KvValue& value) {
    ++nextVersion;
    set(keyToString(key), StoredValue{value, "v" + rt::numberToString(nextVersion)});
  }
  void CapacityKvStore::remove(const rt::KvKey& key) { delete_(keyToString(key)); }
  std::optional<std::string> CapacityKvStore::version(const rt::KvKey& key) const {
    const StoredValue* stored = get(keyToString(key));
    return stored ? std::optional<std::string>(stored->versionstamp) : std::nullopt;
  }
  rt::Task<rt::KvEntryMaybe<>> CapacityKv::get(rt::KvKey key, rt::KvGetOptions) {
    return rt::Promise<rt::KvEntryMaybe<>>::resolve(entryOf(key)).wait();
  }
  rt::Task<std::vector<rt::KvEntryMaybe<>>> CapacityKv::getMany(std::vector<rt::KvKey>, rt::KvGetOptions) {
    throw rt::TypeError("kv.getMany is not a function");
  }
  rt::Task<rt::KvCommitResult> CapacityKv::set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) {
    kvStore.put(key, value);
    return rt::fulfilledTask(rt::KvCommitResult{true, ""});
  }
  rt::Task<void> CapacityKv::delete_(rt::KvKey key) { kvStore.remove(key); return rt::fulfilledTask(); }
  std::unique_ptr<rt::KvListIterator> CapacityKv::list(rt::KvListSelector selector, rt::KvListOptions) {
    return std::make_unique<CapacityList>(std::move(selector));
  }
  std::unique_ptr<rt::AtomicOperation> CapacityKv::atomic() {
    return std::make_unique<rt::GenericAtomicOperation>([](std::vector<rt::AtomicCheck> checks, std::vector<rt::KvMutation> operations) {
      for (const auto& check : checks)
        if (kvStore.version(check.key) != check.versionstamp) return rt::fulfilledTask(rt::KvCommitResult{});
      for (const auto& operation : operations) {
        if (operation.type == rt::KvMutationType::set) kvStore.put(operation.key, operation.value);
        else if (operation.type == rt::KvMutationType::delete_) kvStore.remove(operation.key);
        else throw rt::TypeError("atomic arithmetic is not a function");
      }
      return rt::fulfilledTask(rt::KvCommitResult{true, "v" + rt::numberToString(static_cast<double>(kvStore.size()))});
    });
  }
  void seed() {
    kvStore.clearStore();
    kvStore.put(codex::index::CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::object{{"accounts", json::array{
      json::object{{"access_token", "token-one"}, {"refresh_token", "refresh-one"}, {"account_id", "account-one"}, {"updated_at_ms", nowMs}},
      json::object{{"access_token", "token-two"}, {"refresh_token", "refresh-two"}, {"account_id", "account-two"}, {"updated_at_ms", nowMs}}}},
      {"updated_at_ms", nowMs}}));
    kvStore.put(metered_quota::METERED_QUOTA_STATE_KEY, rt::KvValue(json::object{
      {"current_balance_quota", 750}, {"post_refill_baseline_quota", 1'000}, {"last_observed_used_quota", 250}, {"quota_per_credit", 100},
      {"observed_at_ms", nowMs - 1'000}, {"cycle_started_at_ms", nowMs - 5'000}, {"confidence", "refill_observed"}, {"last_known_debits_quota", 0},
      {"last_inferred_credit_quota", 1'000}, {"last_credit_at_ms", nowMs - 5'000}, {"latest_refill_id", "refill-one"}, {"latest_refill_amount_credits", 10},
      {"latest_refill_completed_at_ms", nowMs - 5'000}}));
    rt::env::set("METERED_API_KEY", "metered-api-key");
  }
  rt::Fetch createFetcher(std::vector<FetchCall>& calls, std::optional<std::string> failureAccount, MeteredUsage metered,
                          UsageForAccount codexUsage, bool codexSpark, double codexSparkResetAt, int failureStatus,
                          std::function<bool(std::optional<std::string>)> codexSparkForAccount, UsageForAccount codexResetAt) {
    return rt::Fetch([&calls, failureAccount, metered, codexUsage, codexSpark, codexSparkResetAt, failureStatus,
                      codexSparkForAccount, codexResetAt](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
      const rt::Headers headers = init.headers.value_or(rt::Headers());
      const std::string url = requestUrl(input);
      const auto account = headers.get("ChatGPT-Account-ID");
      calls.push_back({account, headers.get("Authorization"), url});
      rt::ResponseInit responseInit;
      responseInit.status = 200; responseInit.headers = rt::Headers{{"Content-Type", "application/json"}};
      if (url == "https://api.openlux.ai/api/usage/token/") {
        json::object data{{"total_available", metered.total_available.value_or(750)}, {"total_granted", metered.total_granted.value_or(1'000)},
                          {"total_used", metered.total_used.value_or(250)}, {"unlimited_quota", metered.unlimited_quota.value_or(false)}};
        return rt::Response(rt::BodyInit(rt::jsonStringify(json::object{{"success", true}, {"data", data}})), responseInit);
      }
      if (account == failureAccount) {
        responseInit.status = failureStatus; responseInit.headers.reset();
        return rt::Response(rt::BodyInit(std::string("upstream-secret-body")), responseInit);
      }
      const auto used = codexUsage ? codexUsage(account) : account == "account-one" ? std::array<double, 2>{12.5, 38} : std::array<double, 2>{67, 81.25};
      const auto resetAt = codexResetAt ? codexResetAt(account) : std::array<double, 2>{1'800'010'000, 1'800'020'000};
      const bool includeCodexSpark = codexSparkForAccount ? codexSparkForAccount(account) : codexSpark;
      const json::object body = includeCodexSpark ? codexSparkUsageBody(used[0], used[1], codexSparkResetAt)
                                                : codexUsageBody(used[0], used[1], resetAt[0], resetAt[1]);
      return rt::Response(rt::BodyInit(rt::jsonStringify(body)), responseInit);
    });
  }
  void installHarness() {
    if (!harnessEnvironment) harnessEnvironment.emplace(rt::env::get("METERED_API_KEY"));
    kv::setKvForTest(kvStub);
  }
  void resetHarness() { kvStore.clearStore(); installHarness(); }
  void cleanupHarness() {
    if (harnessEnvironment) {
      if (*harnessEnvironment) rt::env::set("METERED_API_KEY", **harnessEnvironment);
      else rt::env::remove("METERED_API_KEY");
    }
    harnessEnvironment.reset(); kv::setKvForTest(nullptr);
  }
  HarnessScope::HarnessScope()
    : environment(std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/cpp-c2-capacity"}}),
      savedSeed(config::config.codexAuthJsonB64) {
    config::config.codexAuthJsonB64.clear(); resetHarness();
  }
  HarnessScope::~HarnessScope() { cleanupHarness(); config::config.codexAuthJsonB64 = savedSeed; }
}
