// Port of src/authority/native-runtime-contracts.ts.
#ifndef UOS_NATIVE_RUNTIME_CONTRACTS_HPP
#include "uos/authority/native_authority_contracts.hpp"
#ifndef UOS_NATIVE_RUNTIME_CONTRACTS_HPP
#define UOS_NATIVE_RUNTIME_CONTRACTS_HPP
namespace uos::authority::native_runtime_contracts {
  inline constexpr double NATIVE_RUNTIME_REVISION = 3;
  struct NativeRuntimeIdentityKvApiKey {
    static constexpr std::string_view kind = "kv_api_key";
    std::string keyId;
  };
  struct NativeRuntimeIdentityLocalDevelopmentKey {
    static constexpr std::string_view kind = "local_development_key";
    std::string keyId;
  };
  struct NativeRuntimeIdentityGithubToken {
    static constexpr std::string_view kind = "github_token";
    std::string owner;
    std::string repo;
  };
  struct NativeRuntimeIdentityPasskeySession {
    static constexpr std::string_view kind = "passkey_session";
    std::string userId;
  };
  struct NativeRuntimeIdentityAuthTokensAllowlist {
    static constexpr std::string_view kind = "auth_tokens_allowlist";
  };
  struct NativeRuntimeIdentityAdminAllowlist {
    static constexpr std::string_view kind = "admin_allowlist";
  };
  struct NativeRuntimeIdentityDenoDeployToken {
    static constexpr std::string_view kind = "deno_deploy_token";
  };
  struct NativeRuntimeIdentityDisabledLocal {
    static constexpr std::string_view kind = "disabled_local";
    std::optional<std::string> bearerDigest;
  };
  using NativeRuntimeIdentity = std::variant<NativeRuntimeIdentityKvApiKey, NativeRuntimeIdentityLocalDevelopmentKey, NativeRuntimeIdentityGithubToken, NativeRuntimeIdentityPasskeySession, NativeRuntimeIdentityAuthTokensAllowlist, NativeRuntimeIdentityAdminAllowlist, NativeRuntimeIdentityDenoDeployToken, NativeRuntimeIdentityDisabledLocal>;
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeIdentity& value);
  NativeRuntimeIdentity tag_invoke(const json::value_to_tag<NativeRuntimeIdentity>&, const json::value& value);
  struct NativeRuntimeGrant {
    std::string id;
    double revision;
    std::string epoch;
    std::string subjectId;
    std::string contextBinding;
    std::vector<native_authority_contracts::NativeRole> roles;
    double expiresAtMs;
    bool revoked;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeGrant& value);
  NativeRuntimeGrant tag_invoke(const json::value_to_tag<NativeRuntimeGrant>&, const json::value& value);
  struct NativeRuntimeBinding {
    std::string grantId;
    std::string contextBinding;
    json::value privateContext;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeBinding& value);
  NativeRuntimeBinding tag_invoke(const json::value_to_tag<NativeRuntimeBinding>&, const json::value& value);
  struct NativeRuntimeApiScope {
    native_principal_contracts::NativePrincipalReference principalRef;
    std::string tokenHash;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeApiScope& value);
  NativeRuntimeApiScope tag_invoke(const json::value_to_tag<NativeRuntimeApiScope>&, const json::value& value);
  struct NativeRuntimeKernel {
    std::string owner;
    std::string repo;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeKernel& value);
  NativeRuntimeKernel tag_invoke(const json::value_to_tag<NativeRuntimeKernel>&, const json::value& value);
  struct VerifiedNativeRuntime {
    std::string bindingId;
    std::string subjectId;
    std::string contextBinding;
    std::string intentDigest;
    NativeRuntimeIdentity identity;
    std::optional<NativeRuntimeApiScope> api;
    std::optional<NativeRuntimeKernel> kernel;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const VerifiedNativeRuntime& value);
  VerifiedNativeRuntime tag_invoke(const json::value_to_tag<VerifiedNativeRuntime>&, const json::value& value);
  struct NativeValidatedMetadata {
    std::string model;
    bool stream;
    std::optional<std::string> reasoning;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeValidatedMetadata& value);
  NativeValidatedMetadata tag_invoke(const json::value_to_tag<NativeValidatedMetadata>&, const json::value& value);
  struct NativeRoutingHop {
    std::string hopId;
    std::string authorization;
    std::string upstreamModel;
    std::string billingModel;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRoutingHop& value);
  NativeRoutingHop tag_invoke(const json::value_to_tag<NativeRoutingHop>&, const json::value& value);
  struct NativeTrustedRoutingDecisionWithoutIntent {
    std::string subjectId;
    std::string contextBinding;
    std::string sourceRevision;
    std::string cohort;
    std::string codex;
    std::vector<native_contracts::ApiKeyUsageProvider> providers;
    bool allowUnrosteredModel;
    std::vector<NativeRoutingHop> hops;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeTrustedRoutingDecisionWithoutIntent& value);
  NativeTrustedRoutingDecisionWithoutIntent tag_invoke(const json::value_to_tag<NativeTrustedRoutingDecisionWithoutIntent>&, const json::value& value);
  struct NativeTrustedRoutingDecision {
    std::string subjectId;
    std::string contextBinding;
    std::string intentDigest;
    std::string sourceRevision;
    std::string cohort;
    std::string codex;
    std::vector<native_contracts::ApiKeyUsageProvider> providers;
    bool allowUnrosteredModel;
    std::vector<NativeRoutingHop> hops;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeTrustedRoutingDecision& value);
  NativeTrustedRoutingDecision tag_invoke(const json::value_to_tag<NativeTrustedRoutingDecision>&, const json::value& value);
  struct NativeRuntimeAuthorization {
    NativeRuntimeBinding binding;
    VerifiedNativeRuntime verified;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeAuthorization& value);
  NativeRuntimeAuthorization tag_invoke(const json::value_to_tag<NativeRuntimeAuthorization>&, const json::value& value);
  struct NativeRuntimeRequestBinding {
    std::string handle;
    std::string runtimeBindingId;
    std::string requestId;
    std::string route;
    std::string path;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeRequestBinding& value);
  NativeRuntimeRequestBinding tag_invoke(const json::value_to_tag<NativeRuntimeRequestBinding>&, const json::value& value);
  struct NativeRetainedHistoryBinding {
    std::string kind;
    std::string grantId;
    std::string contextBinding;
    json::value privateContext;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRetainedHistoryBinding& value);
  NativeRetainedHistoryBinding tag_invoke(const json::value_to_tag<NativeRetainedHistoryBinding>&, const json::value& value);
  struct NativeRetainedHistoryScope {
    std::string namespace_;
    std::string originalBindingId;
    std::string originalPrincipalId;
    std::string keyId;
    std::vector<std::string> requestHandles;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRetainedHistoryScope& value);
  NativeRetainedHistoryScope tag_invoke(const json::value_to_tag<NativeRetainedHistoryScope>&, const json::value& value);
  struct NativeRetainedHistoryGrant {
    std::string id;
    double revision;
    std::string epoch;
    std::string subjectId;
    std::string contextBinding;
    std::vector<native_authority_contracts::NativeRole> roles;
    double expiresAtMs;
    bool revoked;
    std::string namespace_;
    std::string originalBindingId;
    std::string originalPrincipalId;
    std::string keyId;
    std::vector<std::string> requestHandles;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRetainedHistoryGrant& value);
  NativeRetainedHistoryGrant tag_invoke(const json::value_to_tag<NativeRetainedHistoryGrant>&, const json::value& value);
  struct VerifiedNativeRetainedHistory {
    std::string namespace_;
    std::string originalBindingId;
    std::string originalPrincipalId;
    std::string keyId;
    std::vector<std::string> requestHandles;
    std::string grantId;
    std::string epoch;
    std::string subjectId;
    std::string contextBinding;
    std::string intentDigest;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const VerifiedNativeRetainedHistory& value);
  VerifiedNativeRetainedHistory tag_invoke(const json::value_to_tag<VerifiedNativeRetainedHistory>&, const json::value& value);
  struct NativeValidatedRequest;
  struct TrustedNativeRuntimeVerifier {
    std::function<rt::Task<VerifiedNativeRuntime>(json::value, std::string, double)> verify;
    std::function<rt::Task<NativeTrustedRoutingDecision>(json::value, NativeValidatedRequest, std::string, double)> verifyRouting;
  };
  inline constexpr std::array<std::string_view, 7> NATIVE_RETAINED_METHODS = {"releaseApiUndispatched", "releasePaidBeforeFetch", "recordPaidAttemptLifecycle", "recordVerifiedPaidBilling", "deferPaidReconciliation", "claimPaidReconciliation", "finishPaidReconciliation"};
  struct NativeRetainedHistoryInitial {
    struct Verifier { std::function<rt::Task<VerifiedNativeRetainedHistory>(json::value, std::string, double)> verify; };
    std::vector<NativeRetainedHistoryGrant> grants;
    Verifier verifier;
  };
  struct NativeRuntimeInitial {
    struct SourceRecord { ::uos::types::ApiKeyRecord record; ::uos::types::ApiKeyHashRecord hashRecord; };
    std::vector<NativeRuntimeGrant> grants;
    TrustedNativeRuntimeVerifier verifier;
    std::vector<SourceRecord> records;
    std::optional<NativeRetainedHistoryInitial> retainedHistory{};
  };
}
#endif
#endif
