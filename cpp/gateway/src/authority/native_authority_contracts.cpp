// Port of src/authority/native-authority-contracts.ts.
#include "uos/authority/native_authority_contracts.hpp"
#include "uos/rt/record.hpp"

namespace uos::authority::native_codex_contracts {
  namespace { using rt::record::Reader; using rt::record::Writer; }
  [[maybe_unused]] static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexActiveAccountSelection& value) { out = toJson(value); }
  static CodexActiveAccountSelection tag_invoke(const json::value_to_tag<CodexActiveAccountSelection>&, const json::value& value) {
    const auto parsed = parseCodexActiveAccountSelection(value); if (!parsed) throw rt::TypeError("Invalid CodexActiveAccountSelection"); return *parsed;
  }
  [[maybe_unused]] static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAccountRoutingState& value) { out = toJson(value); }
  static CodexAccountRoutingState tag_invoke(const json::value_to_tag<CodexAccountRoutingState>&, const json::value& value) {
    const auto parsed = parseCodexAccountRoutingState(value); if (!parsed) throw rt::TypeError("Invalid CodexAccountRoutingState"); return *parsed;
  }
  static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexRoutingIdentity& value) {
    out = Writer().required("slot", value.slot).required("accountIdHash", value.accountIdHash).required("credentialVersion", value.credentialVersion).done();
  }
  static CodexRoutingIdentity tag_invoke(const json::value_to_tag<CodexRoutingIdentity>&, const json::value& value) {
    const Reader in(value, "CodexRoutingIdentity");
    CodexRoutingIdentity result;
    result.slot = in.required<double>("slot");
    result.accountIdHash = in.required<std::string>("accountIdHash");
    result.credentialVersion = in.required<std::string>("credentialVersion");
    return result;
  }
  static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexRoutingAccount& value) {
    out = Writer().required("slot", value.slot).required("accountIdHash", value.accountIdHash).required("credentialVersion", value.credentialVersion).orNull("quotaHeadroom", value.quotaHeadroom).required("probeRequired", value.probeRequired).orNull("probeGeneration", value.probeGeneration).orNull("probeToken", value.probeToken).nullable("probeCircuit", value.probeCircuit).orUndefined("overageFallback", value.overageFallback).orUndefined("routingGeneration", value.routingGeneration).orUndefined("activeGeneration", value.activeGeneration).orUndefined("activePoolVersionstamp", value.activePoolVersionstamp).nullable("activeTransitionReason", value.activeTransitionReason).nullable("requestedModel", value.requestedModel).done();
  }
  static CodexRoutingAccount tag_invoke(const json::value_to_tag<CodexRoutingAccount>&, const json::value& value) {
    const Reader in(value, "CodexRoutingAccount");
    CodexRoutingAccount result;
    result.slot = in.required<double>("slot");
    result.accountIdHash = in.required<std::string>("accountIdHash");
    result.credentialVersion = in.required<std::string>("credentialVersion");
    result.quotaHeadroom = in.orNull<double>("quotaHeadroom");
    result.probeRequired = in.required<bool>("probeRequired");
    result.probeGeneration = in.orNull<double>("probeGeneration");
    result.probeToken = in.orNull<std::string>("probeToken");
    result.probeCircuit = in.nullable<std::string>("probeCircuit");
    result.overageFallback = in.orUndefined<bool>("overageFallback");
    result.routingGeneration = in.orUndefined<double>("routingGeneration");
    result.activeGeneration = in.orUndefined<double>("activeGeneration");
    result.activePoolVersionstamp = in.orUndefined<std::string>("activePoolVersionstamp");
    result.activeTransitionReason = in.nullable<std::string>("activeTransitionReason");
    result.requestedModel = in.nullable<std::string>("requestedModel");
    return result;
  }
  static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexBlockedAccountMetadata& value) {
    out = Writer().required("slot", value.slot).required("accountIdHash", value.accountIdHash).required("credentialVersion", value.credentialVersion).orNull("quotaHeadroom", value.quotaHeadroom).required("probeRequired", value.probeRequired).orNull("probeGeneration", value.probeGeneration).orNull("probeToken", value.probeToken).nullable("probeCircuit", value.probeCircuit).orUndefined("overageFallback", value.overageFallback).orUndefined("routingGeneration", value.routingGeneration).orUndefined("activeGeneration", value.activeGeneration).orUndefined("activePoolVersionstamp", value.activePoolVersionstamp).nullable("activeTransitionReason", value.activeTransitionReason).nullable("requestedModel", value.requestedModel).required("quotaResetAtMs", value.quotaResetAtMs).done();
  }
  static CodexBlockedAccountMetadata tag_invoke(const json::value_to_tag<CodexBlockedAccountMetadata>&, const json::value& value) {
    const Reader in(value, "CodexBlockedAccountMetadata");
    CodexBlockedAccountMetadata result;
    result.slot = in.required<double>("slot");
    result.accountIdHash = in.required<std::string>("accountIdHash");
    result.credentialVersion = in.required<std::string>("credentialVersion");
    result.quotaHeadroom = in.orNull<double>("quotaHeadroom");
    result.probeRequired = in.required<bool>("probeRequired");
    result.probeGeneration = in.orNull<double>("probeGeneration");
    result.probeToken = in.orNull<std::string>("probeToken");
    result.probeCircuit = in.nullable<std::string>("probeCircuit");
    result.overageFallback = in.orUndefined<bool>("overageFallback");
    result.routingGeneration = in.orUndefined<double>("routingGeneration");
    result.activeGeneration = in.orUndefined<double>("activeGeneration");
    result.activePoolVersionstamp = in.orUndefined<std::string>("activePoolVersionstamp");
    result.activeTransitionReason = in.nullable<std::string>("activeTransitionReason");
    result.requestedModel = in.nullable<std::string>("requestedModel");
    result.quotaResetAtMs = in.required<double>("quotaResetAtMs");
    return result;
  }
  static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexActiveAccountSnapshot& value) {
    out = Writer().orNull("selection", value.selection).done();
  }
  static CodexActiveAccountSnapshot tag_invoke(const json::value_to_tag<CodexActiveAccountSnapshot>&, const json::value& value) {
    const Reader in(value, "CodexActiveAccountSnapshot");
    CodexActiveAccountSnapshot result;
    result.selection = in.orNull<CodexActiveAccountSelection>("selection");
    return result;
  }
  static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexRoutingRowRevisions& value) {
    out = Writer().orNull("active", value.active).orNull("pool", value.pool).orNull("routing", value.routing).orNull("capacity", value.capacity).done();
  }
  static CodexRoutingRowRevisions tag_invoke(const json::value_to_tag<CodexRoutingRowRevisions>&, const json::value& value) {
    const Reader in(value, "CodexRoutingRowRevisions");
    CodexRoutingRowRevisions result;
    result.active = in.orNull<std::string>("active");
    result.pool = in.orNull<std::string>("pool");
    result.routing = in.orNull<std::string>("routing");
    result.capacity = in.orNull<std::string>("capacity");
    return result;
  }
  [[maybe_unused]] static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexCapacityRoutingWindow& value) {
    out = Writer().orNull("limit_window_seconds", value.limit_window_seconds).orNull("used_percent", value.used_percent).orNull("reset_at_ms", value.reset_at_ms).done();
  }
  static CodexCapacityRoutingWindow tag_invoke(const json::value_to_tag<CodexCapacityRoutingWindow>&, const json::value& value) {
    const Reader in(value, "CodexCapacityRoutingWindow");
    CodexCapacityRoutingWindow result;
    result.limit_window_seconds = in.orNull<double>("limit_window_seconds");
    result.used_percent = in.orNull<double>("used_percent");
    result.reset_at_ms = in.orNull<double>("reset_at_ms");
    return result;
  }
  [[maybe_unused]] static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexCapacityRoutingWindows& value) {
    out = Writer().orNull("primary", value.primary).orNull("secondary", value.secondary).done();
  }
  static CodexCapacityRoutingWindows tag_invoke(const json::value_to_tag<CodexCapacityRoutingWindows>&, const json::value& value) {
    const Reader in(value, "CodexCapacityRoutingWindows");
    CodexCapacityRoutingWindows result;
    result.primary = in.orNull<CodexCapacityRoutingWindow>("primary");
    result.secondary = in.orNull<CodexCapacityRoutingWindow>("secondary");
    return result;
  }
  [[maybe_unused]] static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexCapacityRoutingAdditionalRateLimit& value) {
    out = Writer().required("limit_name", value.limit_name).orNull("metered_feature", value.metered_feature).required("windows", value.windows).done();
  }
  static CodexCapacityRoutingAdditionalRateLimit tag_invoke(const json::value_to_tag<CodexCapacityRoutingAdditionalRateLimit>&, const json::value& value) {
    const Reader in(value, "CodexCapacityRoutingAdditionalRateLimit");
    CodexCapacityRoutingAdditionalRateLimit result;
    result.limit_name = in.required<std::string>("limit_name");
    result.metered_feature = in.orNull<std::string>("metered_feature");
    result.windows = in.required<CodexCapacityRoutingWindows>("windows");
    return result;
  }
  [[maybe_unused]] static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexCapacityRoutingObservation& value) {
    out = Writer().required("slot", value.slot).required("account_id_hash", value.account_id_hash).required("state", value.state).orNull("source_observed_at_ms", value.source_observed_at_ms).required("snapshot_at_ms", value.snapshot_at_ms).required("windows", value.windows).required("additional_rate_limits", value.additional_rate_limits).done();
  }
  static CodexCapacityRoutingObservation tag_invoke(const json::value_to_tag<CodexCapacityRoutingObservation>&, const json::value& value) {
    const Reader in(value, "CodexCapacityRoutingObservation");
    CodexCapacityRoutingObservation result;
    result.slot = in.required<double>("slot");
    result.account_id_hash = in.required<std::string>("account_id_hash");
    result.state = in.required<std::string>("state");
    result.source_observed_at_ms = in.orNull<double>("source_observed_at_ms");
    result.snapshot_at_ms = in.required<double>("snapshot_at_ms");
    result.windows = in.required<CodexCapacityRoutingWindows>("windows");
    result.additional_rate_limits = in.required<std::vector<CodexCapacityRoutingAdditionalRateLimit>>("additional_rate_limits");
    return result;
  }
  static void tag_invoke(const json::value_from_tag&, json::value& out, const CodexRouteSelection& value) {
    std::visit([&](const auto& item) {
      using T = std::decay_t<decltype(item)>;
      Writer writer; writer.required("kind", std::string(T::kind));
      if constexpr (std::is_same_v<T, CodexRouteSelectionEligible>) { writer.required("accounts", item.accounts); writer.required("skippedSlots", item.skippedSlots); writer.required("blockedAccounts", item.blockedAccounts); }
      if constexpr (std::is_same_v<T, CodexRouteSelectionQuotaBlocked>) { writer.required("skippedSlots", item.skippedSlots); writer.orNull("retryAtMs", item.retryAtMs); writer.required("blockedAccounts", item.blockedAccounts); writer.required("fullCohortExhausted", item.fullCohortExhausted); writer.orUndefined("fullCohortUnavailable", item.fullCohortUnavailable); writer.required("activeSnapshot", item.activeSnapshot); writer.orNull("poolSnapshotJson", item.poolSnapshotJson); writer.orNull("capacitySnapshotJson", item.capacitySnapshotJson); }
      if constexpr (std::is_same_v<T, CodexRouteSelectionUpstreamBlocked>) { writer.required("skippedSlots", item.skippedSlots); writer.orNull("retryAtMs", item.retryAtMs); writer.required("blockedAccounts", item.blockedAccounts); }
      if constexpr (std::is_same_v<T, CodexRouteSelectionModelUnavailable>) { writer.required("model", item.model); writer.orNull("detail", item.detail); writer.required("skippedSlots", item.skippedSlots); }
      if constexpr (std::is_same_v<T, CodexRouteSelectionCredentialsInvalid>) { writer.required("skippedSlots", item.skippedSlots); writer.orUndefined("fullCohortUnavailable", item.fullCohortUnavailable); }
      out = writer.done();
    }, value);
  }
  static CodexRouteSelection tag_invoke(const json::value_to_tag<CodexRouteSelection>&, const json::value& value) {
    const Reader in(value, "CodexRouteSelection");
    const auto kind = in.required<std::string>("kind");
    if (kind == "eligible") return CodexRouteSelectionEligible{.accounts = in.required<std::vector<CodexRoutingAccount>>("accounts"), .skippedSlots = in.required<std::vector<double>>("skippedSlots"), .blockedAccounts = in.required<std::vector<CodexBlockedAccountMetadata>>("blockedAccounts")};
    if (kind == "quota_blocked") return CodexRouteSelectionQuotaBlocked{.skippedSlots = in.required<std::vector<double>>("skippedSlots"), .retryAtMs = in.orNull<double>("retryAtMs"), .blockedAccounts = in.required<std::vector<CodexBlockedAccountMetadata>>("blockedAccounts"), .fullCohortExhausted = in.required<bool>("fullCohortExhausted"), .fullCohortUnavailable = in.orUndefined<bool>("fullCohortUnavailable"), .activeSnapshot = in.required<CodexActiveAccountSnapshot>("activeSnapshot"), .poolSnapshotJson = in.orNull<std::string>("poolSnapshotJson"), .capacitySnapshotJson = in.orNull<std::string>("capacitySnapshotJson")};
    if (kind == "upstream_blocked") return CodexRouteSelectionUpstreamBlocked{.skippedSlots = in.required<std::vector<double>>("skippedSlots"), .retryAtMs = in.orNull<double>("retryAtMs"), .blockedAccounts = in.required<std::vector<CodexBlockedAccountMetadata>>("blockedAccounts")};
    if (kind == "model_unavailable") return CodexRouteSelectionModelUnavailable{.model = in.required<std::string>("model"), .detail = in.orNull<std::string>("detail"), .skippedSlots = in.required<std::vector<double>>("skippedSlots")};
    if (kind == "credentials_invalid") return CodexRouteSelectionCredentialsInvalid{.skippedSlots = in.required<std::vector<double>>("skippedSlots"), .fullCohortUnavailable = in.orUndefined<bool>("fullCohortUnavailable")};
    if (kind == "routing_unavailable") return CodexRouteSelectionRoutingUnavailable{};
    in.fail("kind", rt::get(value, "kind"), "known discriminator");
  }
}
namespace uos::authority::native_kernel_contracts {
  namespace { using rt::record::Reader; using rt::record::Writer; }
  static void tag_invoke(const json::value_from_tag&, json::value& out, const KernelQuotaDefaults& value) {
    out = Writer().required("limit", value.limit).required("windowMs", value.windowMs).done();
  }
  static KernelQuotaDefaults tag_invoke(const json::value_to_tag<KernelQuotaDefaults>&, const json::value& value) {
    const Reader in(value, "KernelQuotaDefaults");
    KernelQuotaDefaults result;
    result.limit = in.required<double>("limit");
    result.windowMs = in.required<double>("windowMs");
    return result;
  }
  static void tag_invoke(const json::value_from_tag&, json::value& out, const KernelQuotaConstants& value) {
    out = Writer().required("noUsageLimit", value.noUsageLimit).required("noExpirationMs", value.noExpirationMs).done();
  }
  static KernelQuotaConstants tag_invoke(const json::value_to_tag<KernelQuotaConstants>&, const json::value& value) {
    const Reader in(value, "KernelQuotaConstants");
    KernelQuotaConstants result;
    result.noUsageLimit = in.required<double>("noUsageLimit");
    result.noExpirationMs = in.required<double>("noExpirationMs");
    return result;
  }
  static void tag_invoke(const json::value_from_tag&, json::value& out, const KernelQuotaReservationIdentity& value) {
    out = Writer().required("scope", value.scope).required("owner", value.owner).orUndefined("repo", value.repo).required("requestId", value.requestId).required("route", value.route).required("windowCreatedAtMs", value.windowCreatedAtMs).done();
  }
  static KernelQuotaReservationIdentity tag_invoke(const json::value_to_tag<KernelQuotaReservationIdentity>&, const json::value& value) {
    const Reader in(value, "KernelQuotaReservationIdentity");
    KernelQuotaReservationIdentity result;
    result.scope = in.required<KernelQuotaScope>("scope");
    result.owner = in.required<std::string>("owner");
    result.repo = in.orUndefined<std::string>("repo");
    result.requestId = in.required<std::string>("requestId");
    result.route = in.required<std::string>("route");
    result.windowCreatedAtMs = in.required<double>("windowCreatedAtMs");
    return result;
  }
  static void tag_invoke(const json::value_from_tag&, json::value& out, const KernelQuotaPolicySetOptions& value) {
    out = Writer().orUndefined("resetUsage", value.resetUsage).orUndefined("windowMs", value.windowMs).orUndefined("expiresAtMs", value.expiresAtMs).done();
  }
  static KernelQuotaPolicySetOptions tag_invoke(const json::value_to_tag<KernelQuotaPolicySetOptions>&, const json::value& value) {
    const Reader in(value, "KernelQuotaPolicySetOptions");
    KernelQuotaPolicySetOptions result;
    result.resetUsage = in.orUndefined<bool>("resetUsage");
    result.windowMs = in.orUndefined<double>("windowMs");
    result.expiresAtMs = in.orUndefined<double>("expiresAtMs");
    return result;
  }
}

namespace uos::authority::native_authority_contracts {
  namespace { using rt::record::Reader; using rt::record::Writer; }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKernelBinding& value) {
    (void)value;
    out = Writer().required("owner", value.owner).required("repo", value.repo).done();
  }
  NativeKernelBinding tag_invoke(const json::value_to_tag<NativeKernelBinding>&, const json::value& value) {
    const Reader in(value, "NativeKernelBinding");
    return {.owner = in.required<std::string>("owner"), .repo = in.required<std::string>("repo")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCapability& value) {
    (void)value;
    out = Writer().required("id", value.id).required("epoch", value.epoch).required("principalId", value.principalId).orNull("keyId", value.keyId).orNull("tokenHash", value.tokenHash).orNull("kernel", value.kernel).required("runtimeRevision", value.runtimeRevision).required("runtimeGrantId", value.runtimeGrantId).required("contextBinding", value.contextBinding).required("subjectId", value.subjectId).orNull("principalRefId", value.principalRefId).orNull("retainedRequestHandles", value.retainedRequestHandles).required("roles", value.roles).required("revoked", value.revoked).done();
  }
  NativeCapability tag_invoke(const json::value_to_tag<NativeCapability>&, const json::value& value) {
    const Reader in(value, "NativeCapability");
    return {.id = in.required<std::string>("id"), .epoch = in.required<std::string>("epoch"), .principalId = in.required<std::string>("principalId"), .keyId = in.orNull<std::string>("keyId"), .tokenHash = in.orNull<std::string>("tokenHash"), .kernel = in.orNull<NativeKernelBinding>("kernel"), .runtimeRevision = in.required<double>("runtimeRevision"), .runtimeGrantId = in.required<std::string>("runtimeGrantId"), .contextBinding = in.required<std::string>("contextBinding"), .subjectId = in.required<std::string>("subjectId"), .principalRefId = in.orNull<std::string>("principalRefId"), .retainedRequestHandles = in.orNull<std::vector<std::string>>("retainedRequestHandles"), .roles = in.required<std::vector<NativeRole>>("roles"), .revoked = in.required<bool>("revoked")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePaidPolicy& value) {
    (void)value;
    out = Writer().required("policyVersion", value.policyVersion).required("quotaPerCredit", value.quotaPerCredit).required("modelExposure", value.modelExposure).required("eligibleModels", value.eligibleModels).required("allowUnrosteredUnlimited", value.allowUnrosteredUnlimited).done();
  }
  NativePaidPolicy tag_invoke(const json::value_to_tag<NativePaidPolicy>&, const json::value& value) {
    const Reader in(value, "NativePaidPolicy");
    return {.policyVersion = in.required<std::string>("policyVersion"), .quotaPerCredit = in.required<double>("quotaPerCredit"), .modelExposure = in.required<json::object>("modelExposure"), .eligibleModels = in.required<std::vector<std::string>>("eligibleModels"), .allowUnrosteredUnlimited = in.required<bool>("allowUnrosteredUnlimited")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKeyPolicy& value) {
    (void)value;
    out = Writer().required("api", value.api).required("paid", value.paid).required("revoked", value.revoked).orUndefined("rawSource", value.rawSource).done();
  }
  NativeKeyPolicy tag_invoke(const json::value_to_tag<NativeKeyPolicy>&, const json::value& value) {
    const Reader in(value, "NativeKeyPolicy");
    return {.api = in.required<native_contracts::ApiKeyPolicy>("api"), .paid = in.required<NativePaidPolicy>("paid"), .revoked = in.required<bool>("revoked"), .rawSource = in.orUndefined<bool>("rawSource")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeOwner& value) {
    (void)value;
    out = Writer().required("administrativeAccountLabelHash", value.administrativeAccountLabelHash).required("routingAccountIdHash", value.routingAccountIdHash).required("credentialVersion", value.credentialVersion).orNull("nativeGenerationHash", value.nativeGenerationHash).required("publicationRevision", value.publicationRevision).required("ownerId", value.ownerId).required("ownerEpoch", value.ownerEpoch).required("revoked", value.revoked).required("issuedAtMs", value.issuedAtMs).required("expiresAtMs", value.expiresAtMs).orNull("nativeRefreshedAt", value.nativeRefreshedAt).done();
  }
  NativeOwner tag_invoke(const json::value_to_tag<NativeOwner>&, const json::value& value) {
    const Reader in(value, "NativeOwner");
    return {.administrativeAccountLabelHash = in.required<std::string>("administrativeAccountLabelHash"), .routingAccountIdHash = in.required<std::string>("routingAccountIdHash"), .credentialVersion = in.required<std::string>("credentialVersion"), .nativeGenerationHash = in.orNull<std::string>("nativeGenerationHash"), .publicationRevision = in.required<std::string>("publicationRevision"), .ownerId = in.required<std::string>("ownerId"), .ownerEpoch = in.required<std::string>("ownerEpoch"), .revoked = in.required<bool>("revoked"), .issuedAtMs = in.required<double>("issuedAtMs"), .expiresAtMs = in.required<double>("expiresAtMs"), .nativeRefreshedAt = in.orNull<std::string>("nativeRefreshedAt")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePool& value) {
    (void)value;
    out = Writer().required("identities", value.identities).required("selectedAccountHashes", value.selectedAccountHashes).required("policies", value.policies).orUndefined("modelPolicies", value.modelPolicies).done();
  }
  NativePool tag_invoke(const json::value_to_tag<NativePool>&, const json::value& value) {
    const Reader in(value, "NativePool");
    return {.identities = in.required<std::vector<native_codex_contracts::CodexRoutingIdentity>>("identities"), .selectedAccountHashes = in.required<std::vector<std::string>>("selectedAccountHashes"), .policies = in.required<json::object>("policies"), .modelPolicies = in.orUndefined<json::object>("modelPolicies")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeProviderHop& value) {
    (void)value;
    out = Writer().required("id", value.id).required("publicProvider", value.publicProvider).required("ledgerProvider", value.ledgerProvider).required("billingCorrelation", value.billingCorrelation).required("microcreditsPerCredit", value.microcreditsPerCredit).required("billingSuffix", value.billingSuffix).required("authorization", value.authorization).done();
  }
  NativeProviderHop tag_invoke(const json::value_to_tag<NativeProviderHop>&, const json::value& value) {
    const Reader in(value, "NativeProviderHop");
    return {.id = in.required<std::string>("id"), .publicProvider = in.required<std::string>("publicProvider"), .ledgerProvider = in.required<std::string>("ledgerProvider"), .billingCorrelation = in.required<std::string>("billingCorrelation"), .microcreditsPerCredit = in.required<double>("microcreditsPerCredit"), .billingSuffix = in.required<std::string>("billingSuffix"), .authorization = in.required<std::string>("authorization")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeResetProof& value) {
    (void)value;
    out = Writer().required("id", value.id).required("account", value.account).required("quotaResetAtMs", value.quotaResetAtMs).required("routingGeneration", value.routingGeneration).orNull("activeSnapshot", value.activeSnapshot).required("poolRevision", value.poolRevision).orNull("capacityRevision", value.capacityRevision).required("verified", value.verified).done();
  }
  NativeResetProof tag_invoke(const json::value_to_tag<NativeResetProof>&, const json::value& value) {
    const Reader in(value, "NativeResetProof");
    return {.id = in.required<std::string>("id"), .account = in.required<native_codex_contracts::CodexRoutingAccount>("account"), .quotaResetAtMs = in.required<double>("quotaResetAtMs"), .routingGeneration = in.required<double>("routingGeneration"), .activeSnapshot = in.orNull<native_codex_contracts::CodexActiveAccountSelection>("activeSnapshot"), .poolRevision = in.required<std::string>("poolRevision"), .capacityRevision = in.orNull<std::string>("capacityRevision"), .verified = in.required<bool>("verified")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCapacity& value) {
    (void)value;
    out = Writer().required("v", value.v).required("observations", value.observations).done();
  }
  NativeCapacity tag_invoke(const json::value_to_tag<NativeCapacity>&, const json::value& value) {
    const Reader in(value, "NativeCapacity");
    return {.v = in.required<double>("v"), .observations = in.required<std::vector<native_codex_contracts::CodexCapacityRoutingObservation>>("observations")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeAuthorityInitial& value) {
    (void)value;
    out = Writer().required("namespace", value.namespace_).required("epoch", value.epoch).required("keys", value.keys).required("kernelPolicies", value.kernelPolicies).required("kernelDefaults", value.kernelDefaults).required("kernelConstants", value.kernelConstants).required("pool", value.pool).orNull("routing", value.routing).orNull("active", value.active).required("capacity", value.capacity).required("owners", value.owners).required("providerHops", value.providerHops).required("resetProofs", value.resetProofs).done();
  }
  NativeAuthorityInitial tag_invoke(const json::value_to_tag<NativeAuthorityInitial>&, const json::value& value) {
    const Reader in(value, "NativeAuthorityInitial");
    return {.namespace_ = in.required<std::string>("namespace"), .epoch = in.required<std::string>("epoch"), .keys = in.required<std::vector<NativeKeyPolicy>>("keys"), .kernelPolicies = in.required<std::vector<native_kernel_contracts::KernelQuotaPolicyV2>>("kernelPolicies"), .kernelDefaults = in.required<native_kernel_contracts::KernelQuotaDefaults>("kernelDefaults"), .kernelConstants = in.required<native_kernel_contracts::KernelQuotaConstants>("kernelConstants"), .pool = in.required<NativePool>("pool"), .routing = in.orNull<native_codex_contracts::CodexAccountRoutingState>("routing"), .active = in.orNull<native_codex_contracts::CodexActiveAccountSelection>("active"), .capacity = in.required<NativeCapacity>("capacity"), .owners = in.required<std::vector<NativeOwner>>("owners"), .providerHops = in.required<std::vector<NativeProviderHop>>("providerHops"), .resetProofs = in.required<std::vector<NativeResetProof>>("resetProofs")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCodexAuthorization& value) {
    (void)value;
    out = Writer().required("account", value.account).required("owner", value.owner).done();
  }
  NativeCodexAuthorization tag_invoke(const json::value_to_tag<NativeCodexAuthorization>&, const json::value& value) {
    const Reader in(value, "NativeCodexAuthorization");
    return {.account = in.required<native_codex_contracts::CodexRoutingAccount>("account"), .owner = in.required<NativeOwner>("owner")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRequestTransport& value) {
    (void)value;
    out = Writer().required("provider", value.provider).orNull("attemptHandle", value.attemptHandle).required("operationId", value.operationId).required("startedAtMs", value.startedAtMs).done();
  }
  NativeRequestTransport tag_invoke(const json::value_to_tag<NativeRequestTransport>&, const json::value& value) {
    const Reader in(value, "NativeRequestTransport");
    return {.provider = in.required<native_contracts::ApiKeyUsageProvider>("provider"), .attemptHandle = in.orNull<std::string>("attemptHandle"), .operationId = in.required<std::string>("operationId"), .startedAtMs = in.required<double>("startedAtMs")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRequestResponding& value) {
    (void)value;
    out = Writer().required("provider", value.provider).orNull("attemptHandle", value.attemptHandle).done();
  }
  NativeRequestResponding tag_invoke(const json::value_to_tag<NativeRequestResponding>&, const json::value& value) {
    const Reader in(value, "NativeRequestResponding");
    return {.provider = in.required<native_contracts::ApiKeyUsageProvider>("provider"), .attemptHandle = in.orNull<std::string>("attemptHandle")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRequestDispatch& value) {
    (void)value;
    out = Writer().required("provider", value.provider).required("cancellationOwned", value.cancellationOwned).required("fetchStarted", value.fetchStarted).done();
  }
  NativeRequestDispatch tag_invoke(const json::value_to_tag<NativeRequestDispatch>&, const json::value& value) {
    const Reader in(value, "NativeRequestDispatch");
    return {.provider = in.required<native_contracts::ApiKeyUsageProvider>("provider"), .cancellationOwned = in.required<bool>("cancellationOwned"), .fetchStarted = in.required<bool>("fetchStarted")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRequest& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("runtimeBindingId", value.runtimeBindingId).required("principalId", value.principalId).orNull("keyId", value.keyId).required("requestId", value.requestId).required("route", value.route).required("path", value.path).orNull("model", value.model).orNull("stream", value.stream).orNull("reasoning", value.reasoning).required("metadataBound", value.metadataBound).required("apiApplicable", value.apiApplicable).required("kernelApplicable", value.kernelApplicable).required("createdAtMs", value.createdAtMs).orNull("api", value.api).orNull("routingDecision", value.routingDecision).orNull("kernel", value.kernel).orNull("codex", value.codex).orNull("selection", value.selection).orNull("codexProof", value.codexProof).orNull("ownerFence", value.ownerFence).orNull("semantic", value.semantic).required("providerObserved", value.providerObserved).orNull("delivery", value.delivery).orNull("transport", value.transport).orNull("responding", value.responding).orNull("dispatch", value.dispatch).done();
  }
  NativeRequest tag_invoke(const json::value_to_tag<NativeRequest>&, const json::value& value) {
    const Reader in(value, "NativeRequest");
    return {.handle = in.required<std::string>("handle"), .runtimeBindingId = in.required<std::string>("runtimeBindingId"), .principalId = in.required<std::string>("principalId"), .keyId = in.orNull<std::string>("keyId"), .requestId = in.required<std::string>("requestId"), .route = in.required<std::string>("route"), .path = in.required<std::string>("path"), .model = in.orNull<std::string>("model"), .stream = in.orNull<bool>("stream"), .reasoning = in.orNull<std::string>("reasoning"), .metadataBound = in.required<bool>("metadataBound"), .apiApplicable = in.required<bool>("apiApplicable"), .kernelApplicable = in.required<bool>("kernelApplicable"), .createdAtMs = in.required<double>("createdAtMs"), .api = in.orNull<native_contracts::ApiKeyPolicy>("api"), .routingDecision = in.orNull<native_runtime_contracts::NativeTrustedRoutingDecisionWithoutIntent>("routingDecision"), .kernel = in.orNull<native_kernel_contracts::KernelQuotaReservationIdentity>("kernel"), .codex = in.orNull<native_codex_contracts::CodexRoutingAccount>("codex"), .selection = in.orNull<native_codex_contracts::CodexRouteSelection>("selection"), .codexProof = in.orNull<native_codex_contracts::CodexRoutingRowRevisions>("codexProof"), .ownerFence = in.orNull<NativeOwner>("ownerFence"), .semantic = in.orNull<NativeSemantic>("semantic"), .providerObserved = in.required<bool>("providerObserved"), .delivery = in.orNull<std::string>("delivery"), .transport = in.orNull<NativeRequestTransport>("transport"), .responding = in.orNull<NativeRequestResponding>("responding"), .dispatch = in.orNull<NativeRequestDispatch>("dispatch")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeAttempt& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("runtimeBindingId", value.runtimeBindingId).required("requestHandle", value.requestHandle).required("principalId", value.principalId).required("keyId", value.keyId).required("hopId", value.hopId).required("billingRequestId", value.billingRequestId).required("windowResetAtMs", value.windowResetAtMs).required("fetchStarted", value.fetchStarted).done();
  }
  NativeAttempt tag_invoke(const json::value_to_tag<NativeAttempt>&, const json::value& value) {
    const Reader in(value, "NativeAttempt");
    return {.handle = in.required<std::string>("handle"), .runtimeBindingId = in.required<std::string>("runtimeBindingId"), .requestHandle = in.required<std::string>("requestHandle"), .principalId = in.required<std::string>("principalId"), .keyId = in.required<std::string>("keyId"), .hopId = in.required<std::string>("hopId"), .billingRequestId = in.required<std::string>("billingRequestId"), .windowResetAtMs = in.required<double>("windowResetAtMs"), .fetchStarted = in.required<bool>("fetchStarted")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCursor& value) {
    (void)value;
    out = Writer().required("id", value.id).required("capabilityId", value.capabilityId).required("family", value.family).required("after", value.after).orNull("parent", value.parent).orNull("lease", value.lease).orUndefined("retainedGrantId", value.retainedGrantId).orUndefined("retainedHandles", value.retainedHandles).done();
  }
  NativeCursor tag_invoke(const json::value_to_tag<NativeCursor>&, const json::value& value) {
    const Reader in(value, "NativeCursor");
    return {.id = in.required<std::string>("id"), .capabilityId = in.required<std::string>("capabilityId"), .family = in.required<std::string>("family"), .after = in.required<std::string>("after"), .parent = in.orNull<std::string>("parent"), .lease = in.orNull<std::string>("lease"), .retainedGrantId = in.orUndefined<std::string>("retainedGrantId"), .retainedHandles = in.orUndefined<std::vector<std::string>>("retainedHandles")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeAuthorityConfig& value) {
    (void)value;
    out = Writer().required("providerHops", value.providerHops).done();
  }
  NativeAuthorityConfig tag_invoke(const json::value_to_tag<NativeAuthorityConfig>&, const json::value& value) {
    const Reader in(value, "NativeAuthorityConfig");
    return {.providerHops = in.required<std::vector<NativeProviderHop>>("providerHops")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeOperationReceipt& value) {
    (void)value;
    out = Writer().required("command", value.command).required("reply", value.reply).orUndefined("codexAuthorization", value.codexAuthorization).orUndefined("retainedScope", value.retainedScope).done();
  }
  NativeOperationReceipt tag_invoke(const json::value_to_tag<NativeOperationReceipt>&, const json::value& value) {
    const Reader in(value, "NativeOperationReceipt");
    return {.command = in.required<std::string>("command"), .reply = in.required<NativeReply>("reply"), .codexAuthorization = in.orUndefined<NativeCodexAuthorization>("codexAuthorization"), .retainedScope = in.orUndefined<std::string>("retainedScope")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCommand& value) {
    (void)value;
    out = Writer().required("epoch", value.epoch).required("operationId", value.operationId).required("method", value.method).required("args", value.args).done();
  }
  NativeCommand tag_invoke(const json::value_to_tag<NativeCommand>&, const json::value& value) {
    const Reader in(value, "NativeCommand");
    return {.epoch = in.required<std::string>("epoch"), .operationId = in.required<std::string>("operationId"), .method = in.required<NativeMethod>("method"), .args = in.required<json::object>("args")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeCodexSignalResponse& value) {
    (void)value;
    out = Writer().required("status", value.status).required("headers", value.headers).required("body", value.body).done();
  }
  NativeCodexSignalResponse tag_invoke(const json::value_to_tag<NativeCodexSignalResponse>&, const json::value& value) {
    const Reader in(value, "NativeCodexSignalResponse");
    return {.status = in.required<double>("status"), .headers = in.required<std::vector<std::vector<std::string>>>("headers"), .body = in.required<std::string>("body")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePaidLifecyclePatch& value) {
    (void)value;
    out = Writer().nullable("provider_request_id", value.provider_request_id).orUndefined("dispatch_state", value.dispatch_state).orUndefined("terminal_state", value.terminal_state).done();
  }
  NativePaidLifecyclePatch tag_invoke(const json::value_to_tag<NativePaidLifecyclePatch>&, const json::value& value) {
    const Reader in(value, "NativePaidLifecyclePatch");
    return {.provider_request_id = in.nullable<std::string>("provider_request_id"), .dispatch_state = in.orUndefined<std::string>("dispatch_state"), .terminal_state = in.orUndefined<std::string>("terminal_state")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeTerminalUsage& value) {
    (void)value;
    out = Writer().orNull("input_tokens", value.input_tokens).orNull("cached_input_tokens", value.cached_input_tokens).orNull("output_tokens", value.output_tokens).done();
  }
  NativeTerminalUsage tag_invoke(const json::value_to_tag<NativeTerminalUsage>&, const json::value& value) {
    const Reader in(value, "NativeTerminalUsage");
    return {.input_tokens = in.orNull<double>("input_tokens"), .cached_input_tokens = in.orNull<double>("cached_input_tokens"), .output_tokens = in.orNull<double>("output_tokens")};
  }
  namespace NativeArguments {
  void tag_invoke(const json::value_from_tag&, json::value& out, const openAuthenticatedRequest& value) {
    (void)value;
    out = Writer().required("requestId", value.requestId).required("route", value.route).required("path", value.path).done();
  }
  openAuthenticatedRequest tag_invoke(const json::value_to_tag<openAuthenticatedRequest>&, const json::value& value) {
    const Reader in(value, "openAuthenticatedRequest");
    return {.requestId = in.required<std::string>("requestId"), .route = in.required<std::string>("route"), .path = in.required<std::string>("path")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const reserveApiUsage& value) {
    (void)value;
    out = Writer().required("handle", value.handle).done();
  }
  reserveApiUsage tag_invoke(const json::value_to_tag<reserveApiUsage>&, const json::value& value) {
    const Reader in(value, "reserveApiUsage");
    return {.handle = in.required<std::string>("handle")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const bindValidatedRequestMetadata& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("model", value.model).required("stream", value.stream).orNull("reasoning", value.reasoning).done();
  }
  bindValidatedRequestMetadata tag_invoke(const json::value_to_tag<bindValidatedRequestMetadata>&, const json::value& value) {
    const Reader in(value, "bindValidatedRequestMetadata");
    return {.handle = in.required<std::string>("handle"), .model = in.required<std::string>("model"), .stream = in.required<bool>("stream"), .reasoning = in.orNull<std::string>("reasoning")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordTransportStarted& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("provider", value.provider).orUndefined("attemptHandle", value.attemptHandle).done();
  }
  recordTransportStarted tag_invoke(const json::value_to_tag<recordTransportStarted>&, const json::value& value) {
    const Reader in(value, "recordTransportStarted");
    return {.handle = in.required<std::string>("handle"), .provider = in.required<native_contracts::ApiKeyUsageProvider>("provider"), .attemptHandle = in.orUndefined<std::string>("attemptHandle")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const markApiDispatch& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("provider", value.provider).done();
  }
  markApiDispatch tag_invoke(const json::value_to_tag<markApiDispatch>&, const json::value& value) {
    const Reader in(value, "markApiDispatch");
    return {.handle = in.required<std::string>("handle"), .provider = in.required<native_contracts::ApiKeyUsageProvider>("provider")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const releaseApiUndispatched& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("reason", value.reason).done();
  }
  releaseApiUndispatched tag_invoke(const json::value_to_tag<releaseApiUndispatched>&, const json::value& value) {
    const Reader in(value, "releaseApiUndispatched");
    return {.handle = in.required<std::string>("handle"), .reason = in.required<std::string>("reason")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordApiDispatchCancellation& value) {
    (void)value;
    out = Writer().required("handle", value.handle).done();
  }
  recordApiDispatchCancellation tag_invoke(const json::value_to_tag<recordApiDispatchCancellation>&, const json::value& value) {
    const Reader in(value, "recordApiDispatchCancellation");
    return {.handle = in.required<std::string>("handle")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const reserveKernelUsage& value) {
    (void)value;
    out = Writer().required("handle", value.handle).done();
  }
  reserveKernelUsage tag_invoke(const json::value_to_tag<reserveKernelUsage>&, const json::value& value) {
    const Reader in(value, "reserveKernelUsage");
    return {.handle = in.required<std::string>("handle")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordKernelTerminalIntent& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("terminalState", value.terminalState).done();
  }
  recordKernelTerminalIntent tag_invoke(const json::value_to_tag<recordKernelTerminalIntent>&, const json::value& value) {
    const Reader in(value, "recordKernelTerminalIntent");
    return {.handle = in.required<std::string>("handle"), .terminalState = in.required<std::string>("terminalState")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const settleKernelUsage& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("terminalState", value.terminalState).required("reason", value.reason).done();
  }
  settleKernelUsage tag_invoke(const json::value_to_tag<settleKernelUsage>&, const json::value& value) {
    const Reader in(value, "settleKernelUsage");
    return {.handle = in.required<std::string>("handle"), .terminalState = in.required<std::string>("terminalState"), .reason = in.required<std::string>("reason")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const renewKernelReservation& value) {
    (void)value;
    out = Writer().required("handle", value.handle).done();
  }
  renewKernelReservation tag_invoke(const json::value_to_tag<renewKernelReservation>&, const json::value& value) {
    const Reader in(value, "renewKernelReservation");
    return {.handle = in.required<std::string>("handle")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const reclaimExpiredKernelReservations& value) {
    (void)value;
    out = Writer().orUndefined("cursor", value.cursor).required("limit", value.limit).done();
  }
  reclaimExpiredKernelReservations tag_invoke(const json::value_to_tag<reclaimExpiredKernelReservations>&, const json::value& value) {
    const Reader in(value, "reclaimExpiredKernelReservations");
    return {.cursor = in.orUndefined<std::string>("cursor"), .limit = in.required<double>("limit")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const setKernelQuota& value) {
    (void)value;
    out = Writer().required("scope", value.scope).required("usageLimitRequests", value.usageLimitRequests).required("options", value.options).done();
  }
  setKernelQuota tag_invoke(const json::value_to_tag<setKernelQuota>&, const json::value& value) {
    const Reader in(value, "setKernelQuota");
    return {.scope = in.required<native_kernel_contracts::KernelQuotaScope>("scope"), .usageLimitRequests = in.required<double>("usageLimitRequests"), .options = in.required<native_kernel_contracts::KernelQuotaPolicySetOptions>("options")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const deleteKernelQuota& value) {
    (void)value;
    out = Writer().required("scope", value.scope).done();
  }
  deleteKernelQuota tag_invoke(const json::value_to_tag<deleteKernelQuota>&, const json::value& value) {
    const Reader in(value, "deleteKernelQuota");
    return {.scope = in.required<native_kernel_contracts::KernelQuotaScope>("scope")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const acquireKernelDefaultCutover& value) {
    (void)value;
    out = Writer().done();
  }
  acquireKernelDefaultCutover tag_invoke(const json::value_to_tag<acquireKernelDefaultCutover>&, const json::value& value) {
    const Reader in(value, "acquireKernelDefaultCutover");
    return {};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const selectCodexAccount& value) {
    (void)value;
    out = Writer().required("handle", value.handle).done();
  }
  selectCodexAccount tag_invoke(const json::value_to_tag<selectCodexAccount>&, const json::value& value) {
    const Reader in(value, "selectCodexAccount");
    return {.handle = in.required<std::string>("handle")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordCodexAttemptSignal& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("signal", value.signal).orUndefined("response", value.response).orUndefined("recoveryProbe", value.recoveryProbe).done();
  }
  recordCodexAttemptSignal tag_invoke(const json::value_to_tag<recordCodexAttemptSignal>&, const json::value& value) {
    const Reader in(value, "recordCodexAttemptSignal");
    return {.handle = in.required<std::string>("handle"), .signal = in.required<std::string>("signal"), .response = in.orUndefined<NativeCodexSignalResponse>("response"), .recoveryProbe = in.orUndefined<bool>("recoveryProbe")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const releaseCodexProbe& value) {
    (void)value;
    out = Writer().required("handle", value.handle).done();
  }
  releaseCodexProbe tag_invoke(const json::value_to_tag<releaseCodexProbe>&, const json::value& value) {
    const Reader in(value, "releaseCodexProbe");
    return {.handle = in.required<std::string>("handle")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const electCodexResetProbe& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("proofId", value.proofId).done();
  }
  electCodexResetProbe tag_invoke(const json::value_to_tag<electCodexResetProbe>&, const json::value& value) {
    const Reader in(value, "electCodexResetProbe");
    return {.handle = in.required<std::string>("handle"), .proofId = in.required<std::string>("proofId")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const refreshCodexAdmission& value) {
    (void)value;
    out = Writer().required("handle", value.handle).done();
  }
  refreshCodexAdmission tag_invoke(const json::value_to_tag<refreshCodexAdmission>&, const json::value& value) {
    const Reader in(value, "refreshCodexAdmission");
    return {.handle = in.required<std::string>("handle")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const reservePaidAttempt& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("hopId", value.hopId).done();
  }
  reservePaidAttempt tag_invoke(const json::value_to_tag<reservePaidAttempt>&, const json::value& value) {
    const Reader in(value, "reservePaidAttempt");
    return {.handle = in.required<std::string>("handle"), .hopId = in.required<std::string>("hopId")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordPaidAttemptLifecycle& value) {
    (void)value;
    out = Writer().required("attemptHandle", value.attemptHandle).required("patch", value.patch).orUndefined("fetchStarted", value.fetchStarted).done();
  }
  recordPaidAttemptLifecycle tag_invoke(const json::value_to_tag<recordPaidAttemptLifecycle>&, const json::value& value) {
    const Reader in(value, "recordPaidAttemptLifecycle");
    return {.attemptHandle = in.required<std::string>("attemptHandle"), .patch = in.required<NativePaidLifecyclePatch>("patch"), .fetchStarted = in.orUndefined<bool>("fetchStarted")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const releasePaidBeforeFetch& value) {
    (void)value;
    out = Writer().required("attemptHandle", value.attemptHandle).done();
  }
  releasePaidBeforeFetch tag_invoke(const json::value_to_tag<releasePaidBeforeFetch>&, const json::value& value) {
    const Reader in(value, "releasePaidBeforeFetch");
    return {.attemptHandle = in.required<std::string>("attemptHandle")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordVerifiedPaidBilling& value) {
    (void)value;
    out = Writer().required("attemptHandle", value.attemptHandle).required("log", value.log).orUndefined("surplusBuyerCostMicro", value.surplusBuyerCostMicro).done();
  }
  recordVerifiedPaidBilling tag_invoke(const json::value_to_tag<recordVerifiedPaidBilling>&, const json::value& value) {
    const Reader in(value, "recordVerifiedPaidBilling");
    return {.attemptHandle = in.required<std::string>("attemptHandle"), .log = in.required<native_paid_contracts::NativePaidProviderLog>("log"), .surplusBuyerCostMicro = in.orUndefined<double>("surplusBuyerCostMicro")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const deferPaidReconciliation& value) {
    (void)value;
    out = Writer().required("attemptHandle", value.attemptHandle).done();
  }
  deferPaidReconciliation tag_invoke(const json::value_to_tag<deferPaidReconciliation>&, const json::value& value) {
    const Reader in(value, "deferPaidReconciliation");
    return {.attemptHandle = in.required<std::string>("attemptHandle")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const claimPaidReconciliation& value) {
    (void)value;
    out = Writer().orUndefined("cursor", value.cursor).required("limit", value.limit).done();
  }
  claimPaidReconciliation tag_invoke(const json::value_to_tag<claimPaidReconciliation>&, const json::value& value) {
    const Reader in(value, "claimPaidReconciliation");
    return {.cursor = in.orUndefined<std::string>("cursor"), .limit = in.required<double>("limit")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const finishPaidReconciliation& value) {
    (void)value;
    out = Writer().required("lease", value.lease).done();
  }
  finishPaidReconciliation tag_invoke(const json::value_to_tag<finishPaidReconciliation>&, const json::value& value) {
    const Reader in(value, "finishPaidReconciliation");
    return {.lease = in.required<std::string>("lease")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const foldPaidBackfillPage& value) {
    (void)value;
    out = Writer().orUndefined("cursor", value.cursor).required("limit", value.limit).done();
  }
  foldPaidBackfillPage tag_invoke(const json::value_to_tag<foldPaidBackfillPage>&, const json::value& value) {
    const Reader in(value, "foldPaidBackfillPage");
    return {.cursor = in.orUndefined<std::string>("cursor"), .limit = in.required<double>("limit")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const beginPaidKeyDeletion& value) {
    (void)value;
    out = Writer().done();
  }
  beginPaidKeyDeletion tag_invoke(const json::value_to_tag<beginPaidKeyDeletion>&, const json::value& value) {
    const Reader in(value, "beginPaidKeyDeletion");
    return {};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordSemanticTerminal& value) {
    (void)value;
    out = Writer().required("handle", value.handle).orUndefined("attemptHandle", value.attemptHandle).required("outcome", value.outcome).orUndefined("usage", value.usage).done();
  }
  recordSemanticTerminal tag_invoke(const json::value_to_tag<recordSemanticTerminal>&, const json::value& value) {
    const Reader in(value, "recordSemanticTerminal");
    return {.handle = in.required<std::string>("handle"), .attemptHandle = in.orUndefined<std::string>("attemptHandle"), .outcome = in.required<NativeSemantic>("outcome"), .usage = in.orUndefined<NativeTerminalUsage>("usage")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const recordDeliveryObservation& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("observation", value.observation).done();
  }
  recordDeliveryObservation tag_invoke(const json::value_to_tag<recordDeliveryObservation>&, const json::value& value) {
    const Reader in(value, "recordDeliveryObservation");
    return {.handle = in.required<std::string>("handle"), .observation = in.required<std::string>("observation")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const readRequestStatus& value) {
    (void)value;
    out = Writer().required("handle", value.handle).orUndefined("cursor", value.cursor).required("limit", value.limit).done();
  }
  readRequestStatus tag_invoke(const json::value_to_tag<readRequestStatus>&, const json::value& value) {
    const Reader in(value, "readRequestStatus");
    return {.handle = in.required<std::string>("handle"), .cursor = in.orUndefined<std::string>("cursor"), .limit = in.required<double>("limit")};
  }
  }
  bool nativeObject(const json::value& value) { return rt::isRecord(value); }
  bool nativeObject(const json::value* value) { return rt::isRecord(value); }
  native_kernel_contracts::KernelQuotaIdentity nativeIdentity(const native_kernel_contracts::KernelQuotaScope& scope, const NativeCapability& capability) {
    if (!capability.kernel) throw NativeAuthorityError("kernel_scope_not_applicable", 403);
    return {.scope = scope, .owner = capability.kernel->owner, .repo = scope == "repo" ? std::optional(capability.kernel->repo) : std::nullopt};
  }
  std::string nativeApiKeyId(const NativeCapability& capability) { return nativeApiKeyId(NativeApiKeyIdentity{capability.keyId}); }
  std::string nativeApiKeyId(const NativeApiKeyIdentity& capability) {
    if (!capability.keyId) throw NativeAuthorityError("api_scope_not_applicable", 403);
    return *capability.keyId;
  }
}
