// Port of src/authority/native-runtime-contracts.ts.
#include "uos/authority/native_runtime_contracts.hpp"
#include "uos/rt/record.hpp"

namespace uos::authority::native_runtime_contracts {
  namespace { using rt::record::Reader; using rt::record::Writer; }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeGrant& value) {
    (void)value;
    out = Writer().required("id", value.id).required("revision", value.revision).required("epoch", value.epoch).required("subjectId", value.subjectId).required("contextBinding", value.contextBinding).required("roles", value.roles).required("expiresAtMs", value.expiresAtMs).required("revoked", value.revoked).done();
  }
  NativeRuntimeGrant tag_invoke(const json::value_to_tag<NativeRuntimeGrant>&, const json::value& value) {
    const Reader in(value, "NativeRuntimeGrant");
    return {.id = in.required<std::string>("id"), .revision = in.required<double>("revision"), .epoch = in.required<std::string>("epoch"), .subjectId = in.required<std::string>("subjectId"), .contextBinding = in.required<std::string>("contextBinding"), .roles = in.required<std::vector<native_authority_contracts::NativeRole>>("roles"), .expiresAtMs = in.required<double>("expiresAtMs"), .revoked = in.required<bool>("revoked")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeBinding& value) {
    (void)value;
    out = Writer().required("grantId", value.grantId).required("contextBinding", value.contextBinding).required("privateContext", value.privateContext).done();
  }
  NativeRuntimeBinding tag_invoke(const json::value_to_tag<NativeRuntimeBinding>&, const json::value& value) {
    const Reader in(value, "NativeRuntimeBinding");
    return {.grantId = in.required<std::string>("grantId"), .contextBinding = in.required<std::string>("contextBinding"), .privateContext = in.required<json::value>("privateContext")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeApiScope& value) {
    (void)value;
    out = Writer().required("principalRef", value.principalRef).required("tokenHash", value.tokenHash).done();
  }
  NativeRuntimeApiScope tag_invoke(const json::value_to_tag<NativeRuntimeApiScope>&, const json::value& value) {
    const Reader in(value, "NativeRuntimeApiScope");
    return {.principalRef = in.required<native_principal_contracts::NativePrincipalReference>("principalRef"), .tokenHash = in.required<std::string>("tokenHash")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeKernel& value) {
    (void)value;
    out = Writer().required("owner", value.owner).required("repo", value.repo).done();
  }
  NativeRuntimeKernel tag_invoke(const json::value_to_tag<NativeRuntimeKernel>&, const json::value& value) {
    const Reader in(value, "NativeRuntimeKernel");
    return {.owner = in.required<std::string>("owner"), .repo = in.required<std::string>("repo")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const VerifiedNativeRuntime& value) {
    (void)value;
    out = Writer().required("bindingId", value.bindingId).required("subjectId", value.subjectId).required("contextBinding", value.contextBinding).required("intentDigest", value.intentDigest).required("identity", value.identity).orNull("api", value.api).orNull("kernel", value.kernel).done();
  }
  VerifiedNativeRuntime tag_invoke(const json::value_to_tag<VerifiedNativeRuntime>&, const json::value& value) {
    const Reader in(value, "VerifiedNativeRuntime");
    return {.bindingId = in.required<std::string>("bindingId"), .subjectId = in.required<std::string>("subjectId"), .contextBinding = in.required<std::string>("contextBinding"), .intentDigest = in.required<std::string>("intentDigest"), .identity = in.required<NativeRuntimeIdentity>("identity"), .api = in.orNull<NativeRuntimeApiScope>("api"), .kernel = in.orNull<NativeRuntimeKernel>("kernel")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeValidatedMetadata& value) {
    (void)value;
    out = Writer().required("model", value.model).required("stream", value.stream).orNull("reasoning", value.reasoning).done();
  }
  NativeValidatedMetadata tag_invoke(const json::value_to_tag<NativeValidatedMetadata>&, const json::value& value) {
    const Reader in(value, "NativeValidatedMetadata");
    return {.model = in.required<std::string>("model"), .stream = in.required<bool>("stream"), .reasoning = in.orNull<std::string>("reasoning")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRoutingHop& value) {
    (void)value;
    out = Writer().required("hopId", value.hopId).required("authorization", value.authorization).required("upstreamModel", value.upstreamModel).required("billingModel", value.billingModel).done();
  }
  NativeRoutingHop tag_invoke(const json::value_to_tag<NativeRoutingHop>&, const json::value& value) {
    const Reader in(value, "NativeRoutingHop");
    return {.hopId = in.required<std::string>("hopId"), .authorization = in.required<std::string>("authorization"), .upstreamModel = in.required<std::string>("upstreamModel"), .billingModel = in.required<std::string>("billingModel")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeTrustedRoutingDecisionWithoutIntent& value) {
    (void)value;
    out = Writer().required("subjectId", value.subjectId).required("contextBinding", value.contextBinding).required("sourceRevision", value.sourceRevision).required("cohort", value.cohort).required("codex", value.codex).required("providers", value.providers).required("allowUnrosteredModel", value.allowUnrosteredModel).required("hops", value.hops).done();
  }
  NativeTrustedRoutingDecisionWithoutIntent tag_invoke(const json::value_to_tag<NativeTrustedRoutingDecisionWithoutIntent>&, const json::value& value) {
    const Reader in(value, "NativeTrustedRoutingDecisionWithoutIntent");
    return {.subjectId = in.required<std::string>("subjectId"), .contextBinding = in.required<std::string>("contextBinding"), .sourceRevision = in.required<std::string>("sourceRevision"), .cohort = in.required<std::string>("cohort"), .codex = in.required<std::string>("codex"), .providers = in.required<std::vector<native_contracts::ApiKeyUsageProvider>>("providers"), .allowUnrosteredModel = in.required<bool>("allowUnrosteredModel"), .hops = in.required<std::vector<NativeRoutingHop>>("hops")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeTrustedRoutingDecision& value) {
    (void)value;
    out = Writer().required("subjectId", value.subjectId).required("contextBinding", value.contextBinding).required("intentDigest", value.intentDigest).required("sourceRevision", value.sourceRevision).required("cohort", value.cohort).required("codex", value.codex).required("providers", value.providers).required("allowUnrosteredModel", value.allowUnrosteredModel).required("hops", value.hops).done();
  }
  NativeTrustedRoutingDecision tag_invoke(const json::value_to_tag<NativeTrustedRoutingDecision>&, const json::value& value) {
    const Reader in(value, "NativeTrustedRoutingDecision");
    return {.subjectId = in.required<std::string>("subjectId"), .contextBinding = in.required<std::string>("contextBinding"), .intentDigest = in.required<std::string>("intentDigest"), .sourceRevision = in.required<std::string>("sourceRevision"), .cohort = in.required<std::string>("cohort"), .codex = in.required<std::string>("codex"), .providers = in.required<std::vector<native_contracts::ApiKeyUsageProvider>>("providers"), .allowUnrosteredModel = in.required<bool>("allowUnrosteredModel"), .hops = in.required<std::vector<NativeRoutingHop>>("hops")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeAuthorization& value) {
    (void)value;
    out = Writer().required("binding", value.binding).required("verified", value.verified).done();
  }
  NativeRuntimeAuthorization tag_invoke(const json::value_to_tag<NativeRuntimeAuthorization>&, const json::value& value) {
    const Reader in(value, "NativeRuntimeAuthorization");
    return {.binding = in.required<NativeRuntimeBinding>("binding"), .verified = in.required<VerifiedNativeRuntime>("verified")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeRequestBinding& value) {
    (void)value;
    out = Writer().required("handle", value.handle).required("runtimeBindingId", value.runtimeBindingId).required("requestId", value.requestId).required("route", value.route).required("path", value.path).done();
  }
  NativeRuntimeRequestBinding tag_invoke(const json::value_to_tag<NativeRuntimeRequestBinding>&, const json::value& value) {
    const Reader in(value, "NativeRuntimeRequestBinding");
    return {.handle = in.required<std::string>("handle"), .runtimeBindingId = in.required<std::string>("runtimeBindingId"), .requestId = in.required<std::string>("requestId"), .route = in.required<std::string>("route"), .path = in.required<std::string>("path")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRetainedHistoryBinding& value) {
    (void)value;
    out = Writer().required("kind", value.kind).required("grantId", value.grantId).required("contextBinding", value.contextBinding).required("privateContext", value.privateContext).done();
  }
  NativeRetainedHistoryBinding tag_invoke(const json::value_to_tag<NativeRetainedHistoryBinding>&, const json::value& value) {
    const Reader in(value, "NativeRetainedHistoryBinding");
    return {.kind = in.required<std::string>("kind"), .grantId = in.required<std::string>("grantId"), .contextBinding = in.required<std::string>("contextBinding"), .privateContext = in.required<json::value>("privateContext")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRetainedHistoryScope& value) {
    (void)value;
    out = Writer().required("namespace", value.namespace_).required("originalBindingId", value.originalBindingId).required("originalPrincipalId", value.originalPrincipalId).required("keyId", value.keyId).required("requestHandles", value.requestHandles).done();
  }
  NativeRetainedHistoryScope tag_invoke(const json::value_to_tag<NativeRetainedHistoryScope>&, const json::value& value) {
    const Reader in(value, "NativeRetainedHistoryScope");
    return {.namespace_ = in.required<std::string>("namespace"), .originalBindingId = in.required<std::string>("originalBindingId"), .originalPrincipalId = in.required<std::string>("originalPrincipalId"), .keyId = in.required<std::string>("keyId"), .requestHandles = in.required<std::vector<std::string>>("requestHandles")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRetainedHistoryGrant& value) {
    (void)value;
    out = Writer().required("id", value.id).required("revision", value.revision).required("epoch", value.epoch).required("subjectId", value.subjectId).required("contextBinding", value.contextBinding).required("roles", value.roles).required("expiresAtMs", value.expiresAtMs).required("revoked", value.revoked).required("namespace", value.namespace_).required("originalBindingId", value.originalBindingId).required("originalPrincipalId", value.originalPrincipalId).required("keyId", value.keyId).required("requestHandles", value.requestHandles).done();
  }
  NativeRetainedHistoryGrant tag_invoke(const json::value_to_tag<NativeRetainedHistoryGrant>&, const json::value& value) {
    const Reader in(value, "NativeRetainedHistoryGrant");
    return {.id = in.required<std::string>("id"), .revision = in.required<double>("revision"), .epoch = in.required<std::string>("epoch"), .subjectId = in.required<std::string>("subjectId"), .contextBinding = in.required<std::string>("contextBinding"), .roles = in.required<std::vector<native_authority_contracts::NativeRole>>("roles"), .expiresAtMs = in.required<double>("expiresAtMs"), .revoked = in.required<bool>("revoked"), .namespace_ = in.required<std::string>("namespace"), .originalBindingId = in.required<std::string>("originalBindingId"), .originalPrincipalId = in.required<std::string>("originalPrincipalId"), .keyId = in.required<std::string>("keyId"), .requestHandles = in.required<std::vector<std::string>>("requestHandles")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const VerifiedNativeRetainedHistory& value) {
    (void)value;
    out = Writer().required("namespace", value.namespace_).required("originalBindingId", value.originalBindingId).required("originalPrincipalId", value.originalPrincipalId).required("keyId", value.keyId).required("requestHandles", value.requestHandles).required("grantId", value.grantId).required("epoch", value.epoch).required("subjectId", value.subjectId).required("contextBinding", value.contextBinding).required("intentDigest", value.intentDigest).done();
  }
  VerifiedNativeRetainedHistory tag_invoke(const json::value_to_tag<VerifiedNativeRetainedHistory>&, const json::value& value) {
    const Reader in(value, "VerifiedNativeRetainedHistory");
    return {.namespace_ = in.required<std::string>("namespace"), .originalBindingId = in.required<std::string>("originalBindingId"), .originalPrincipalId = in.required<std::string>("originalPrincipalId"), .keyId = in.required<std::string>("keyId"), .requestHandles = in.required<std::vector<std::string>>("requestHandles"), .grantId = in.required<std::string>("grantId"), .epoch = in.required<std::string>("epoch"), .subjectId = in.required<std::string>("subjectId"), .contextBinding = in.required<std::string>("contextBinding"), .intentDigest = in.required<std::string>("intentDigest")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeRuntimeIdentity& value) {
    std::visit([&](const auto& item) {
      using T = std::decay_t<decltype(item)>;
      Writer writer; writer.required("kind", std::string(T::kind));
      if constexpr (std::is_same_v<T, NativeRuntimeIdentityKvApiKey>) { writer.required("keyId", item.keyId); }
      if constexpr (std::is_same_v<T, NativeRuntimeIdentityLocalDevelopmentKey>) { writer.required("keyId", item.keyId); }
      if constexpr (std::is_same_v<T, NativeRuntimeIdentityGithubToken>) { writer.required("owner", item.owner); writer.required("repo", item.repo); }
      if constexpr (std::is_same_v<T, NativeRuntimeIdentityPasskeySession>) { writer.required("userId", item.userId); }
      if constexpr (std::is_same_v<T, NativeRuntimeIdentityDisabledLocal>) { writer.orNull("bearerDigest", item.bearerDigest); }
      out = writer.done();
    }, value);
  }
  NativeRuntimeIdentity tag_invoke(const json::value_to_tag<NativeRuntimeIdentity>&, const json::value& value) {
    const Reader in(value, "NativeRuntimeIdentity");
    const auto kind = in.required<std::string>("kind");
    if (kind == "kv_api_key") return NativeRuntimeIdentityKvApiKey{.keyId = in.required<std::string>("keyId")};
    if (kind == "local_development_key") return NativeRuntimeIdentityLocalDevelopmentKey{.keyId = in.required<std::string>("keyId")};
    if (kind == "github_token") return NativeRuntimeIdentityGithubToken{.owner = in.required<std::string>("owner"), .repo = in.required<std::string>("repo")};
    if (kind == "passkey_session") return NativeRuntimeIdentityPasskeySession{.userId = in.required<std::string>("userId")};
    if (kind == "auth_tokens_allowlist") return NativeRuntimeIdentityAuthTokensAllowlist{};
    if (kind == "admin_allowlist") return NativeRuntimeIdentityAdminAllowlist{};
    if (kind == "deno_deploy_token") return NativeRuntimeIdentityDenoDeployToken{};
    if (kind == "disabled_local") return NativeRuntimeIdentityDisabledLocal{.bearerDigest = in.orNull<std::string>("bearerDigest")};
    in.fail("kind", rt::get(value, "kind"), "known discriminator");
  }
}
