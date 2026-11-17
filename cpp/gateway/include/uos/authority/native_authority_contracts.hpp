// Port of src/authority/native-authority-contracts.ts.
#ifndef UOS_NATIVE_AUTHORITY_CONTRACTS_HPP
#define UOS_NATIVE_AUTHORITY_CONTRACTS_HPP
#include "uos/authority/native_api_key_audit.hpp"
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/authority/native_contracts.hpp"
#include "uos/authority/native_kernel_contracts.hpp"
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/auth/local_development_key.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/kv.hpp"
#include "uos/types.hpp"
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace uos::authority::native_authority_contracts {
  using NativeRole = std::string;
  using NativeSemantic = std::string;
  using NativeMethod = std::string;
  struct NativeKernelBinding {
    std::string owner;
    std::string repo;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKernelBinding& value);
  NativeKernelBinding tag_invoke(const json::value_to_tag<NativeKernelBinding>&, const json::value& value);
  struct NativeCapability {
    std::string id;
    std::string epoch;
    std::string principalId;
    std::optional<std::string> keyId;
    std::optional<std::string> tokenHash;
    std::optional<NativeKernelBinding> kernel;
    double runtimeRevision;
    std::string runtimeGrantId;
    std::string contextBinding;
    std::string subjectId;
    std::optional<std::string> principalRefId;
    std::optional<std::vector<std::string>> retainedRequestHandles;
    std::vector<NativeRole> roles;
    bool revoked;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCapability& value);
  NativeCapability tag_invoke(const json::value_to_tag<NativeCapability>&, const json::value& value);
  struct NativePaidPolicy {
    std::string policyVersion;
    double quotaPerCredit;
    json::object modelExposure;
    std::vector<std::string> eligibleModels;
    bool allowUnrosteredUnlimited;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePaidPolicy& value);
  NativePaidPolicy tag_invoke(const json::value_to_tag<NativePaidPolicy>&, const json::value& value);
  struct NativeKeyPolicy {
    native_contracts::ApiKeyPolicy api;
    NativePaidPolicy paid;
    bool revoked;
    std::optional<bool> rawSource{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKeyPolicy& value);
  NativeKeyPolicy tag_invoke(const json::value_to_tag<NativeKeyPolicy>&, const json::value& value);
  struct NativeOwner {
    std::string administrativeAccountLabelHash;
    std::string routingAccountIdHash;
    std::string credentialVersion;
    std::optional<std::string> nativeGenerationHash;
    std::string publicationRevision;
    std::string ownerId;
    std::string ownerEpoch;
    bool revoked;
    double issuedAtMs;
    double expiresAtMs;
    std::optional<std::string> nativeRefreshedAt;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeOwner& value);
  NativeOwner tag_invoke(const json::value_to_tag<NativeOwner>&, const json::value& value);
  struct NativePool {
    std::vector<native_codex_contracts::CodexRoutingIdentity> identities;
    std::vector<std::string> selectedAccountHashes;
    json::object policies;
    std::optional<json::object> modelPolicies{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePool& value);
  NativePool tag_invoke(const json::value_to_tag<NativePool>&, const json::value& value);
  struct NativeProviderHop {
    std::string id;
    std::string publicProvider;
    std::string ledgerProvider;
    std::string billingCorrelation;
    double microcreditsPerCredit;
    std::string billingSuffix;
    std::string authorization;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeProviderHop& value);
  NativeProviderHop tag_invoke(const json::value_to_tag<NativeProviderHop>&, const json::value& value);
  struct NativeResetProof {
    std::string id;
    native_codex_contracts::CodexRoutingAccount account;
    double quotaResetAtMs;
    double routingGeneration;
    std::optional<native_codex_contracts::CodexActiveAccountSelection> activeSnapshot;
    std::string poolRevision;
    std::optional<std::string> capacityRevision;
    bool verified;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeResetProof& value);
  NativeResetProof tag_invoke(const json::value_to_tag<NativeResetProof>&, const json::value& value);
  struct NativeCapacity {
    double v;
    std::vector<native_codex_contracts::CodexCapacityRoutingObservation> observations;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCapacity& value);
  NativeCapacity tag_invoke(const json::value_to_tag<NativeCapacity>&, const json::value& value);
  struct NativeAuthorityInitial {
    std::string namespace_;
    std::string epoch;
    std::vector<NativeKeyPolicy> keys;
    std::vector<native_kernel_contracts::KernelQuotaPolicyV2> kernelPolicies;
    native_kernel_contracts::KernelQuotaDefaults kernelDefaults;
    native_kernel_contracts::KernelQuotaConstants kernelConstants;
    NativePool pool;
    std::optional<native_codex_contracts::CodexAccountRoutingState> routing;
    std::optional<native_codex_contracts::CodexActiveAccountSelection> active;
    NativeCapacity capacity;
    std::vector<NativeOwner> owners;
    std::vector<NativeProviderHop> providerHops;
    std::vector<NativeResetProof> resetProofs;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeAuthorityInitial& value);
  NativeAuthorityInitial tag_invoke(const json::value_to_tag<NativeAuthorityInitial>&, const json::value& value);
  struct NativeRequest;
}
#include "uos/authority/native_principal_contracts.hpp"
#include "uos/authority/native_runtime_contracts.hpp"
namespace uos::authority::native_authority_contracts {
  struct NativeCodexAuthorization {
    native_codex_contracts::CodexRoutingAccount account;
    NativeOwner owner;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCodexAuthorization& value);
  NativeCodexAuthorization tag_invoke(const json::value_to_tag<NativeCodexAuthorization>&, const json::value& value);
  struct NativeRequestTransport {
    native_contracts::ApiKeyUsageProvider provider;
    std::optional<std::string> attemptHandle;
    std::string operationId;
    double startedAtMs;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRequestTransport& value);
  NativeRequestTransport tag_invoke(const json::value_to_tag<NativeRequestTransport>&, const json::value& value);
  struct NativeRequestResponding {
    native_contracts::ApiKeyUsageProvider provider;
    std::optional<std::string> attemptHandle;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRequestResponding& value);
  NativeRequestResponding tag_invoke(const json::value_to_tag<NativeRequestResponding>&, const json::value& value);
  struct NativeRequestDispatch {
    native_contracts::ApiKeyUsageProvider provider;
    bool cancellationOwned;
    bool fetchStarted;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRequestDispatch& value);
  NativeRequestDispatch tag_invoke(const json::value_to_tag<NativeRequestDispatch>&, const json::value& value);
  struct NativeRequest {
    std::string handle;
    std::string runtimeBindingId;
    std::string principalId;
    std::optional<std::string> keyId;
    std::string requestId;
    std::string route;
    std::string path;
    std::optional<std::string> model;
    std::optional<bool> stream;
    std::optional<std::string> reasoning;
    bool metadataBound;
    bool apiApplicable;
    bool kernelApplicable;
    double createdAtMs;
    std::optional<native_contracts::ApiKeyPolicy> api;
    std::optional<native_runtime_contracts::NativeTrustedRoutingDecisionWithoutIntent> routingDecision;
    std::optional<native_kernel_contracts::KernelQuotaReservationIdentity> kernel;
    std::optional<native_codex_contracts::CodexRoutingAccount> codex;
    std::optional<native_codex_contracts::CodexRouteSelection> selection;
    std::optional<native_codex_contracts::CodexRoutingRowRevisions> codexProof;
    std::optional<NativeOwner> ownerFence;
    std::optional<NativeSemantic> semantic;
    bool providerObserved;
    std::optional<std::string> delivery;
    std::optional<NativeRequestTransport> transport;
    std::optional<NativeRequestResponding> responding;
    std::optional<NativeRequestDispatch> dispatch;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRequest& value);
  NativeRequest tag_invoke(const json::value_to_tag<NativeRequest>&, const json::value& value);
  struct NativeAttempt {
    std::string handle;
    std::string runtimeBindingId;
    std::string requestHandle;
    std::string principalId;
    std::string keyId;
    std::string hopId;
    std::string billingRequestId;
    double windowResetAtMs;
    bool fetchStarted;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeAttempt& value);
  NativeAttempt tag_invoke(const json::value_to_tag<NativeAttempt>&, const json::value& value);
  struct NativeCursor {
    std::string id;
    std::string capabilityId;
    std::string family;
    std::string after;
    std::optional<std::string> parent;
    std::optional<std::string> lease;
    std::optional<std::string> retainedGrantId{};
    std::optional<std::vector<std::string>> retainedHandles{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCursor& value);
  NativeCursor tag_invoke(const json::value_to_tag<NativeCursor>&, const json::value& value);
  // The source only views replies as wire JSON; preserve undeclared reply fields.
  using NativeReply = json::object;
  struct NativeAuthorityConfig {
    std::vector<NativeProviderHop> providerHops;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeAuthorityConfig& value);
  NativeAuthorityConfig tag_invoke(const json::value_to_tag<NativeAuthorityConfig>&, const json::value& value);
  struct NativeOperationReceipt {
    std::string command;
    NativeReply reply;
    std::optional<NativeCodexAuthorization> codexAuthorization{};
    std::optional<std::string> retainedScope{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeOperationReceipt& value);
  NativeOperationReceipt tag_invoke(const json::value_to_tag<NativeOperationReceipt>&, const json::value& value);
  struct NativeCommand {
    std::string epoch;
    std::string operationId;
    NativeMethod method;
    json::object args;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCommand& value);
  NativeCommand tag_invoke(const json::value_to_tag<NativeCommand>&, const json::value& value);
  struct NativeCodexSignalResponse {
    double status;
    std::vector<std::vector<std::string>> headers;
    std::string body;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCodexSignalResponse& value);
  NativeCodexSignalResponse tag_invoke(const json::value_to_tag<NativeCodexSignalResponse>&, const json::value& value);
  struct NativePaidLifecyclePatch {
    rt::Nullable<std::string> provider_request_id{};
    std::optional<std::string> dispatch_state{};
    std::optional<std::string> terminal_state{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePaidLifecyclePatch& value);
  NativePaidLifecyclePatch tag_invoke(const json::value_to_tag<NativePaidLifecyclePatch>&, const json::value& value);
  struct NativeTerminalUsage {
    std::optional<double> input_tokens;
    std::optional<double> cached_input_tokens;
    std::optional<double> output_tokens;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeTerminalUsage& value);
  NativeTerminalUsage tag_invoke(const json::value_to_tag<NativeTerminalUsage>&, const json::value& value);
  namespace NativeArguments {
  struct openAuthenticatedRequest {
    std::string requestId;
    std::string route;
    std::string path;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const openAuthenticatedRequest& value);
  openAuthenticatedRequest tag_invoke(const json::value_to_tag<openAuthenticatedRequest>&, const json::value& value);
  struct reserveApiUsage {
    std::string handle;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const reserveApiUsage& value);
  reserveApiUsage tag_invoke(const json::value_to_tag<reserveApiUsage>&, const json::value& value);
  struct bindValidatedRequestMetadata {
    std::string handle;
    std::string model;
    bool stream;
    std::optional<std::string> reasoning;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const bindValidatedRequestMetadata& value);
  bindValidatedRequestMetadata tag_invoke(const json::value_to_tag<bindValidatedRequestMetadata>&, const json::value& value);
  struct recordTransportStarted {
    std::string handle;
    native_contracts::ApiKeyUsageProvider provider;
    std::optional<std::string> attemptHandle{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordTransportStarted& value);
  recordTransportStarted tag_invoke(const json::value_to_tag<recordTransportStarted>&, const json::value& value);
  struct markApiDispatch {
    std::string handle;
    native_contracts::ApiKeyUsageProvider provider;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const markApiDispatch& value);
  markApiDispatch tag_invoke(const json::value_to_tag<markApiDispatch>&, const json::value& value);
  struct releaseApiUndispatched {
    std::string handle;
    std::string reason;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const releaseApiUndispatched& value);
  releaseApiUndispatched tag_invoke(const json::value_to_tag<releaseApiUndispatched>&, const json::value& value);
  struct recordApiDispatchCancellation {
    std::string handle;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordApiDispatchCancellation& value);
  recordApiDispatchCancellation tag_invoke(const json::value_to_tag<recordApiDispatchCancellation>&, const json::value& value);
  struct reserveKernelUsage {
    std::string handle;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const reserveKernelUsage& value);
  reserveKernelUsage tag_invoke(const json::value_to_tag<reserveKernelUsage>&, const json::value& value);
  struct recordKernelTerminalIntent {
    std::string handle;
    std::string terminalState;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordKernelTerminalIntent& value);
  recordKernelTerminalIntent tag_invoke(const json::value_to_tag<recordKernelTerminalIntent>&, const json::value& value);
  struct settleKernelUsage {
    std::string handle;
    std::string terminalState;
    std::string reason;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const settleKernelUsage& value);
  settleKernelUsage tag_invoke(const json::value_to_tag<settleKernelUsage>&, const json::value& value);
  struct renewKernelReservation {
    std::string handle;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const renewKernelReservation& value);
  renewKernelReservation tag_invoke(const json::value_to_tag<renewKernelReservation>&, const json::value& value);
  struct reclaimExpiredKernelReservations {
    std::optional<std::string> cursor{};
    double limit;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const reclaimExpiredKernelReservations& value);
  reclaimExpiredKernelReservations tag_invoke(const json::value_to_tag<reclaimExpiredKernelReservations>&, const json::value& value);
  struct setKernelQuota {
    native_kernel_contracts::KernelQuotaScope scope;
    double usageLimitRequests;
    native_kernel_contracts::KernelQuotaPolicySetOptions options;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const setKernelQuota& value);
  setKernelQuota tag_invoke(const json::value_to_tag<setKernelQuota>&, const json::value& value);
  struct deleteKernelQuota {
    native_kernel_contracts::KernelQuotaScope scope;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const deleteKernelQuota& value);
  deleteKernelQuota tag_invoke(const json::value_to_tag<deleteKernelQuota>&, const json::value& value);
  struct acquireKernelDefaultCutover {
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const acquireKernelDefaultCutover& value);
  acquireKernelDefaultCutover tag_invoke(const json::value_to_tag<acquireKernelDefaultCutover>&, const json::value& value);
  struct selectCodexAccount {
    std::string handle;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const selectCodexAccount& value);
  selectCodexAccount tag_invoke(const json::value_to_tag<selectCodexAccount>&, const json::value& value);
  struct recordCodexAttemptSignal {
    std::string handle;
    std::string signal;
    std::optional<NativeCodexSignalResponse> response{};
    std::optional<bool> recoveryProbe{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordCodexAttemptSignal& value);
  recordCodexAttemptSignal tag_invoke(const json::value_to_tag<recordCodexAttemptSignal>&, const json::value& value);
  struct releaseCodexProbe {
    std::string handle;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const releaseCodexProbe& value);
  releaseCodexProbe tag_invoke(const json::value_to_tag<releaseCodexProbe>&, const json::value& value);
  struct electCodexResetProbe {
    std::string handle;
    std::string proofId;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const electCodexResetProbe& value);
  electCodexResetProbe tag_invoke(const json::value_to_tag<electCodexResetProbe>&, const json::value& value);
  struct refreshCodexAdmission {
    std::string handle;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const refreshCodexAdmission& value);
  refreshCodexAdmission tag_invoke(const json::value_to_tag<refreshCodexAdmission>&, const json::value& value);
  struct reservePaidAttempt {
    std::string handle;
    std::string hopId;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const reservePaidAttempt& value);
  reservePaidAttempt tag_invoke(const json::value_to_tag<reservePaidAttempt>&, const json::value& value);
  struct recordPaidAttemptLifecycle {
    std::string attemptHandle;
    NativePaidLifecyclePatch patch;
    std::optional<bool> fetchStarted{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordPaidAttemptLifecycle& value);
  recordPaidAttemptLifecycle tag_invoke(const json::value_to_tag<recordPaidAttemptLifecycle>&, const json::value& value);
  struct releasePaidBeforeFetch {
    std::string attemptHandle;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const releasePaidBeforeFetch& value);
  releasePaidBeforeFetch tag_invoke(const json::value_to_tag<releasePaidBeforeFetch>&, const json::value& value);
  struct recordVerifiedPaidBilling {
    std::string attemptHandle;
    native_paid_contracts::NativePaidProviderLog log;
    std::optional<double> surplusBuyerCostMicro{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordVerifiedPaidBilling& value);
  recordVerifiedPaidBilling tag_invoke(const json::value_to_tag<recordVerifiedPaidBilling>&, const json::value& value);
  struct deferPaidReconciliation {
    std::string attemptHandle;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const deferPaidReconciliation& value);
  deferPaidReconciliation tag_invoke(const json::value_to_tag<deferPaidReconciliation>&, const json::value& value);
  struct claimPaidReconciliation {
    std::optional<std::string> cursor{};
    double limit;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const claimPaidReconciliation& value);
  claimPaidReconciliation tag_invoke(const json::value_to_tag<claimPaidReconciliation>&, const json::value& value);
  struct finishPaidReconciliation {
    std::string lease;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const finishPaidReconciliation& value);
  finishPaidReconciliation tag_invoke(const json::value_to_tag<finishPaidReconciliation>&, const json::value& value);
  struct foldPaidBackfillPage {
    std::optional<std::string> cursor{};
    double limit;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const foldPaidBackfillPage& value);
  foldPaidBackfillPage tag_invoke(const json::value_to_tag<foldPaidBackfillPage>&, const json::value& value);
  struct beginPaidKeyDeletion {
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const beginPaidKeyDeletion& value);
  beginPaidKeyDeletion tag_invoke(const json::value_to_tag<beginPaidKeyDeletion>&, const json::value& value);
  struct recordSemanticTerminal {
    std::string handle;
    std::optional<std::string> attemptHandle{};
    NativeSemantic outcome;
    std::optional<NativeTerminalUsage> usage{};
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordSemanticTerminal& value);
  recordSemanticTerminal tag_invoke(const json::value_to_tag<recordSemanticTerminal>&, const json::value& value);
  struct recordDeliveryObservation {
    std::string handle;
    std::string observation;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordDeliveryObservation& value);
  recordDeliveryObservation tag_invoke(const json::value_to_tag<recordDeliveryObservation>&, const json::value& value);
  struct readRequestStatus {
    std::string handle;
    std::optional<std::string> cursor{};
    double limit;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const readRequestStatus& value);
  readRequestStatus tag_invoke(const json::value_to_tag<readRequestStatus>&, const json::value& value);
  }
  struct NativeRows {
    using retained_history_grant = native_runtime_contracts::NativeRetainedHistoryGrant;
    using runtime_grant = native_runtime_contracts::NativeRuntimeGrant;
    using runtime_request = native_runtime_contracts::NativeRuntimeRequestBinding;
    using key_control_grant = native_principal_contracts::NativeKeyControlGrant;
    using principal_resolution = native_principal_contracts::NativePrincipalResolution;
    using api_key_id = ::uos::types::ApiKeyRecord;
    using api_key_hash = ::uos::types::ApiKeyHashRecord;
    using api_key_audit = native_api_key_audit::ApiKeyChangeEvent;
    using key_control_receipt = native_principal_contracts::NativeControlReceipt;
    using kernel_default = double;
    using authority_config = NativeAuthorityConfig;
    using capability = NativeCapability;
    using api_policy = NativeKeyPolicy;
    using api_window = native_contracts::ApiKeyUsageWindowV3;
    using api_request = native_contracts::ApiKeyUsageRequestV3;
    using kernel_policy = native_kernel_contracts::KernelQuotaPolicyV2;
    using kernel_window = native_kernel_contracts::KernelQuotaWindowV2;
    using kernel_reservation = native_kernel_contracts::KernelQuotaReservationRowV2;
    using kernel_cutover = native_kernel_contracts::KernelDefaultWindowCutoverV2;
    using codex_pool = NativePool;
    using codex_routing = native_codex_contracts::CodexAccountRoutingState;
    using codex_active = native_codex_contracts::CodexActiveAccountSelection;
    using codex_capacity = NativeCapacity;
    using codex_owner = NativeOwner;
    using codex_reset = NativeResetProof;
    using paid_request = native_paid_contracts::PaidFallbackRequestV3;
    using paid_window = native_paid_contracts::PaidFallbackWindowV3;
    using paid_pending = native_paid_contracts::PaidFallbackPendingV3;
    using paid_gate = native_paid_contracts::PaidFallbackReconciliationGateV3;
    using paid_lease = native_paid_contracts::PaidFallbackReconciliationLeaseV3;
    using paid_backfill_lease = native_paid_contracts::PaidFallbackBackfillLeaseV3;
    using paid_rollup = native_paid_contracts::PaidFallbackUsageRollup;
    using paid_deletion_guard = std::variant<native_paid_contracts::PaidFallbackDeletionGuardV3, auth::local_development_key::LocalDevelopmentDeletionGuard>;
    using request_lifetime = NativeRequest;
    using attempt_lifetime = NativeAttempt;
    using cursor = NativeCursor;
    using operation_receipt = NativeOperationReceipt;
  };
  using NativeTable = std::string;
  using NativeKey = rt::KvKey;
  template<class T> struct NativeRead {
    std::optional<T> value;
    std::optional<std::string> revision;
    std::optional<std::string> versionstamp;
    std::optional<double> expiresAtMs;
  };
  struct NativeSqlStorage {
    using Binding = std::variant<std::string, double, std::nullptr_t>;
    struct Result { std::vector<json::object> rows; std::vector<json::object> toArray() const { return rows; } };
    struct Sql { std::function<Result(std::string, std::vector<Binding>)> exec; };
    Sql sql;
    std::function<void(std::function<void()>)> transaction;
    template<class Callback> auto transactionSync(Callback callback) const {
      using ResultType = std::invoke_result_t<Callback>;
      if constexpr (std::is_void_v<ResultType>) transaction(callback);
      else { std::optional<ResultType> result; transaction([&] { result = callback(); }); return std::move(result.value()); }
    }
  };
  class NativeAuthorityError : public rt::JsError {
  public:
    const std::string code;
    const double status;
    explicit NativeAuthorityError(std::string code_, double status_ = 409) : rt::JsError(code_), code(std::move(code_)), status(status_) {}
  };
  bool nativeObject(const json::value& value);
  bool nativeObject(const json::value* value);
  native_kernel_contracts::KernelQuotaIdentity nativeIdentity(const native_kernel_contracts::KernelQuotaScope& scope, const NativeCapability& capability);
  std::string nativeApiKeyId(const NativeCapability& capability);
  struct NativeApiKeyIdentity { std::optional<std::string> keyId; };
  std::string nativeApiKeyId(const NativeApiKeyIdentity& capability);
}
namespace uos::authority::native_runtime_contracts {
  struct NativeValidatedRequest : native_authority_contracts::NativeRequest { std::string model; bool stream; std::optional<std::string> reasoning; json::object rawJson{}; };
}
#endif
