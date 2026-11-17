#include "uos/authority/native_paid_contracts.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/record.hpp"

#include <string>

namespace uos::authority::native_paid_contracts {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackPendingV3& value) {
    out = Writer()
      .required("created_at_ms", value.created_at_ms)
      .required("next_reconciliation_at_ms", value.next_reconciliation_at_ms)
      .done();
  }

  PaidFallbackPendingV3 tag_invoke(const json::value_to_tag<PaidFallbackPendingV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackPendingV3");
    return {
      .created_at_ms = in.required<double>("created_at_ms"),
      .next_reconciliation_at_ms = in.required<double>("next_reconciliation_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackReconciliationGateV3& value) {
    out = Writer()
      .orNull("next_due_at_ms", value.next_due_at_ms)
      .done();
  }

  PaidFallbackReconciliationGateV3 tag_invoke(const json::value_to_tag<PaidFallbackReconciliationGateV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackReconciliationGateV3");
    return {
      .next_due_at_ms = in.orNull<double>("next_due_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackReconciliationLeaseV3& value) {
    out = Writer()
      .required("token", value.token)
      .required("expires_at_ms", value.expires_at_ms)
      .done();
  }

  PaidFallbackReconciliationLeaseV3 tag_invoke(const json::value_to_tag<PaidFallbackReconciliationLeaseV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackReconciliationLeaseV3");
    return {
      .token = in.required<std::string>("token"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackBackfillLeaseV3& value) {
    out = Writer()
      .required("token", value.token)
      .required("expires_at_ms", value.expires_at_ms)
      .done();
  }

  PaidFallbackBackfillLeaseV3 tag_invoke(const json::value_to_tag<PaidFallbackBackfillLeaseV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackBackfillLeaseV3");
    return {
      .token = in.required<std::string>("token"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackBackfillStateV3& value) {
    out = Writer()
      .required("v", value.v)
      .required("completed_at_ms", value.completed_at_ms)
      .done();
  }

  PaidFallbackBackfillStateV3 tag_invoke(const json::value_to_tag<PaidFallbackBackfillStateV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackBackfillStateV3");
    return {
      .v = in.required<double>("v"),
      .completed_at_ms = in.required<double>("completed_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackDeletionGuardV3& value) {
    out = Writer()
      .required("created_at_ms", value.created_at_ms)
      .done();
  }

  PaidFallbackDeletionGuardV3 tag_invoke(const json::value_to_tag<PaidFallbackDeletionGuardV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackDeletionGuardV3");
    return {
      .created_at_ms = in.required<double>("created_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackAdmissionV3& value) {
    out = Writer()
      .required("key_id", value.key_id)
      .required("request_id", value.request_id)
      .required("created_at_ms", value.created_at_ms)
      .required("reserved_microcredits", value.reserved_microcredits)
      .required("quota_per_credit", value.quota_per_credit)
      .required("window_reset_at_ms", value.window_reset_at_ms)
      .orNull("quota_used_percent", value.quota_used_percent)
      .done();
  }

  PaidFallbackAdmissionV3 tag_invoke(const json::value_to_tag<PaidFallbackAdmissionV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackAdmissionV3");
    return {
      .key_id = in.required<std::string>("key_id"),
      .request_id = in.required<std::string>("request_id"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .reserved_microcredits = in.required<double>("reserved_microcredits"),
      .quota_per_credit = in.required<double>("quota_per_credit"),
      .window_reset_at_ms = in.required<double>("window_reset_at_ms"),
      .quota_used_percent = in.orNull<double>("quota_used_percent"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackWindowProjectionV3& value) {
    out = Writer()
      .required("key_id", value.key_id)
      .orNull("policy_version", value.policy_version)
      .required("window_reset_at_ms", value.window_reset_at_ms)
      .required("limit_microcredits", value.limit_microcredits)
      .required("settled_microcredits", value.settled_microcredits)
      .required("reserved_microcredits", value.reserved_microcredits)
      .required("pending_count", value.pending_count)
      .orNull("updated_at_ms", value.updated_at_ms)
      .done();
  }

  PaidFallbackWindowProjectionV3 tag_invoke(const json::value_to_tag<PaidFallbackWindowProjectionV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackWindowProjectionV3");
    return {
      .key_id = in.required<std::string>("key_id"),
      .policy_version = in.orNull<std::string>("policy_version"),
      .window_reset_at_ms = in.required<double>("window_reset_at_ms"),
      .limit_microcredits = in.required<double>("limit_microcredits"),
      .settled_microcredits = in.required<double>("settled_microcredits"),
      .reserved_microcredits = in.required<double>("reserved_microcredits"),
      .pending_count = in.required<double>("pending_count"),
      .updated_at_ms = in.orNull<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackOutstandingV3& value) {
    out = Writer()
      .required("pending_requests", value.pending_requests)
      .required("unresolved_requests", value.unresolved_requests)
      .required("pending_markers", value.pending_markers)
      .required("has_outstanding", value.has_outstanding)
      .done();
  }

  PaidFallbackOutstandingV3 tag_invoke(const json::value_to_tag<PaidFallbackOutstandingV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackOutstandingV3");
    return {
      .pending_requests = in.required<double>("pending_requests"),
      .unresolved_requests = in.required<double>("unresolved_requests"),
      .pending_markers = in.required<double>("pending_markers"),
      .has_outstanding = in.required<bool>("has_outstanding"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackRequestLifecyclePatchV3& value) {
    out = Writer()
      .orUndefined("provider", value.provider)
      .nullable("provider_request_id", value.provider_request_id)
      .orUndefined("dispatch_state", value.dispatch_state)
      .orUndefined("terminal_state", value.terminal_state)
      .orUndefined("reconciliation_attempts", value.reconciliation_attempts)
      .nullable("last_reconciliation_at_ms", value.last_reconciliation_at_ms)
      .orUndefined("increment_reconciliation_attempts", value.increment_reconciliation_attempts)
      .done();
  }

  PaidFallbackRequestLifecyclePatchV3 tag_invoke(const json::value_to_tag<PaidFallbackRequestLifecyclePatchV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackRequestLifecyclePatchV3");
    return {
      .provider = in.orUndefined<std::string>("provider"),
      .provider_request_id = in.nullable<std::string>("provider_request_id"),
      .dispatch_state = in.orUndefined<std::string>("dispatch_state"),
      .terminal_state = in.orUndefined<std::string>("terminal_state"),
      .reconciliation_attempts = in.orUndefined<double>("reconciliation_attempts"),
      .last_reconciliation_at_ms = in.nullable<double>("last_reconciliation_at_ms"),
      .increment_reconciliation_attempts = in.orUndefined<bool>("increment_reconciliation_attempts"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackUsageRollup& value) {
    out = Writer()
      .required("v", value.v)
      .required("bucket_start_at_ms", value.bucket_start_at_ms)
      .required("model", value.model)
      .required("provider", value.provider)
      .required("request_count", value.request_count)
      .required("quota_sum", value.quota_sum)
      .required("input_tokens", value.input_tokens)
      .required("cached_input_tokens", value.cached_input_tokens)
      .required("output_tokens", value.output_tokens)
      .required("spend_microcredits", value.spend_microcredits)
      .required("first_request_at_ms", value.first_request_at_ms)
      .required("last_request_at_ms", value.last_request_at_ms)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  PaidFallbackUsageRollup tag_invoke(const json::value_to_tag<PaidFallbackUsageRollup>&, const json::value& value) {
    const Reader in(value, "PaidFallbackUsageRollup");
    return {
      .v = in.required<double>("v"),
      .bucket_start_at_ms = in.required<double>("bucket_start_at_ms"),
      .model = in.required<std::string>("model"),
      .provider = in.required<std::string>("provider"),
      .request_count = in.required<double>("request_count"),
      .quota_sum = in.required<double>("quota_sum"),
      .input_tokens = in.required<double>("input_tokens"),
      .cached_input_tokens = in.required<double>("cached_input_tokens"),
      .output_tokens = in.required<double>("output_tokens"),
      .spend_microcredits = in.required<double>("spend_microcredits"),
      .first_request_at_ms = in.required<double>("first_request_at_ms"),
      .last_request_at_ms = in.required<double>("last_request_at_ms"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackUsageRollupInput& value) {
    out = Writer()
      .required("bucket_start_at_ms", value.bucket_start_at_ms)
      .required("model", value.model)
      .required("provider", value.provider)
      .required("request_id", value.request_id)
      .required("quota", value.quota)
      .required("input_tokens", value.input_tokens)
      .orNull("cached_input_tokens", value.cached_input_tokens)
      .required("output_tokens", value.output_tokens)
      .required("spend_microcredits", value.spend_microcredits)
      .required("request_created_at_ms", value.request_created_at_ms)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  PaidFallbackUsageRollupInput tag_invoke(const json::value_to_tag<PaidFallbackUsageRollupInput>&, const json::value& value) {
    const Reader in(value, "PaidFallbackUsageRollupInput");
    return {
      .bucket_start_at_ms = in.required<double>("bucket_start_at_ms"),
      .model = in.required<std::string>("model"),
      .provider = in.required<std::string>("provider"),
      .request_id = in.required<std::string>("request_id"),
      .quota = in.required<double>("quota"),
      .input_tokens = in.required<double>("input_tokens"),
      .cached_input_tokens = in.orNull<double>("cached_input_tokens"),
      .output_tokens = in.required<double>("output_tokens"),
      .spend_microcredits = in.required<double>("spend_microcredits"),
      .request_created_at_ms = in.required<double>("request_created_at_ms"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const TerminalUsageRollupInput& value) {
    out = Writer()
      .orNull("model", value.model)
      .orNull("provider", value.provider)
      .required("request_id", value.request_id)
      .orUndefined("request_created_at_ms", value.request_created_at_ms)
      .orNull("input_tokens", value.input_tokens)
      .orNull("cached_input_tokens", value.cached_input_tokens)
      .orNull("output_tokens", value.output_tokens)
      .done();
  }

  TerminalUsageRollupInput tag_invoke(const json::value_to_tag<TerminalUsageRollupInput>&, const json::value& value) {
    const Reader in(value, "TerminalUsageRollupInput");
    return {
      .model = in.orNull<std::string>("model"),
      .provider = in.orNull<std::string>("provider"),
      .request_id = in.required<std::string>("request_id"),
      .request_created_at_ms = in.orUndefined<double>("request_created_at_ms"),
      .input_tokens = in.orNull<double>("input_tokens"),
      .cached_input_tokens = in.orNull<double>("cached_input_tokens"),
      .output_tokens = in.orNull<double>("output_tokens"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePaidProviderLog& value) {
    out = Writer()
      .required("request_id", value.request_id)
      .required("quota", value.quota)
      .required("prompt_tokens", value.prompt_tokens)
      .orUndefined("cached_prompt_tokens", value.cached_prompt_tokens)
      .orUndefined("cache_write_prompt_tokens", value.cache_write_prompt_tokens)
      .required("completion_tokens", value.completion_tokens)
      .required("model", value.model)
      .required("created_at", value.created_at)
      .done();
  }

  NativePaidProviderLog tag_invoke(const json::value_to_tag<NativePaidProviderLog>&, const json::value& value) {
    const Reader in(value, "NativePaidProviderLog");
    return {
      .request_id = in.required<std::string>("request_id"),
      .quota = in.required<double>("quota"),
      .prompt_tokens = in.required<double>("prompt_tokens"),
      .cached_prompt_tokens = in.orUndefined<double>("cached_prompt_tokens"),
      .cache_write_prompt_tokens = in.orUndefined<double>("cache_write_prompt_tokens"),
      .completion_tokens = in.required<double>("completion_tokens"),
      .model = in.required<std::string>("model"),
      .created_at = in.required<double>("created_at"),
    };
  }
}
