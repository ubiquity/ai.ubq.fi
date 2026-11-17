// Port of src/authority/native-principal-contracts.ts.
#ifndef UOS_NATIVE_PRINCIPAL_CONTRACTS_HPP
#include "uos/authority/native_authority_contracts.hpp"
#ifndef UOS_NATIVE_PRINCIPAL_CONTRACTS_HPP
#define UOS_NATIVE_PRINCIPAL_CONTRACTS_HPP
#include "uos/paid_fallback/index.hpp"
namespace uos::authority::native_principal_contracts {
  using NativeControlRole = std::string;
  using NativeControlMethod = std::string;
  struct NativeKeyIntent {
    std::optional<json::value> name{};
    std::optional<json::value> expires_at_ms{};
    std::optional<json::value> usage_limit_requests{};
    std::optional<json::value> window_ms{};
    std::optional<json::value> paid_fallback_enabled{};
    std::optional<json::value> paid_fallback_limit_credits{};
    std::optional<json::value> banked_resets_enabled{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKeyIntent& value);
  NativeKeyIntent tag_invoke(const json::value_to_tag<NativeKeyIntent>&, const json::value& value);
  struct NativeKeyUpdateIntent {
    std::optional<json::value> name{};
    std::optional<json::value> expires_at_ms{};
    std::optional<json::value> usage_limit_requests{};
    std::optional<json::value> window_ms{};
    std::optional<json::value> paid_fallback_enabled{};
    std::optional<json::value> paid_fallback_limit_credits{};
    std::optional<json::value> banked_resets_enabled{};
    std::optional<json::value> reset_usage{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKeyUpdateIntent& value);
  NativeKeyUpdateIntent tag_invoke(const json::value_to_tag<NativeKeyUpdateIntent>&, const json::value& value);
  struct NativeControlCommand {
    std::string epoch;
    std::string operationId;
    NativeControlMethod method;
    json::object args;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlCommand& value);
  NativeControlCommand tag_invoke(const json::value_to_tag<NativeControlCommand>&, const json::value& value);
  struct NativeKeyControlGrant {
    std::string id;
    std::string epoch;
    std::string subjectId;
    std::string contextBinding;
    std::vector<NativeControlRole> roles;
    double expiresAtMs;
    bool revoked;
    std::optional<std::vector<std::string>> targetKeyIds;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKeyControlGrant& value);
  NativeKeyControlGrant tag_invoke(const json::value_to_tag<NativeKeyControlGrant>&, const json::value& value);
  struct NativePrincipalReference {
    std::string id;
    std::string principalId;
    std::string keyId;
    std::string epoch;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePrincipalReference& value);
  NativePrincipalReference tag_invoke(const json::value_to_tag<NativePrincipalReference>&, const json::value& value);
  struct NativePrincipalResolution {
    std::string id;
    std::string principalId;
    std::string keyId;
    std::string epoch;
    std::string tokenHash;
    std::string hashRevision;
    std::string grantId;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePrincipalResolution& value);
  NativePrincipalResolution tag_invoke(const json::value_to_tag<NativePrincipalResolution>&, const json::value& value);
  struct NativeKeyCreatePreparation {
    ::uos::types::ApiKeyRecord record;
    native_authority_contracts::NativeKeyPolicy projection;
    double atMs;
    std::optional<std::string> hashRevision;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKeyCreatePreparation& value);
  NativeKeyCreatePreparation tag_invoke(const json::value_to_tag<NativeKeyCreatePreparation>&, const json::value& value);
  struct NativeControlCleanup {
    std::string keyId;
    std::optional<std::string> localOwner;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlCleanup& value);
  NativeControlCleanup tag_invoke(const json::value_to_tag<NativeControlCleanup>&, const json::value& value);
  struct NativeControlOutcomePrincipal {
    static constexpr std::string_view kind = "principal";
    NativePrincipalReference principalRef;
  };
  struct NativeControlOutcomeApplied {
    static constexpr std::string_view kind = "applied";
    std::string keyId;
    std::optional<json::object> detail{};
  };
  struct NativeControlOutcomeUnchanged {
    static constexpr std::string_view kind = "unchanged";
    std::string keyId;
    std::optional<json::object> detail{};
  };
  struct NativeControlOutcomeRefused {
    static constexpr std::string_view kind = "refused";
    double status;
    std::string code;
    std::string message;
  };
  using NativeControlOutcome = std::variant<NativeControlOutcomePrincipal, NativeControlOutcomeApplied, NativeControlOutcomeUnchanged, NativeControlOutcomeRefused>;
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlOutcome& value);
  NativeControlOutcome tag_invoke(const json::value_to_tag<NativeControlOutcome>&, const json::value& value);
  struct NativeControlReplyHistoricalAck {
    static constexpr std::string_view kind = "historical_ack";
    NativeControlOutcome outcome;
  };
  using NativeControlReply = std::variant<NativeControlOutcomePrincipal, NativeControlOutcomeApplied, NativeControlOutcomeUnchanged, NativeControlOutcomeRefused, NativeControlReplyHistoricalAck>;
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlReply& value);
  NativeControlReply tag_invoke(const json::value_to_tag<NativeControlReply>&, const json::value& value);
  struct NativeControlReceipt {
    std::string intent;
    std::optional<std::string> targetKeyId;
    std::optional<NativeControlOutcome> reply;
    std::optional<NativeKeyCreatePreparation> prepared{};
    std::optional<NativeControlCleanup> cleanup;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlReceipt& value);
  NativeControlReceipt tag_invoke(const json::value_to_tag<NativeControlReceipt>&, const json::value& value);
  struct NativeControlBinding {
    std::string grantId;
    std::string contextBinding;
    json::value privateContext;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlBinding& value);
  NativeControlBinding tag_invoke(const json::value_to_tag<NativeControlBinding>&, const json::value& value);
  namespace NativeControlArguments {
  struct resolvePrincipal {
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const resolvePrincipal& value);
  resolvePrincipal tag_invoke(const json::value_to_tag<resolvePrincipal>&, const json::value& value);
  struct createKey {
    NativeKeyIntent input;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const createKey& value);
  createKey tag_invoke(const json::value_to_tag<createKey>&, const json::value& value);
  struct updateKey {
    NativeKeyUpdateIntent input;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const updateKey& value);
  updateKey tag_invoke(const json::value_to_tag<updateKey>&, const json::value& value);
  struct revokeKey {
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const revokeKey& value);
  revokeKey tag_invoke(const json::value_to_tag<revokeKey>&, const json::value& value);
  struct unrevokeKey {
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const unrevokeKey& value);
  unrevokeKey tag_invoke(const json::value_to_tag<unrevokeKey>&, const json::value& value);
  struct deleteKey {
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const deleteKey& value);
  deleteKey tag_invoke(const json::value_to_tag<deleteKey>&, const json::value& value);
  }
  struct VerifiedKeyAuth { static constexpr std::string_view kind = "principal_resolver"; std::string subjectId; std::string contextBinding; std::string intentDigest; std::string principalId; std::string tokenHash; };
  struct VerifiedKeyAdmin { static constexpr std::string_view kind = "key_admin"; std::string subjectId; std::string contextBinding; std::string intentDigest; std::optional<std::string> targetKeyId; native_api_key_audit::ApiKeyAuditContext auditContext; rt::Nullable<std::string> createToken{}; };
  using VerifiedControlIdentity = std::variant<VerifiedKeyAuth, VerifiedKeyAdmin>;
  struct TrustedPrincipalVerifier { std::function<rt::Task<VerifiedControlIdentity>(json::value, std::string, double)> verify; };
  struct NativePrincipalControlInitial {
    struct SourceRecord { ::uos::types::ApiKeyRecord record; ::uos::types::ApiKeyHashRecord hashRecord; };
    struct Sources {
      struct Defaults { double expiryMs; double usageLimitRequests; };
      std::string localDevelopmentKeyId;
      std::function<Defaults(double)> defaults;
      std::function<paid_fallback::index::PaidFallbackPolicyFields()> defaultPaidPolicy;
      std::function<std::string()> generateToken;
      std::function<rt::Task<std::string>(std::string)> hashToken;
      std::function<std::string()> randomUUID;
      native_api_key_audit::ApiKeyAuditSources audit;
      std::function<rt::Task<paid_fallback::index::InitializePaidFallbackPolicyResult>()> initializePricing;
      std::function<NativeControlOutcomeRefused(rt::Reason)> pricingFailure;
      std::function<std::optional<bool>(VerifiedKeyAdmin, ::uos::types::ApiKeyRecord)> allowUnrosteredUnlimited{};
      std::function<rt::Task<void>(std::string)> cleanupApiLogs;
    };
    std::vector<NativeKeyControlGrant> grants;
    std::vector<SourceRecord> records;
    TrustedPrincipalVerifier verifier;
    Sources sources;
  };
  struct NativePrincipalControlRunner { std::function<rt::Task<NativeControlReply>(NativeControlBinding, NativeControlCommand)> control; };
}
#endif
#endif
