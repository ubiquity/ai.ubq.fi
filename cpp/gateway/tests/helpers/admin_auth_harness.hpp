#pragma once
// Port of tests/helpers/admin-auth-harness.ts. Reuses the real admin/index exports and source helpers.
//
// Timing: like the TS stub (whose methods return `Promise.resolve(...)`), every call does its work
// when it runs and its result arrives one microtask later.
#include "uos/admin/index.hpp"
#include "uos/analytics.hpp"
#include "uos/codex/auth.hpp"
#include "uos/kernel/quota_v2.hpp"
#include "uos/kv_conversion/profile_json.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::tests::helpers::admin_auth_harness {
  // `JSON.stringify(key)`. A bigint or byte-array part throws TypeError (JSON.stringify cannot write it).
  std::string keyToString(const rt::KvKey& key);

  // `urlOf(input: RequestInfo | URL)`
  std::string urlOf(const rt::RequestInfo& input);

  // `class TestKvStore extends Map<string, unknown>`: `clear()` also resets the commit-failure seams
  // and the recorded `atomicSetCalls`.
  class TestKvStore : public rt::OrderedMap<std::string, rt::KvValue> {
  public:
    void clear();
  };

  // The shared store, keyed by keyToString(key). Touched only after main starts.
  extern TestKvStore kvStore;

  /** Records the `expireIn` option every `kv.atomic().set()` call receives. */
  struct AtomicSetCall {
    rt::KvKey key;
    std::optional<double> expireIn;
  };
  extern std::vector<AtomicSetCall> atomicSetCalls;

  // `compareKvKeyPart(left, right)`: numbers by difference, everything else by String(part) (`<`).
  double compareKvKeyPart(const rt::KvKeyPart& left, const rt::KvKeyPart& right);
  double compareKvKeys(const rt::KvKey& left, const rt::KvKey& right);
  // `prefix.every((part, index) => key[index] === part)`
  bool matchesPrefix(const rt::KvKey& key, const rt::KvKey& prefix);
  bool kvStoreHasPrefix(const rt::KvKey& prefix);

  // `kvStub`: get / set / delete / list (a numeric index cursor) / atomic (checks are accepted,
  // commits apply sets and deletes unless a failure seam is armed) / close. The same instance
  // every call.
  std::shared_ptr<rt::Kv> kvStub();

  void setAtomicCommitsToFail(double value);
  // std::nullopt is null.
  void setAtomicCommitsBeforeFailure(std::optional<double> value);

  /** Reinstall this fixture explicitly when a Bun test file uses the cached harness. */
  void installHarness();
  void resetHarness();
  void cleanupHarness();
  inline const std::string DISABLED_FALLBACK_TOKEN = "u_" + std::string(64, 'a');
  inline const std::string ENABLED_FALLBACK_TOKEN = "u_" + std::string(64, 'b');
  inline const std::string FAILED_FALLBACK_TOKEN = "u_" + std::string(64, 'c');
  inline const json::object authPayload{{"tokens", json::object{{"access_token", "access"}, {"refresh_token", "refresh"}, {"account_id", "acct"}}}};
  std::string stringEntryLine(const rt::KvKey& key, const std::string& value);
  void seedCodexSnapshot(const models::codex_models::CodexModelsSnapshot& snapshot);
  rt::Request makeRequest(const json::value& body);
  rt::Response meteredMetadataResponse(const std::string& url);
  using ::uos::admin::api_keys::handleAdminApiKeysCreate;
  using ::uos::admin::api_key_mutations::handleAdminApiKeysDelete;
  using ::uos::admin::api_keys::handleAdminApiKeysList;
  using ::uos::admin::api_keys::handleAdminApiKeysPaidFallbacks;
  using ::uos::admin::api_key_mutations::handleAdminApiKeysUnrevoke;
  using ::uos::admin::api_key_mutations::handleAdminApiKeysUpdate;
  using ::uos::admin::codex::handleAdminCodexAuth;
  using ::uos::admin::codex::handleAdminCodexModelsGet;
  using ::uos::admin::codex::handleAdminCodexModelsSet;
  using ::uos::admin::kernel::handleAdminCodexResetSettings;
  using ::uos::admin::defaults::handleAdminDefaults;
  using ::uos::admin::kernel::handleAdminKernelUsageDelete;
  using ::uos::admin::kernel::handleAdminKernelUsageSet;
  using ::uos::admin::defaults::handleAdminKvMigrationImport;
  using ::uos::analytics::listApiKeyRequestLogs;
  using ::uos::analytics::recordApiKeyRequestLog;
  using ::uos::runtime_config::buildRuntimeConfig;
  using ::uos::runtime_config::cacheRuntimeConfig;
  using ::uos::runtime_config::resetRuntimeConfigCacheForTest;
  using ::uos::codex::auth::resetCodexAuthCacheForTest;
  using ::uos::kernel::quota_v2::getKernelUsageLimitSnapshot;
  using ::uos::authority::native_kernel::kernelRepoPolicyKey;
  using ::uos::authority::native_kernel::kernelRepoWindowKey;

}
