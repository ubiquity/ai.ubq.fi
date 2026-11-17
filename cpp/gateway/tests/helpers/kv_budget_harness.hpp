#pragma once
// Port of tests/helpers/kv-budget-harness.ts.
#include "uos/admin/defaults.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/auth/index.hpp"
#include "uos/auth/passkeys.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/defaults.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/handler/index.hpp"
#include "uos/handler/serve_handler.hpp"
#include "uos/responses_handler.hpp"
#include "uos/kernel/quota_reserve.hpp"
#include "uos/kernel/quota_write.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"

namespace uos::tests::helpers::kv_budget_harness {
  std::string encodeKey(const rt::KvKey& key);
  inline const rt::TextEncoder textEncoder;
  std::string kvFingerprint(const rt::KvValue& value);
  std::string encodeBase64Url(const rt::Bytes& bytes);
  std::string encodeJsonBase64Url(const json::value& value);
  std::string toPublicKeyPem(const rt::Bytes& spki);
  struct CountingKvMutation { std::string kind; rt::KvKey key; rt::KvValue value{}; };
  bool isSumMutation(const CountingKvMutation& mutation);
  bool isKernelReservationMutation(const CountingKvMutation& mutation);
  bool isKernelSettlementMutation(const CountingKvMutation& mutation);
  bool isApiKeyV3DispatchMutation(const CountingKvMutation& mutation);
  class CountingAtomic;
  class CountingKv : public rt::Kv {
  public:
    rt::OrderedMap<std::string, rt::KvValue> values;
    double reads = 0, readUnits = 0, writes = 0, sums = 0, sumCommitAttempts = 0;
    double failNextSumCommits = 0, failNextCommits = 0;
    bool failApiKeyV3Reads = false, failKernelQuotaReads = false;
    std::optional<rt::Promise<void>> kernelQuotaReadGate;
    double failNextKernelSettlementCommits = 0;
    std::function<void()> onKernelReservationCommit;
    double sumCommitDelayMs = 0;
    std::optional<rt::Promise<void>> apiKeyV3DispatchCommitGate;
    std::function<void()> onApiKeyV3DispatchCommit;
    double retries = 0, listCalls = 0;
    std::vector<rt::KvKey> readKeys, writeKeys;
    void resetCounts();
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override;
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options = {}) override;
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override;
    rt::Task<void> delete_(rt::KvKey key) override;
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override;
    std::unique_ptr<rt::AtomicOperation> atomic() override;
    void close() override;
  private:
    friend class CountingAtomic;
    struct Version { std::string fingerprint; double revision; };
    rt::OrderedMap<std::string, Version> versions;
    double nextRevision = 1;
    std::optional<std::string> versionstamp(const rt::KvKey& key);
    void write(const rt::KvKey& key, const rt::KvValue& value);
    void remove(const rt::KvKey& key);
    rt::Task<void> commitSumMutation();
    void applyMutation(const CountingKvMutation& mutation);
  };
  extern std::shared_ptr<CountingKv> kv;
  inline constexpr std::string_view MODEL = "gpt-5-kv-budget";
  double now();
  const json::object& runtime();
  json::object codexAuthPool(double accountCount = 1);
  struct SeedKeyResult { std::string hash; json::object record; };
  rt::Task<SeedKeyResult> seedKey(std::string token, std::string id, double limit);
  rt::Task<SeedKeyResult> seedPaidFallbackKey(std::string token, std::string id);
  rt::Request request(const std::string& token);
  rt::Request streamingRequest(const std::string& token, const std::string& route);
  std::string fetchInputUrl(const rt::RequestInfo& input);
  std::string completedSseEvent(double inputTokens = 1, double outputTokens = 1);
  std::string semanticSseEvent(const std::string& text = "fixture output");
  rt::Response sse();
  rt::Response sse(const json::object& usage);
  rt::Response authoritativeCodexQuotaResponse();
  struct Deferred { rt::Promise<void> promise; std::function<void(rt::Reason)> reject; std::function<void()> resolve; };
  Deferred deferred();
  rt::Task<void> waitFor(std::function<bool()> predicate, std::string label, double timeoutMs = 2000);
  json::object usageWindow(const authority::native_contracts::ApiKeyPolicy& policy);
  struct PreparedApiKeyInference : SeedKeyResult { std::string token; authority::native_contracts::ApiKeyPolicy policy; };
  rt::Task<PreparedApiKeyInference> prepareApiKeyInference(std::string tokenDigit, std::string keyId, double limit);
  const rt::crypto::CryptoKeyPair& kernelTestKeyPair();
  rt::Task<void> seedKernelTestPublicKey();
  rt::Task<std::string> makeKernelTestToken(std::string apiToken, std::string owner, std::string repo);
  rt::Task<rt::Request> withKernelTestToken(rt::Request req, std::string apiToken, std::string owner, std::string repo);
  void seedKernelDefaultLimit(double limit, double windowMs = 60000);
  rt::Response validStreamingSse();
  double requiredTerminalTiming(const json::object& terminal, const std::string& field);
  void assertOrderedTerminalTimings(const json::object& terminal, bool expectsDownstreamDrain);
  void installHarness();
  void resetHarness();
  void cleanupHarness();
  using ::uos::handler::index::handler;
  using ::uos::handler::serve_handler::createRequestDeliveryLifecycle;
  using ::uos::responses_handler::handleResponses;
  using ::uos::api_key_policy::API_KEY_USAGE_V3_REQUEST_PREFIX;
  using ::uos::api_key_policy::ApiKeyQuotaDispatchError;
  using ::uos::codex::auth::CODEX_AUTH_POOL_KV_KEY;
  using ::uos::defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY;
  using ::uos::defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY;
  using ::uos::authority::native_kernel::KERNEL_QUOTA_RESERVATION_LEASE_MS;
  using ::uos::auth::passkeys::PASSKEY_RELAY_COOKIE_NAME;
  using ::uos::runtime_config::RUNTIME_CONFIG_CACHE_TTL_MS;
  using ::uos::runtime_config::RUNTIME_CONFIG_V2_KEY;
  using ::uos::authority::native_api_key_policy::apiKeyPolicyFromHashRecord;
  using ::uos::api_key_policy::apiKeyUsageV3RequestKey;
  using ::uos::api_key_policy::apiKeyUsageV3WindowKey;
  using ::uos::auth::index::authenticateAdmin;
  using ::uos::api_key_policy::authenticateApiKeyToken;
  using ::uos::auth::index::authenticateClient;
  using ::uos::kernel::quota_write::deleteKernelOrgUsageLimit;
  using ::uos::provider::metered::fetchMeteredModels;
  using ::uos::provider::health::getCodexProviderHealth;
  using ::uos::admin::defaults::handleAdminDefaults;
  using ::uos::api_key_policy::invalidateApiKeyPolicy;
  using ::uos::authority::native_kernel::kernelOrgReservationKey;
  using ::uos::authority::native_kernel::kernelOrgWindowKey;
  using ::uos::authority::native_kernel::kernelRepoPolicyKey;
  using ::uos::runtime_config::loadRuntimeConfig;
  using ::uos::api_key_policy::makeApiKeyUsageWindowV3;
  using ::uos::paid_fallback::ledger_state::paidFallbackRequestV3Key;
  using ::uos::auth::passkeys::passkeySessionKey;
  using ::uos::auth::passkeys::passkeyUserKey;
  using ::uos::api_key_policy::reserveApiKeyUsageV3;
  using ::uos::kernel::quota_reserve::reserveEffectiveKernelUsageLimit;
  using ::uos::kernel::quota_reserve::reserveKernelOrgUsageLimit;
  using ::uos::api_key_policy::resetApiKeyPolicyCacheForTest;
  using ::uos::codex::auth::resetCodexAuthCacheForTest;
  using ::uos::provider::metered::resetMeteredModelsCacheForTest;
  using ::uos::provider::health::resetProviderHealthThrottleForTest;
  using ::uos::runtime_config::resetRuntimeConfigCacheForTest;
  using ::uos::provider::surplus::resetSurplusModelsCacheForTest;
  using ::uos::kernel::quota_write::setKernelOrgUsageLimit;
  using ::uos::kernel::quota_write::setKernelUsageLimit;
  using ::uos::paid_fallback::index::setPaidFallbackTerminalSweepForTest;
  using ::uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest;
  using ::uos::inference_deadline::setStreamFirstEventDeadlineMsForTest;
  using ::uos::codex::account_routing::CODEX_ACCOUNT_ROUTING_KV_KEY;
  using ::uos::codex::account_routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY;
  using ::uos::codex::account_routing::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY;
  using ::uos::utils::sha256Hex;
  using ::uos::utils::sha256Base64Url;
}
