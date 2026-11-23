#pragma once
// Port of tests/helpers/codex-auth-cache-harness.ts.
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/banked_reset.hpp"
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/codex/experiment.hpp"
#include "uos/codex/index.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/capacity_contract.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::tests::helpers::codex_auth_cache_harness {
  inline const rt::KvKey AUTH_KEY{"ubq_ai", "codex_auth"};
  inline constexpr double fixedStartMs = 1'000'000;

  struct FakeKvWrite {
    std::string type;
    rt::KvKey key;
    rt::KvValue value{};
  };
  bool isAuthKey(const rt::KvKey& key);
  std::string probeErrorType(double status);
  bool isProviderHealthSuccessWrite(const FakeKvWrite& write);
  std::string keyOf(const rt::KvKey& key);

  class AuthKv : public rt::Kv {
  public:
    struct ExtraEntry {
      rt::KvValue value;
      double version;
      friend bool operator==(const ExtraEntry&, const ExtraEntry&) = default;
    };
    ::uos::types::CodexAuthPoolState auth;
    double reads = 0;
    double routingReads = 0;
    double routingCommitFailures = 0;
    std::optional<rt::Promise<void>> nextReadGate;
    std::function<rt::Task<void>(double)> onRoutingRead;
    std::optional<rt::Promise<void>> providerHealthSuccessCommitGate;
    std::function<void()> onProviderHealthSuccessCommit;
    std::function<void()> onProviderHealthSuccessCommitted;
    double authVersion = 1;
    rt::OrderedMap<std::string, ExtraEntry> extra;

    explicit AuthKv(::uos::types::CodexAuthPoolState auth_);
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override;
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options = {}) override;
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override;
    rt::Task<void> delete_(rt::KvKey key) override;
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override;
    std::unique_ptr<rt::AtomicOperation> atomic() override;
    void close() override {}

  private:
    struct SnapshotRead {
      rt::KvEntryMaybe<> entry;
      bool authRead;
      std::optional<double> routingRead;
    };
    SnapshotRead snapshotRead(const rt::KvKey& key);
    std::optional<std::string> checkVersion(const rt::KvKey& key) const;
    void applyWrite(const FakeKvWrite& write);
    rt::Task<rt::KvCommitResult> commit(std::vector<rt::AtomicCheck> checks, std::vector<rt::KvMutation> mutations);
  };

  std::string encodeBase64Url(const json::value& value);
  std::string accessToken(const std::string& label);
  ::uos::types::CodexAuthState auth(const std::string& label);
  ::uos::types::CodexAuthState staleAuth(const std::string& label);
  ::uos::types::CodexAuthPoolState pool(std::vector<::uos::types::CodexAuthState> accounts);
  template <class... Accounts> ::uos::types::CodexAuthPoolState pool(Accounts... accounts) {
    return pool(std::vector<::uos::types::CodexAuthState>{std::move(accounts)...});
  }
  extern std::shared_ptr<AuthKv> kv;

  std::string requestUrl(const rt::RequestInfo& input);
  rt::Reason abortReasonError(const rt::Reason& reason);
  codex::banked_reset::CodexBankedResetConfig liveBankedResetConfig();
  codex::dispatch::FetchCodexResponsesOptions bankedResetRequestOptions(const std::string& requestId);
  std::string stableBankedResetRetryAfter();
  rt::Task<void> seedStableBankedResetBlock(std::string accountId = "account-one", double observedAtMs = fixedStartMs);

  struct ScriptedResetOptions {
    std::optional<bool> verify{};
    std::optional<std::string> redeemKind{};
    std::function<rt::Task<void>()> onInventory{};
    std::function<void()> onRedeem{};
    std::function<void()> onVerify{};
    std::optional<rt::Promise<void>> redeemGate{};
  };
  class ScriptedResetProvider : public codex::banked_reset_provider::CodexUsageResetProvider {
  public:
    ScriptedResetOptions options;
    std::vector<std::string> calls;
    std::vector<std::string> inventoryAccountIds;
    std::vector<std::string> idempotencyKeys;
    std::vector<std::string> redeemAccountIds;
    explicit ScriptedResetProvider(ScriptedResetOptions options_);
    rt::Task<codex::banked_reset_provider::ResetInventory> readInventory(codex::banked_reset_provider::ResetAccountContext input,
                                                                      rt::AbortSignal signal) override;
    rt::Task<codex::banked_reset_provider::RedeemResetResult> redeem(codex::banked_reset_provider::RedeemResetInput input,
                                                                  rt::AbortSignal signal) override;
    rt::Task<codex::banked_reset_provider::RedeemResetResult> lookup(codex::banked_reset_provider::LookupRedeemResetInput input,
                                                                  rt::AbortSignal signal) override;
    rt::Task<json::value> verifyApplied(codex::banked_reset_provider::ResetAccountContext input, rt::AbortSignal signal) override;
  };
  std::shared_ptr<ScriptedResetProvider> scriptedResetProvider(ScriptedResetOptions options = {});
  void installHarness();
  void resetHarness();
  void cleanupHarness();

  // C++ scope for the suites' beforeEach/afterEach and mandatory credential isolation.
  class HarnessScope {
  public:
    rt::env::ScopedEnvironment environment;
    bool savedDeploy;
    std::string savedSeed;
    std::string savedBase;
    HarnessScope();
    ~HarnessScope();
    HarnessScope(const HarnessScope&) = delete;
    HarnessScope& operator=(const HarnessScope&) = delete;
  };
}
