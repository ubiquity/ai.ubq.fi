// Port of src/authority/native-principal-contracts.ts.
#include "uos/authority/native_principal_contracts.hpp"
#include "uos/rt/record.hpp"

namespace uos::authority::native_principal_contracts {
  namespace { using rt::record::Reader; using rt::record::Writer; }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKeyIntent& value) {
    (void)value;
    out = Writer().orUndefined("name", value.name).orUndefined("expires_at_ms", value.expires_at_ms).orUndefined("usage_limit_requests", value.usage_limit_requests).orUndefined("window_ms", value.window_ms).orUndefined("paid_fallback_enabled", value.paid_fallback_enabled).orUndefined("paid_fallback_limit_credits", value.paid_fallback_limit_credits).orUndefined("banked_resets_enabled", value.banked_resets_enabled).done();
  }
  NativeKeyIntent tag_invoke(const json::value_to_tag<NativeKeyIntent>&, const json::value& value) {
    const Reader in(value, "NativeKeyIntent");
    return {.name = in.orUndefined<json::value>("name"), .expires_at_ms = in.orUndefined<json::value>("expires_at_ms"), .usage_limit_requests = in.orUndefined<json::value>("usage_limit_requests"), .window_ms = in.orUndefined<json::value>("window_ms"), .paid_fallback_enabled = in.orUndefined<json::value>("paid_fallback_enabled"), .paid_fallback_limit_credits = in.orUndefined<json::value>("paid_fallback_limit_credits"), .banked_resets_enabled = in.orUndefined<json::value>("banked_resets_enabled")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKeyUpdateIntent& value) {
    (void)value;
    out = Writer().orUndefined("name", value.name).orUndefined("expires_at_ms", value.expires_at_ms).orUndefined("usage_limit_requests", value.usage_limit_requests).orUndefined("window_ms", value.window_ms).orUndefined("paid_fallback_enabled", value.paid_fallback_enabled).orUndefined("paid_fallback_limit_credits", value.paid_fallback_limit_credits).orUndefined("banked_resets_enabled", value.banked_resets_enabled).orUndefined("reset_usage", value.reset_usage).done();
  }
  NativeKeyUpdateIntent tag_invoke(const json::value_to_tag<NativeKeyUpdateIntent>&, const json::value& value) {
    const Reader in(value, "NativeKeyUpdateIntent");
    return {.name = in.orUndefined<json::value>("name"), .expires_at_ms = in.orUndefined<json::value>("expires_at_ms"), .usage_limit_requests = in.orUndefined<json::value>("usage_limit_requests"), .window_ms = in.orUndefined<json::value>("window_ms"), .paid_fallback_enabled = in.orUndefined<json::value>("paid_fallback_enabled"), .paid_fallback_limit_credits = in.orUndefined<json::value>("paid_fallback_limit_credits"), .banked_resets_enabled = in.orUndefined<json::value>("banked_resets_enabled"), .reset_usage = in.orUndefined<json::value>("reset_usage")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlCommand& value) {
    (void)value;
    out = Writer().required("epoch", value.epoch).required("operationId", value.operationId).required("method", value.method).required("args", value.args).done();
  }
  NativeControlCommand tag_invoke(const json::value_to_tag<NativeControlCommand>&, const json::value& value) {
    const Reader in(value, "NativeControlCommand");
    return {.epoch = in.required<std::string>("epoch"), .operationId = in.required<std::string>("operationId"), .method = in.required<NativeControlMethod>("method"), .args = in.required<json::object>("args")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKeyControlGrant& value) {
    (void)value;
    out = Writer().required("id", value.id).required("epoch", value.epoch).required("subjectId", value.subjectId).required("contextBinding", value.contextBinding).required("roles", value.roles).required("expiresAtMs", value.expiresAtMs).required("revoked", value.revoked).orNull("targetKeyIds", value.targetKeyIds).done();
  }
  NativeKeyControlGrant tag_invoke(const json::value_to_tag<NativeKeyControlGrant>&, const json::value& value) {
    const Reader in(value, "NativeKeyControlGrant");
    return {.id = in.required<std::string>("id"), .epoch = in.required<std::string>("epoch"), .subjectId = in.required<std::string>("subjectId"), .contextBinding = in.required<std::string>("contextBinding"), .roles = in.required<std::vector<NativeControlRole>>("roles"), .expiresAtMs = in.required<double>("expiresAtMs"), .revoked = in.required<bool>("revoked"), .targetKeyIds = in.orNull<std::vector<std::string>>("targetKeyIds")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePrincipalReference& value) {
    (void)value;
    out = Writer().required("id", value.id).required("principalId", value.principalId).required("keyId", value.keyId).required("epoch", value.epoch).done();
  }
  NativePrincipalReference tag_invoke(const json::value_to_tag<NativePrincipalReference>&, const json::value& value) {
    const Reader in(value, "NativePrincipalReference");
    return {.id = in.required<std::string>("id"), .principalId = in.required<std::string>("principalId"), .keyId = in.required<std::string>("keyId"), .epoch = in.required<std::string>("epoch")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePrincipalResolution& value) {
    (void)value;
    out = Writer().required("id", value.id).required("principalId", value.principalId).required("keyId", value.keyId).required("epoch", value.epoch).required("tokenHash", value.tokenHash).required("hashRevision", value.hashRevision).required("grantId", value.grantId).done();
  }
  NativePrincipalResolution tag_invoke(const json::value_to_tag<NativePrincipalResolution>&, const json::value& value) {
    const Reader in(value, "NativePrincipalResolution");
    return {.id = in.required<std::string>("id"), .principalId = in.required<std::string>("principalId"), .keyId = in.required<std::string>("keyId"), .epoch = in.required<std::string>("epoch"), .tokenHash = in.required<std::string>("tokenHash"), .hashRevision = in.required<std::string>("hashRevision"), .grantId = in.required<std::string>("grantId")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeKeyCreatePreparation& value) {
    (void)value;
    out = Writer().required("record", value.record).required("projection", value.projection).required("atMs", value.atMs).orNull("hashRevision", value.hashRevision).done();
  }
  NativeKeyCreatePreparation tag_invoke(const json::value_to_tag<NativeKeyCreatePreparation>&, const json::value& value) {
    const Reader in(value, "NativeKeyCreatePreparation");
    return {.record = in.required<::uos::types::ApiKeyRecord>("record"), .projection = in.required<native_authority_contracts::NativeKeyPolicy>("projection"), .atMs = in.required<double>("atMs"), .hashRevision = in.orNull<std::string>("hashRevision")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlCleanup& value) {
    (void)value;
    out = Writer().required("keyId", value.keyId).orNull("localOwner", value.localOwner).done();
  }
  NativeControlCleanup tag_invoke(const json::value_to_tag<NativeControlCleanup>&, const json::value& value) {
    const Reader in(value, "NativeControlCleanup");
    return {.keyId = in.required<std::string>("keyId"), .localOwner = in.orNull<std::string>("localOwner")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlOutcome& value) {
    std::visit([&](const auto& item) {
      using T = std::decay_t<decltype(item)>;
      Writer writer; writer.required("kind", std::string(T::kind));
      if constexpr (std::is_same_v<T, NativeControlOutcomePrincipal>) { writer.required("principalRef", item.principalRef); }
      if constexpr (std::is_same_v<T, NativeControlOutcomeApplied>) { writer.required("keyId", item.keyId); writer.orUndefined("detail", item.detail); }
      if constexpr (std::is_same_v<T, NativeControlOutcomeUnchanged>) { writer.required("keyId", item.keyId); writer.orUndefined("detail", item.detail); }
      if constexpr (std::is_same_v<T, NativeControlOutcomeRefused>) { writer.required("status", item.status); writer.required("code", item.code); writer.required("message", item.message); }
      out = writer.done();
    }, value);
  }
  NativeControlOutcome tag_invoke(const json::value_to_tag<NativeControlOutcome>&, const json::value& value) {
    const Reader in(value, "NativeControlOutcome");
    const auto kind = in.required<std::string>("kind");
    if (kind == "principal") return NativeControlOutcomePrincipal{.principalRef = in.required<NativePrincipalReference>("principalRef")};
    if (kind == "applied") return NativeControlOutcomeApplied{.keyId = in.required<std::string>("keyId"), .detail = in.orUndefined<json::object>("detail")};
    if (kind == "unchanged") return NativeControlOutcomeUnchanged{.keyId = in.required<std::string>("keyId"), .detail = in.orUndefined<json::object>("detail")};
    if (kind == "refused") return NativeControlOutcomeRefused{.status = in.required<double>("status"), .code = in.required<std::string>("code"), .message = in.required<std::string>("message")};
    in.fail("kind", rt::get(value, "kind"), "known discriminator");
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlReply& value) {
    std::visit([&](const auto& item) {
      using T = std::decay_t<decltype(item)>;
      Writer writer; writer.required("kind", std::string(T::kind));
      if constexpr (std::is_same_v<T, NativeControlOutcomePrincipal>) { writer.required("principalRef", item.principalRef); }
      if constexpr (std::is_same_v<T, NativeControlOutcomeApplied>) { writer.required("keyId", item.keyId); writer.orUndefined("detail", item.detail); }
      if constexpr (std::is_same_v<T, NativeControlOutcomeUnchanged>) { writer.required("keyId", item.keyId); writer.orUndefined("detail", item.detail); }
      if constexpr (std::is_same_v<T, NativeControlOutcomeRefused>) { writer.required("status", item.status); writer.required("code", item.code); writer.required("message", item.message); }
      if constexpr (std::is_same_v<T, NativeControlReplyHistoricalAck>) { writer.required("outcome", item.outcome); }
      out = writer.done();
    }, value);
  }
  NativeControlReply tag_invoke(const json::value_to_tag<NativeControlReply>&, const json::value& value) {
    const Reader in(value, "NativeControlReply");
    const auto kind = in.required<std::string>("kind");
    if (kind == "principal") return NativeControlOutcomePrincipal{.principalRef = in.required<NativePrincipalReference>("principalRef")};
    if (kind == "applied") return NativeControlOutcomeApplied{.keyId = in.required<std::string>("keyId"), .detail = in.orUndefined<json::object>("detail")};
    if (kind == "unchanged") return NativeControlOutcomeUnchanged{.keyId = in.required<std::string>("keyId"), .detail = in.orUndefined<json::object>("detail")};
    if (kind == "refused") return NativeControlOutcomeRefused{.status = in.required<double>("status"), .code = in.required<std::string>("code"), .message = in.required<std::string>("message")};
    if (kind == "historical_ack") return NativeControlReplyHistoricalAck{.outcome = in.required<NativeControlOutcome>("outcome")};
    in.fail("kind", rt::get(value, "kind"), "known discriminator");
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlReceipt& value) {
    (void)value;
    out = Writer().required("intent", value.intent).orNull("targetKeyId", value.targetKeyId).orNull("reply", value.reply).orUndefined("prepared", value.prepared).orNull("cleanup", value.cleanup).done();
  }
  NativeControlReceipt tag_invoke(const json::value_to_tag<NativeControlReceipt>&, const json::value& value) {
    const Reader in(value, "NativeControlReceipt");
    return {.intent = in.required<std::string>("intent"), .targetKeyId = in.orNull<std::string>("targetKeyId"), .reply = in.orNull<NativeControlOutcome>("reply"), .prepared = in.orUndefined<NativeKeyCreatePreparation>("prepared"), .cleanup = in.orNull<NativeControlCleanup>("cleanup")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeControlBinding& value) {
    (void)value;
    out = Writer().required("grantId", value.grantId).required("contextBinding", value.contextBinding).required("privateContext", value.privateContext).done();
  }
  NativeControlBinding tag_invoke(const json::value_to_tag<NativeControlBinding>&, const json::value& value) {
    const Reader in(value, "NativeControlBinding");
    return {.grantId = in.required<std::string>("grantId"), .contextBinding = in.required<std::string>("contextBinding"), .privateContext = in.required<json::value>("privateContext")};
  }
  namespace NativeControlArguments {
  void tag_invoke(const json::value_from_tag&, json::value& out, const resolvePrincipal& value) {
    (void)value;
    out = Writer().done();
  }
  resolvePrincipal tag_invoke(const json::value_to_tag<resolvePrincipal>&, const json::value& value) {
    const Reader in(value, "resolvePrincipal");
    return {};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const createKey& value) {
    (void)value;
    out = Writer().required("input", value.input).done();
  }
  createKey tag_invoke(const json::value_to_tag<createKey>&, const json::value& value) {
    const Reader in(value, "createKey");
    return {.input = in.required<NativeKeyIntent>("input")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const updateKey& value) {
    (void)value;
    out = Writer().required("input", value.input).done();
  }
  updateKey tag_invoke(const json::value_to_tag<updateKey>&, const json::value& value) {
    const Reader in(value, "updateKey");
    return {.input = in.required<NativeKeyUpdateIntent>("input")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const revokeKey& value) {
    (void)value;
    out = Writer().done();
  }
  revokeKey tag_invoke(const json::value_to_tag<revokeKey>&, const json::value& value) {
    const Reader in(value, "revokeKey");
    return {};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const unrevokeKey& value) {
    (void)value;
    out = Writer().done();
  }
  unrevokeKey tag_invoke(const json::value_to_tag<unrevokeKey>&, const json::value& value) {
    const Reader in(value, "unrevokeKey");
    return {};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const deleteKey& value) {
    (void)value;
    out = Writer().done();
  }
  deleteKey tag_invoke(const json::value_to_tag<deleteKey>&, const json::value& value) {
    const Reader in(value, "deleteKey");
    return {};
  }
  }
}
