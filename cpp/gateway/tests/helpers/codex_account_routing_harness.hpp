#pragma once
// Port of tests/helpers/codex-account-routing-harness.ts.
#include "uos/codex/account_routing.hpp"
#include "uos/codex/index.hpp"
#include "uos/kv.hpp"
#include "uos/provider/capacity_contract.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::tests::helpers::codex_account_routing_harness {
  std::string key(const rt::KvKey& value);
  rt::Response httpDateQuotaResponse(double deadline);
  rt::Response deltaSecondsQuotaResponse(const std::string& retryAfter);
  json::object syntheticLegacyBlock(double blockedUntilMs, double quotaSignalObservedAtMs);

  template <class T> const T& required(const std::optional<T>& value, std::string_view what) {
    if (!value) throw rt::JsError("expected " + std::string(what) + " to be present");
    return *value;
  }
  template <class T> const T& required(const T* value, std::string_view what) {
    if (!value) throw rt::JsError("expected " + std::string(what) + " to be present");
    return *value;
  }
  std::string requestUrl(const rt::RequestInfo& input);

  // This fake deliberately uses one decimal version counter per key, as the TS does.
  // getMany snapshots synchronously at the call; every result resumes one microtask later.
  class RoutingKv : public rt::Kv {
  public:
    rt::OrderedMap<std::string, rt::KvValue> values;
    rt::OrderedMap<std::string, double> versions;
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey kvKey, rt::KvGetOptions options = {}) override;
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> kvKeys, rt::KvGetOptions options = {}) override;
    rt::Task<rt::KvCommitResult> set(rt::KvKey kvKey, rt::KvValue value, rt::KvSetOptions options = {}) override;
    rt::Task<void> delete_(rt::KvKey kvKey) override;
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override;
    std::unique_ptr<rt::AtomicOperation> atomic() override;
    void close() override;
    // C++ view of values.get(key(...)); an absent row is null.
    json::value row(const rt::KvKey& kvKey) const;
    rt::KvEntryMaybe<> entryOf(const rt::KvKey& kvKey) const;
  };

  extern const ::uos::types::CodexAuthPoolState pool;
  extern const ::uos::types::CodexAuthPoolState singlePool;
  rt::Task<void> seedSubscriptionSelection(std::shared_ptr<RoutingKv> kv, std::vector<std::string> accountIds);

  using ::uos::types::CodexAuthPoolState;
  using kv::setKvForTest;
  using codex::account_routing::resetCodexAccountRoutingForTest;
  using codex::index::CODEX_AUTH_POOL_KV_KEY;
  using codex::index::CodexError;
  using codex::index::fetchCodexResponses;
  using codex::index::getCodexRoutingError;
  using codex::index::resetCodexAuthCacheForTest;
  using codex::account_routing::claimCodexRoutingProbe;
  using codex::account_routing::CODEX_ACCOUNT_ROUTING_KV_KEY;
  using codex::account_routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY;
  using codex::account_routing::CODEX_CAPACITY_ROUTING_MAX_AGE_MS;
  using codex::account_routing::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY;
  using codex::account_routing::CODEX_HALF_OPEN_LEASE_MS;
  using codex::account_routing::CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS;
  using codex::account_routing::codexCredentialVersion;
  using codex::account_routing::electCodexResetRecoveryAccount;
  using codex::account_routing::getCodexQuotaBlockFence;
  using codex::account_routing::markCodexCredentialInvalid;
  using codex::account_routing::markCodexQuotaBlocked;
  using codex::account_routing::markCodexRecoveryProbeQuotaBlocked;
  using codex::account_routing::markCodexSuccess;
  using codex::account_routing::markCodexUpstreamTimeout;
  using codex::account_routing::parseCodexActiveAccountSelection;
  using codex::account_routing::parseCodexAccountRoutingState;
  using codex::account_routing::readCodex429;
  using codex::account_routing::recheckCodexRoutingSlot;
  using codex::account_routing::reconcileCodexQuotaAfterStaleVerifiedReset;
  using codex::account_routing::reconcileCodexQuotaAfterVerifiedReset;
  using codex::account_routing::reconcileCodexRoutingAccount;
  using codex::account_routing::recordCodexCapacityRoutingObservations;
  using codex::account_routing::refreshCodexActiveAccountAdmission;
  using codex::account_routing::selectCodexRoutingAccounts;
  using codex::account_routing::selectCodexRoutingAccountsStrong;
  using provider::capacity_contract::PROVIDER_CAPACITY_SNAPSHOT_KEY;
  using provider::selection::PROVIDER_SELECTION_KV_KEY;
  using provider::selection::resetProviderSelectionCacheForTest;
}
