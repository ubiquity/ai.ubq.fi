#include "uos/types.hpp"

#include "codex/banked_reset_record_fields.hpp"
#include "uos/codex/banked_reset.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::types {
  namespace {
    using rt::record::describe;
    using rt::record::Reader;
    using rt::record::Writer;

    constexpr std::array<std::string_view, 5> CODEX_RESET_REDEMPTION_STATES = {"claimed", "submitted", "unknown", "verified", "rejected"};

    json::value contentItemJson(const MessageContentItemInputText& item) {
      return Writer().required("type", std::string(item.type)).required("text", item.text).orUndefined("prompt_cache_breakpoint", item.prompt_cache_breakpoint).done();
    }
    json::value contentItemJson(const MessageContentItemOutputText& item) { return Writer().required("type", std::string(item.type)).required("text", item.text).done(); }
    json::value contentItemJson(const MessageContentItemInputImage& item) {
      return Writer()
        .required("type", std::string(item.type))
        .orUndefined("image_url", item.image_url)
        .orUndefined("file_id", item.file_id)
        .nullable("detail", item.detail)
        .orUndefined("prompt_cache_breakpoint", item.prompt_cache_breakpoint)
        .done();
    }
    json::value contentItemJson(const MessageContentItemInputFile& item) {
      return Writer()
        .required("type", std::string(item.type))
        .orUndefined("file_id", item.file_id)
        .orUndefined("file_data", item.file_data)
        .orUndefined("file_url", item.file_url)
        .nullable("filename", item.filename)
        .orUndefined("detail", item.detail)
        .orUndefined("prompt_cache_breakpoint", item.prompt_cache_breakpoint)
        .done();
    }
  }

  std::string_view toString(CodexResetRedemptionState state) { return CODEX_RESET_REDEMPTION_STATES.at(static_cast<std::size_t>(state)); }

  std::optional<CodexResetRedemptionState> parseCodexResetRedemptionState(std::string_view text) {
    for (std::size_t index = 0; index < CODEX_RESET_REDEMPTION_STATES.size(); ++index) {
      if (CODEX_RESET_REDEMPTION_STATES[index] == text) return static_cast<CodexResetRedemptionState>(index);
    }
    return std::nullopt;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, CodexResetRedemptionState value) { out = json::string(toString(value)); }

  CodexResetRedemptionState tag_invoke(const json::value_to_tag<CodexResetRedemptionState>&, const json::value& value) {
    if (const json::string* text = value.if_string()) {
      if (const std::optional<CodexResetRedemptionState> state = parseCodexResetRedemptionState(std::string_view(text->data(), text->size()))) return *state;
    }
    throw rt::TypeError("CodexResetRedemptionState is " + describe(&value) + ", not \"claimed\" | \"submitted\" | \"unknown\" | \"verified\" | \"rejected\"");
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthState::NativeOwner& value) {
    out = Writer()
      .required("codex_home", value.codex_home)
      .required("generation_hash", value.generation_hash)
      .orUndefined("native_refreshed_at", value.native_refreshed_at)
      .orUndefined("auth_mode", value.auth_mode)
      .orUndefined("chatgpt_user_id", value.chatgpt_user_id)
      .done();
  }

  CodexAuthState::NativeOwner tag_invoke(const json::value_to_tag<CodexAuthState::NativeOwner>&, const json::value& value) {
    const Reader in(value, "CodexAuthState.native_owner");
    return {
      .codex_home = in.required<std::string>("codex_home"),
      .generation_hash = in.required<std::string>("generation_hash"),
      .native_refreshed_at = in.orUndefined<std::string>("native_refreshed_at"),
      .auth_mode = in.orUndefined<std::string>("auth_mode"),
      .chatgpt_user_id = in.orUndefined<std::string>("chatgpt_user_id"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthState& value) {
    out = Writer()
      .required("access_token", value.access_token)
      .required("refresh_token", value.refresh_token)
      .required("account_id", value.account_id)
      .required("updated_at_ms", value.updated_at_ms)
      .orUndefined("native_owner", value.native_owner)
      .done();
  }

  CodexAuthState tag_invoke(const json::value_to_tag<CodexAuthState>&, const json::value& value) {
    const Reader in(value, "CodexAuthState");
    return {
      .access_token = in.required<std::string>("access_token"),
      .refresh_token = in.required<std::string>("refresh_token"),
      .account_id = in.required<std::string>("account_id"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
      .native_owner = in.orUndefined<CodexAuthState::NativeOwner>("native_owner"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthPoolState& value) {
    out = Writer()
      .required("accounts", value.accounts)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  CodexAuthPoolState tag_invoke(const json::value_to_tag<CodexAuthPoolState>&, const json::value& value) {
    const Reader in(value, "CodexAuthPoolState");
    return {
      .accounts = in.required<std::vector<CodexAuthState>>("accounts"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexResetRedemptionRecord& value) {
    out = Writer()
      .required("v", value.v)
      .required("account_id_hash", value.account_id_hash)
      .required("credential_version", value.credential_version)
      .required("quota_generation", value.quota_generation)
      .required("routing_generation", value.routing_generation)
      .required("idempotency_key_hash", value.idempotency_key_hash)
      .required("state", value.state)
      .required("owner_token", value.owner_token)
      .required("fence", value.fence)
      .required("lease_expires_at_ms", value.lease_expires_at_ms)
      .orNull("provider_receipt_id", value.provider_receipt_id)
      .required("created_at_ms", value.created_at_ms)
      .required("updated_at_ms", value.updated_at_ms)
      .orNull("submitted_at_ms", value.submitted_at_ms)
      .orNull("verified_at_ms", value.verified_at_ms)
      .orNull("last_error_code", value.last_error_code)
      .done();
  }

  CodexResetRedemptionRecord tag_invoke(const json::value_to_tag<CodexResetRedemptionRecord>&, const json::value& value) {
    const Reader in(value, "CodexResetRedemptionRecord");
    return {
      .v = in.required<double>("v"),
      .account_id_hash = in.required<std::string>("account_id_hash"),
      .credential_version = in.required<std::string>("credential_version"),
      .quota_generation = in.required<std::string>("quota_generation"),
      .routing_generation = in.required<double>("routing_generation"),
      .idempotency_key_hash = in.required<std::string>("idempotency_key_hash"),
      .state = in.required<CodexResetRedemptionState>("state"),
      .owner_token = in.required<std::string>("owner_token"),
      .fence = in.required<double>("fence"),
      .lease_expires_at_ms = in.required<double>("lease_expires_at_ms"),
      .provider_receipt_id = in.orNull<std::string>("provider_receipt_id"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
      .submitted_at_ms = in.orNull<double>("submitted_at_ms"),
      .verified_at_ms = in.orNull<double>("verified_at_ms"),
      .last_error_code = in.orNull<std::string>("last_error_code"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexResetGlobalDailyRecord& value) {
    out = Writer()
      .required("v", value.v)
      .required("day", value.day)
      .required("submission_count", value.submission_count)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  CodexResetGlobalDailyRecord tag_invoke(const json::value_to_tag<CodexResetGlobalDailyRecord>&, const json::value& value) {
    const Reader in(value, "CodexResetGlobalDailyRecord");
    return {
      .v = in.required<double>("v"),
      .day = in.required<std::string>("day"),
      .submission_count = in.required<double>("submission_count"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexResetShadowDecisionRecord::Fence& value) {
    out = Writer()
      .required("slot", value.slot)
      .required("account_id_hash", value.account_id_hash)
      .required("quota_generation", value.quota_generation)
      .required("routing_generation", value.routing_generation)
      .required("quota_reset_at_ms", value.quota_reset_at_ms)
      .done();
  }

  CodexResetShadowDecisionRecord::Fence tag_invoke(const json::value_to_tag<CodexResetShadowDecisionRecord::Fence>&, const json::value& value) {
    const Reader in(value, "CodexResetShadowDecisionRecord.fences");
    return {
      .slot = in.required<double>("slot"),
      .account_id_hash = in.required<std::string>("account_id_hash"),
      .quota_generation = in.required<std::string>("quota_generation"),
      .routing_generation = in.required<double>("routing_generation"),
      .quota_reset_at_ms = in.required<double>("quota_reset_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexResetShadowDecisionRecord& value) {
    out = Writer()
      .required("v", value.v)
      .required("episode_hash", value.episode_hash)
      .required("created_at_ms", value.created_at_ms)
      .required("expires_at_ms", value.expires_at_ms)
      .required("decision_reason", value.decision_reason)
      .orNull("selected_account_id_hash", value.selected_account_id_hash)
      .orNull("selected_credit_id_hash", value.selected_credit_id_hash)
      .orNull("selected_credit_expires_at_ms", value.selected_credit_expires_at_ms)
      .required("fences", value.fences)
      .done();
  }

  CodexResetShadowDecisionRecord tag_invoke(const json::value_to_tag<CodexResetShadowDecisionRecord>&, const json::value& value) {
    const Reader in(value, "CodexResetShadowDecisionRecord");
    return {
      .v = in.required<double>("v"),
      .episode_hash = in.required<std::string>("episode_hash"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .decision_reason = in.required<std::string>("decision_reason"),
      .selected_account_id_hash = in.orNull<std::string>("selected_account_id_hash"),
      .selected_credit_id_hash = in.orNull<std::string>("selected_credit_id_hash"),
      .selected_credit_expires_at_ms = in.orNull<double>("selected_credit_expires_at_ms"),
      .fences = in.required<std::vector<CodexResetShadowDecisionRecord::Fence>>("fences"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyRecord& value) {
    out = Writer()
      .required("id", value.id)
      .required("name", value.name)
      .required("prefix", value.prefix)
      .required("hash", value.hash)
      .required("created_at_ms", value.created_at_ms)
      .required("expires_at_ms", value.expires_at_ms)
      .orNull("revoked_at_ms", value.revoked_at_ms)
      .required("usage_limit_requests", value.usage_limit_requests)
      .required("usage_requests", value.usage_requests)
      .required("usage_reset_at_ms", value.usage_reset_at_ms)
      .required("window_ms", value.window_ms)
      .required("usage_quota_version", value.usage_quota_version)
      .required("paid_fallback_enabled", value.paid_fallback_enabled)
      .required("paid_fallback_limit_microcredits", value.paid_fallback_limit_microcredits)
      .required("paid_fallback_spent_microcredits", value.paid_fallback_spent_microcredits)
      .required("paid_fallback_reserved_microcredits", value.paid_fallback_reserved_microcredits)
      .orNull("paid_fallback_reservation_request_id", value.paid_fallback_reservation_request_id)
      .required("paid_fallback_model_ids", value.paid_fallback_model_ids)
      .required("paid_fallback_quota_per_credit", value.paid_fallback_quota_per_credit)
      .orUndefined("paid_fallback_max_exposure_microcredits", value.paid_fallback_max_exposure_microcredits)
      .orNull("paid_fallback_pricing_checked_at_ms", value.paid_fallback_pricing_checked_at_ms)
      .done();
  }

  ApiKeyRecord tag_invoke(const json::value_to_tag<ApiKeyRecord>&, const json::value& value) {
    const Reader in(value, "ApiKeyRecord");
    return {
      .id = in.required<std::string>("id"),
      .name = in.required<std::string>("name"),
      .prefix = in.required<std::string>("prefix"),
      .hash = in.required<std::string>("hash"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .revoked_at_ms = in.orNull<double>("revoked_at_ms"),
      .usage_limit_requests = in.required<double>("usage_limit_requests"),
      .usage_requests = in.required<double>("usage_requests"),
      .usage_reset_at_ms = in.required<double>("usage_reset_at_ms"),
      .window_ms = in.required<double>("window_ms"),
      .usage_quota_version = in.required<double>("usage_quota_version"),
      .paid_fallback_enabled = in.required<bool>("paid_fallback_enabled"),
      .paid_fallback_limit_microcredits = in.required<double>("paid_fallback_limit_microcredits"),
      .paid_fallback_spent_microcredits = in.required<double>("paid_fallback_spent_microcredits"),
      .paid_fallback_reserved_microcredits = in.required<double>("paid_fallback_reserved_microcredits"),
      .paid_fallback_reservation_request_id = in.orNull<std::string>("paid_fallback_reservation_request_id"),
      .paid_fallback_model_ids = in.required<std::vector<std::string>>("paid_fallback_model_ids"),
      .paid_fallback_quota_per_credit = in.required<double>("paid_fallback_quota_per_credit"),
      .paid_fallback_max_exposure_microcredits = in.orUndefined<json::object>("paid_fallback_max_exposure_microcredits"),
      .paid_fallback_pricing_checked_at_ms = in.orNull<double>("paid_fallback_pricing_checked_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyHashRecord& value) {
    out = Writer()
      .required("id", value.id)
      .required("expires_at_ms", value.expires_at_ms)
      .orNull("revoked_at_ms", value.revoked_at_ms)
      .required("usage_limit_requests", value.usage_limit_requests)
      .required("usage_requests", value.usage_requests)
      .required("usage_reset_at_ms", value.usage_reset_at_ms)
      .required("window_ms", value.window_ms)
      .required("usage_quota_version", value.usage_quota_version)
      .required("paid_fallback_enabled", value.paid_fallback_enabled)
      .required("paid_fallback_limit_microcredits", value.paid_fallback_limit_microcredits)
      .required("paid_fallback_spent_microcredits", value.paid_fallback_spent_microcredits)
      .required("paid_fallback_reserved_microcredits", value.paid_fallback_reserved_microcredits)
      .orNull("paid_fallback_reservation_request_id", value.paid_fallback_reservation_request_id)
      .done();
  }

  ApiKeyHashRecord tag_invoke(const json::value_to_tag<ApiKeyHashRecord>&, const json::value& value) {
    const Reader in(value, "ApiKeyHashRecord");
    return {
      .id = in.required<std::string>("id"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .revoked_at_ms = in.orNull<double>("revoked_at_ms"),
      .usage_limit_requests = in.required<double>("usage_limit_requests"),
      .usage_requests = in.required<double>("usage_requests"),
      .usage_reset_at_ms = in.required<double>("usage_reset_at_ms"),
      .window_ms = in.required<double>("window_ms"),
      .usage_quota_version = in.required<double>("usage_quota_version"),
      .paid_fallback_enabled = in.required<bool>("paid_fallback_enabled"),
      .paid_fallback_limit_microcredits = in.required<double>("paid_fallback_limit_microcredits"),
      .paid_fallback_spent_microcredits = in.required<double>("paid_fallback_spent_microcredits"),
      .paid_fallback_reserved_microcredits = in.required<double>("paid_fallback_reserved_microcredits"),
      .paid_fallback_reservation_request_id = in.orNull<std::string>("paid_fallback_reservation_request_id"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyUsageWindowV3& value) {
    out = Writer()
      .required("v", value.v)
      .required("key_id", value.key_id)
      .required("policy_version", value.policy_version)
      .required("window_start_ms", value.window_start_ms)
      .required("window_reset_at_ms", value.window_reset_at_ms)
      .required("committed_requests", value.committed_requests)
      .required("reserved_requests", value.reserved_requests)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  ApiKeyUsageWindowV3 tag_invoke(const json::value_to_tag<ApiKeyUsageWindowV3>&, const json::value& value) {
    const Reader in(value, "ApiKeyUsageWindowV3");
    return {
      .v = in.required<double>("v"),
      .key_id = in.required<std::string>("key_id"),
      .policy_version = in.required<std::string>("policy_version"),
      .window_start_ms = in.required<double>("window_start_ms"),
      .window_reset_at_ms = in.required<double>("window_reset_at_ms"),
      .committed_requests = in.required<double>("committed_requests"),
      .reserved_requests = in.required<double>("reserved_requests"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyUsageRequestV3& value) {
    out = Writer()
      .required("v", value.v)
      .required("key_id", value.key_id)
      .required("request_id", value.request_id)
      .required("route", value.route)
      .required("state", value.state)
      .required("reserved_at_ms", value.reserved_at_ms)
      .required("lease_expires_at_ms", value.lease_expires_at_ms)
      .orNull("provider", value.provider)
      .orNull("dispatched_at_ms", value.dispatched_at_ms)
      .orNull("released_at_ms", value.released_at_ms)
      .orNull("release_reason", value.release_reason)
      .done();
  }

  ApiKeyUsageRequestV3 tag_invoke(const json::value_to_tag<ApiKeyUsageRequestV3>&, const json::value& value) {
    const Reader in(value, "ApiKeyUsageRequestV3");
    return {
      .v = in.required<double>("v"),
      .key_id = in.required<std::string>("key_id"),
      .request_id = in.required<std::string>("request_id"),
      .route = in.required<std::string>("route"),
      .state = in.required<std::string>("state"),
      .reserved_at_ms = in.required<double>("reserved_at_ms"),
      .lease_expires_at_ms = in.required<double>("lease_expires_at_ms"),
      .provider = in.orNull<std::string>("provider"),
      .dispatched_at_ms = in.orNull<double>("dispatched_at_ms"),
      .released_at_ms = in.orNull<double>("released_at_ms"),
      .release_reason = in.orNull<std::string>("release_reason"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackProviderUsageV3& value) {
    out = Writer()
      .required("request_count", value.request_count)
      .required("input_tokens", value.input_tokens)
      .required("output_tokens", value.output_tokens)
      .required("total_tokens", value.total_tokens)
      .required("spend_microcredits", value.spend_microcredits)
      .done();
  }

  PaidFallbackProviderUsageV3 tag_invoke(const json::value_to_tag<PaidFallbackProviderUsageV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackProviderUsageV3");
    return {
      .request_count = in.required<double>("request_count"),
      .input_tokens = in.required<double>("input_tokens"),
      .output_tokens = in.required<double>("output_tokens"),
      .total_tokens = in.required<double>("total_tokens"),
      .spend_microcredits = in.required<double>("spend_microcredits"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyRequestLogRecord& value) {
    out = Writer()
      .required("id", value.id)
      .required("key_id", value.key_id)
      .required("route", value.route)
      .required("path", value.path)
      .required("method", value.method)
      .required("status_code", value.status_code)
      .required("stream", value.stream)
      .orNull("model", value.model)
      .orNull("reasoning", value.reasoning)
      .required("created_at_ms", value.created_at_ms)
      .required("provider", value.provider)
      .orNull("fallback_reason", value.fallback_reason)
      .orNull("provider_request_id", value.provider_request_id)
      .orNull("completed_at_ms", value.completed_at_ms)
      .orNull("latency_ms", value.latency_ms)
      .orNull("input_tokens", value.input_tokens)
      .orNull("output_tokens", value.output_tokens)
      .orNull("provider_quota", value.provider_quota)
      .orNull("quota_per_credit", value.quota_per_credit)
      .orNull("spend_microcredits", value.spend_microcredits)
      .orNull("paid_fallback_window_reset_at_ms", value.paid_fallback_window_reset_at_ms)
      .required("billing_status", value.billing_status)
      .done();
  }

  ApiKeyRequestLogRecord tag_invoke(const json::value_to_tag<ApiKeyRequestLogRecord>&, const json::value& value) {
    const Reader in(value, "ApiKeyRequestLogRecord");
    return {
      .id = in.required<std::string>("id"),
      .key_id = in.required<std::string>("key_id"),
      .route = in.required<std::string>("route"),
      .path = in.required<std::string>("path"),
      .method = in.required<std::string>("method"),
      .status_code = in.required<double>("status_code"),
      .stream = in.required<bool>("stream"),
      .model = in.orNull<std::string>("model"),
      .reasoning = in.orNull<std::string>("reasoning"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .provider = in.required<std::string>("provider"),
      .fallback_reason = in.orNull<std::string>("fallback_reason"),
      .provider_request_id = in.orNull<std::string>("provider_request_id"),
      .completed_at_ms = in.orNull<double>("completed_at_ms"),
      .latency_ms = in.orNull<double>("latency_ms"),
      .input_tokens = in.orNull<double>("input_tokens"),
      .output_tokens = in.orNull<double>("output_tokens"),
      .provider_quota = in.orNull<double>("provider_quota"),
      .quota_per_credit = in.orNull<double>("quota_per_credit"),
      .spend_microcredits = in.orNull<double>("spend_microcredits"),
      .paid_fallback_window_reset_at_ms = in.orNull<double>("paid_fallback_window_reset_at_ms"),
      .billing_status = in.required<std::string>("billing_status"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackWindowV3& value) {
    out = Writer()
      .required("v", value.v)
      .required("key_id", value.key_id)
      .required("policy_version", value.policy_version)
      .required("window_reset_at_ms", value.window_reset_at_ms)
      .required("limit_microcredits", value.limit_microcredits)
      .required("settled_microcredits", value.settled_microcredits)
      .required("reserved_microcredits", value.reserved_microcredits)
      .required("pending_count", value.pending_count)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  PaidFallbackWindowV3 tag_invoke(const json::value_to_tag<PaidFallbackWindowV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackWindowV3");
    return {
      .v = in.required<double>("v"),
      .key_id = in.required<std::string>("key_id"),
      .policy_version = in.required<std::string>("policy_version"),
      .window_reset_at_ms = in.required<double>("window_reset_at_ms"),
      .limit_microcredits = in.required<double>("limit_microcredits"),
      .settled_microcredits = in.required<double>("settled_microcredits"),
      .reserved_microcredits = in.required<double>("reserved_microcredits"),
      .pending_count = in.required<double>("pending_count"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackRequestV3& value) {
    out = Writer()
      .required("v", value.v)
      .required("key_id", value.key_id)
      .required("request_id", value.request_id)
      .required("policy_version", value.policy_version)
      .required("route", value.route)
      .required("path", value.path)
      .required("model", value.model)
      .required("stream", value.stream)
      .orNull("reasoning", value.reasoning)
      .orUndefined("provider", value.provider)
      .required("window_reset_at_ms", value.window_reset_at_ms)
      .required("reserved_microcredits", value.reserved_microcredits)
      .required("quota_per_credit", value.quota_per_credit)
      .orNull("provider_request_id", value.provider_request_id)
      .orNull("provider_quota", value.provider_quota)
      .orNull("input_tokens", value.input_tokens)
      .nullable("cached_input_tokens", value.cached_input_tokens)
      .orNull("output_tokens", value.output_tokens)
      .required("dispatch_state", value.dispatch_state)
      .required("terminal_state", value.terminal_state)
      .orNull("spend_microcredits", value.spend_microcredits)
      .required("billing_state", value.billing_state)
      .required("reconciliation_attempts", value.reconciliation_attempts)
      .orNull("last_reconciliation_at_ms", value.last_reconciliation_at_ms)
      .orNull("dispatched_at_ms", value.dispatched_at_ms)
      .orNull("terminal_at_ms", value.terminal_at_ms)
      .orNull("settled_at_ms", value.settled_at_ms)
      .required("created_at_ms", value.created_at_ms)
      .required("updated_at_ms", value.updated_at_ms)
      .nullable("usage_rollup_at_ms", value.usage_rollup_at_ms)
      .done();
  }

  PaidFallbackRequestV3 tag_invoke(const json::value_to_tag<PaidFallbackRequestV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackRequestV3");
    return {
      .v = in.required<double>("v"),
      .key_id = in.required<std::string>("key_id"),
      .request_id = in.required<std::string>("request_id"),
      .policy_version = in.required<std::string>("policy_version"),
      .route = in.required<std::string>("route"),
      .path = in.required<std::string>("path"),
      .model = in.required<std::string>("model"),
      .stream = in.required<bool>("stream"),
      .reasoning = in.orNull<std::string>("reasoning"),
      .provider = in.orUndefined<std::string>("provider"),
      .window_reset_at_ms = in.required<double>("window_reset_at_ms"),
      .reserved_microcredits = in.required<double>("reserved_microcredits"),
      .quota_per_credit = in.required<double>("quota_per_credit"),
      .provider_request_id = in.orNull<std::string>("provider_request_id"),
      .provider_quota = in.orNull<double>("provider_quota"),
      .input_tokens = in.orNull<double>("input_tokens"),
      .cached_input_tokens = in.nullable<double>("cached_input_tokens"),
      .output_tokens = in.orNull<double>("output_tokens"),
      .dispatch_state = in.required<std::string>("dispatch_state"),
      .terminal_state = in.required<std::string>("terminal_state"),
      .spend_microcredits = in.orNull<double>("spend_microcredits"),
      .billing_state = in.required<std::string>("billing_state"),
      .reconciliation_attempts = in.required<double>("reconciliation_attempts"),
      .last_reconciliation_at_ms = in.orNull<double>("last_reconciliation_at_ms"),
      .dispatched_at_ms = in.orNull<double>("dispatched_at_ms"),
      .terminal_at_ms = in.orNull<double>("terminal_at_ms"),
      .settled_at_ms = in.orNull<double>("settled_at_ms"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
      .usage_rollup_at_ms = in.nullable<double>("usage_rollup_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAuthUsageRecord& value) {
    out = Writer()
      .required("owner", value.owner)
      .required("repo", value.repo)
      .required("total_requests", value.total_requests)
      .required("stream_requests", value.stream_requests)
      .required("non_stream_requests", value.non_stream_requests)
      .required("completed_requests", value.completed_requests)
      .required("error_requests", value.error_requests)
      .required("input_tokens", value.input_tokens)
      .required("output_tokens", value.output_tokens)
      .required("total_tokens", value.total_tokens)
      .required("first_seen_at_ms", value.first_seen_at_ms)
      .required("last_seen_at_ms", value.last_seen_at_ms)
      .orNull("last_model", value.last_model)
      .orNull("last_reasoning", value.last_reasoning)
      .orNull("last_route", value.last_route)
      .done();
  }

  KernelAuthUsageRecord tag_invoke(const json::value_to_tag<KernelAuthUsageRecord>&, const json::value& value) {
    const Reader in(value, "KernelAuthUsageRecord");
    return {
      .owner = in.required<std::string>("owner"),
      .repo = in.required<std::string>("repo"),
      .total_requests = in.required<double>("total_requests"),
      .stream_requests = in.required<double>("stream_requests"),
      .non_stream_requests = in.required<double>("non_stream_requests"),
      .completed_requests = in.required<double>("completed_requests"),
      .error_requests = in.required<double>("error_requests"),
      .input_tokens = in.required<double>("input_tokens"),
      .output_tokens = in.required<double>("output_tokens"),
      .total_tokens = in.required<double>("total_tokens"),
      .first_seen_at_ms = in.required<double>("first_seen_at_ms"),
      .last_seen_at_ms = in.required<double>("last_seen_at_ms"),
      .last_model = in.orNull<std::string>("last_model"),
      .last_reasoning = in.orNull<std::string>("last_reasoning"),
      .last_route = in.orNull<std::string>("last_route"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAuthUsageDay& value) {
    out = Writer()
      .required("day", value.day)
      .required("request_count", value.request_count)
      .done();
  }

  KernelAuthUsageDay tag_invoke(const json::value_to_tag<KernelAuthUsageDay>&, const json::value& value) {
    const Reader in(value, "KernelAuthUsageDay");
    return {
      .day = in.required<std::string>("day"),
      .request_count = in.required<double>("request_count"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAuthUsageDailyRecord& value) {
    out = Writer()
      .required("owner", value.owner)
      .required("repo", value.repo)
      .required("days", value.days)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  KernelAuthUsageDailyRecord tag_invoke(const json::value_to_tag<KernelAuthUsageDailyRecord>&, const json::value& value) {
    const Reader in(value, "KernelAuthUsageDailyRecord");
    return {
      .owner = in.required<std::string>("owner"),
      .repo = in.required<std::string>("repo"),
      .days = in.required<std::vector<KernelAuthUsageDay>>("days"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAuthLimitRecord& value) {
    out = Writer()
      .required("owner", value.owner)
      .required("repo", value.repo)
      .required("usage_limit_requests", value.usage_limit_requests)
      .required("usage_requests", value.usage_requests)
      .required("usage_reset_at_ms", value.usage_reset_at_ms)
      .required("window_ms", value.window_ms)
      .required("expires_at_ms", value.expires_at_ms)
      .required("created_at_ms", value.created_at_ms)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  KernelAuthLimitRecord tag_invoke(const json::value_to_tag<KernelAuthLimitRecord>&, const json::value& value) {
    const Reader in(value, "KernelAuthLimitRecord");
    return {
      .owner = in.required<std::string>("owner"),
      .repo = in.required<std::string>("repo"),
      .usage_limit_requests = in.required<double>("usage_limit_requests"),
      .usage_requests = in.required<double>("usage_requests"),
      .usage_reset_at_ms = in.required<double>("usage_reset_at_ms"),
      .window_ms = in.required<double>("window_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelOrgUsageRecord& value) {
    out = Writer()
      .required("owner", value.owner)
      .required("total_requests", value.total_requests)
      .required("stream_requests", value.stream_requests)
      .required("non_stream_requests", value.non_stream_requests)
      .required("completed_requests", value.completed_requests)
      .required("error_requests", value.error_requests)
      .required("input_tokens", value.input_tokens)
      .required("output_tokens", value.output_tokens)
      .required("total_tokens", value.total_tokens)
      .required("first_seen_at_ms", value.first_seen_at_ms)
      .required("last_seen_at_ms", value.last_seen_at_ms)
      .orNull("last_model", value.last_model)
      .orNull("last_reasoning", value.last_reasoning)
      .orNull("last_route", value.last_route)
      .done();
  }

  KernelOrgUsageRecord tag_invoke(const json::value_to_tag<KernelOrgUsageRecord>&, const json::value& value) {
    const Reader in(value, "KernelOrgUsageRecord");
    return {
      .owner = in.required<std::string>("owner"),
      .total_requests = in.required<double>("total_requests"),
      .stream_requests = in.required<double>("stream_requests"),
      .non_stream_requests = in.required<double>("non_stream_requests"),
      .completed_requests = in.required<double>("completed_requests"),
      .error_requests = in.required<double>("error_requests"),
      .input_tokens = in.required<double>("input_tokens"),
      .output_tokens = in.required<double>("output_tokens"),
      .total_tokens = in.required<double>("total_tokens"),
      .first_seen_at_ms = in.required<double>("first_seen_at_ms"),
      .last_seen_at_ms = in.required<double>("last_seen_at_ms"),
      .last_model = in.orNull<std::string>("last_model"),
      .last_reasoning = in.orNull<std::string>("last_reasoning"),
      .last_route = in.orNull<std::string>("last_route"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelOrgUsageDay& value) {
    out = Writer()
      .required("day", value.day)
      .required("request_count", value.request_count)
      .done();
  }

  KernelOrgUsageDay tag_invoke(const json::value_to_tag<KernelOrgUsageDay>&, const json::value& value) {
    const Reader in(value, "KernelOrgUsageDay");
    return {
      .day = in.required<std::string>("day"),
      .request_count = in.required<double>("request_count"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelOrgUsageDailyRecord& value) {
    out = Writer()
      .required("owner", value.owner)
      .required("days", value.days)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  KernelOrgUsageDailyRecord tag_invoke(const json::value_to_tag<KernelOrgUsageDailyRecord>&, const json::value& value) {
    const Reader in(value, "KernelOrgUsageDailyRecord");
    return {
      .owner = in.required<std::string>("owner"),
      .days = in.required<std::vector<KernelOrgUsageDay>>("days"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelOrgLimitRecord& value) {
    out = Writer()
      .required("owner", value.owner)
      .required("usage_limit_requests", value.usage_limit_requests)
      .required("usage_requests", value.usage_requests)
      .required("usage_reset_at_ms", value.usage_reset_at_ms)
      .required("window_ms", value.window_ms)
      .required("expires_at_ms", value.expires_at_ms)
      .required("created_at_ms", value.created_at_ms)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  KernelOrgLimitRecord tag_invoke(const json::value_to_tag<KernelOrgLimitRecord>&, const json::value& value) {
    const Reader in(value, "KernelOrgLimitRecord");
    return {
      .owner = in.required<std::string>("owner"),
      .usage_limit_requests = in.required<double>("usage_limit_requests"),
      .usage_requests = in.required<double>("usage_requests"),
      .usage_reset_at_ms = in.required<double>("usage_reset_at_ms"),
      .window_ms = in.required<double>("window_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelPolicyQueueItem& value) {
    out = Writer()
      .required("owner", value.owner)
      .required("repo", value.repo)
      .required("request_count", value.request_count)
      .required("first_seen_at_ms", value.first_seen_at_ms)
      .required("last_seen_at_ms", value.last_seen_at_ms)
      .orNull("last_route", value.last_route)
      .done();
  }

  KernelPolicyQueueItem tag_invoke(const json::value_to_tag<KernelPolicyQueueItem>&, const json::value& value) {
    const Reader in(value, "KernelPolicyQueueItem");
    return {
      .owner = in.required<std::string>("owner"),
      .repo = in.required<std::string>("repo"),
      .request_count = in.required<double>("request_count"),
      .first_seen_at_ms = in.required<double>("first_seen_at_ms"),
      .last_seen_at_ms = in.required<double>("last_seen_at_ms"),
      .last_route = in.orNull<std::string>("last_route"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheBreakpoint& value) {
    out = Writer()
      .required("mode", value.mode)
      .done();
  }

  PromptCacheBreakpoint tag_invoke(const json::value_to_tag<PromptCacheBreakpoint>&, const json::value& value) {
    const Reader in(value, "PromptCacheBreakpoint");
    return {
      .mode = in.required<std::string>("mode"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const MessageContentItem& value) {
    out = std::visit([](const auto& item) { return contentItemJson(item); }, value);
  }

  MessageContentItem tag_invoke(const json::value_to_tag<MessageContentItem>&, const json::value& value) {
    const Reader in(value, "MessageContentItem");
    const std::string type = in.required<std::string>("type");
    if (type == MessageContentItemInputText::type) {
      return MessageContentItemInputText{
        .text = in.required<std::string>("text"),
        .prompt_cache_breakpoint = in.orUndefined<PromptCacheBreakpoint>("prompt_cache_breakpoint"),
      };
    }
    if (type == MessageContentItemOutputText::type) return MessageContentItemOutputText{.text = in.required<std::string>("text")};
    if (type == MessageContentItemInputImage::type) {
      return MessageContentItemInputImage{
        .image_url = in.orUndefined<std::string>("image_url"),
        .file_id = in.orUndefined<std::string>("file_id"),
        .detail = in.nullable<std::string>("detail"),
        .prompt_cache_breakpoint = in.orUndefined<PromptCacheBreakpoint>("prompt_cache_breakpoint"),
      };
    }
    if (type == MessageContentItemInputFile::type) {
      return MessageContentItemInputFile{
        .file_id = in.orUndefined<std::string>("file_id"),
        .file_data = in.orUndefined<std::string>("file_data"),
        .file_url = in.orUndefined<std::string>("file_url"),
        .filename = in.nullable<std::string>("filename"),
        .detail = in.orUndefined<std::string>("detail"),
        .prompt_cache_breakpoint = in.orUndefined<PromptCacheBreakpoint>("prompt_cache_breakpoint"),
      };
    }
    in.fail("type", rt::get(value, "type"), "\"input_text\" | \"output_text\" | \"input_image\" | \"input_file\"");
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ResponseMessageItem& value) {
    out = Writer().required("type", std::string(value.type)).required("role", value.role).required("content", value.content).done();
  }

  ResponseMessageItem tag_invoke(const json::value_to_tag<ResponseMessageItem>&, const json::value& value) {
    const Reader in(value, "ResponseMessageItem");
    if (in.required<std::string>("type") != ResponseMessageItem::type) in.fail("type", rt::get(value, "type"), "\"message\"");
    return {
      .role = in.required<std::string>("role"),
      .content = in.required<std::vector<MessageContentItem>>("content"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ResponseInputItem& value) {
    if (const ResponseMessageItem* message = std::get_if<ResponseMessageItem>(&value)) out = json::value_from(*message);
    else out = rt::canonicalNumbers(json::value(std::get<json::object>(value)));
  }

  ResponseInputItem tag_invoke(const json::value_to_tag<ResponseInputItem>&, const json::value& value) {
    const Reader in(value, "ResponseInputItem");
    if (in.required<std::string>("type") == ResponseMessageItem::type) {
      try {
        return json::value_to<ResponseMessageItem>(value);
      } catch (const rt::TypeError&) {
        // Not a well-formed message item: it stays the record it is.
      }
    }
    return value.get_object();
  }
}

// Durable redemption validation is shared by reset admission and routing recovery.
namespace uos::codex::banked_reset {
  using ::uos::types::CodexResetRedemptionRecord;
  using ::uos::types::CodexResetRedemptionState;

  bool isSafeMs(const json::value& value) { return isSafeMs(&value); }
  bool isSafeMs(const json::value* value) { return value != nullptr && rt::isNumber(*value) && isSafeMs(rt::toNumber(*value)); }
  bool isSafeMs(double value) { return rt::Number::isSafeInteger(value) && value >= 0; }

  bool isSafeNonnegativeInteger(const json::value& value) { return isSafeNonnegativeInteger(&value); }
  bool isSafeNonnegativeInteger(const json::value* value) {
    return value != nullptr && rt::isNumber(*value) && isSafeNonnegativeInteger(rt::toNumber(*value));
  }
  bool isSafeNonnegativeInteger(double value) { return rt::Number::isSafeInteger(value) && value >= 0; }

  bool isNonEmptyText(const json::value& value, double max) { return isNonEmptyText(&value, max); }
  bool isNonEmptyText(const json::value* value, double max) {
    return value != nullptr && value->is_string() && isNonEmptyText(std::string_view(value->get_string()), max);
  }
  bool isNonEmptyText(std::string_view value, double max) {
    const auto length = static_cast<double>(rt::length(value));
    return length > 0 && length <= max;
  }

  namespace {
    using record_fields::isNull;
    using record_fields::numberOrNull;
    using record_fields::text;
    using record_fields::textOrNull;
    using record_fields::versionIsNotOne;
    // `value === "claimed" || ... || value === "rejected"`, as the narrowed state.
    std::optional<CodexResetRedemptionState> isState(const json::value* value) {
      if (value == nullptr || !value->is_string()) return std::nullopt;
      return ::uos::types::parseCodexResetRedemptionState(std::string_view(value->get_string()));
    }

    /**
     * A state is only durable when it agrees with the timestamps and error fields
     * that state allows. `isState` has already narrowed `state` to the five durable
     * states, so the default branch is unreachable; it assigns to a `never`-typed
     * local so a future state cannot be accepted without a compile error here.
     */
    bool redemptionRecordStateIsConsistent(CodexResetRedemptionState state, const std::optional<std::string>& providerReceiptId,
                                           std::optional<double> submittedAtMs, std::optional<double> verifiedAtMs,
                                           const std::optional<std::string>& lastErrorCode) {
      switch (state) {
        case CodexResetRedemptionState::claimed:
          return !providerReceiptId && !submittedAtMs && !verifiedAtMs && !lastErrorCode;
        case CodexResetRedemptionState::submitted:
          return submittedAtMs && !verifiedAtMs && !lastErrorCode;
        case CodexResetRedemptionState::unknown:
          return submittedAtMs && !verifiedAtMs && lastErrorCode;
        case CodexResetRedemptionState::verified:
          return submittedAtMs && verifiedAtMs && !lastErrorCode;
        case CodexResetRedemptionState::rejected:
          return !verifiedAtMs && lastErrorCode;
      }
      json::value unreachableState = json::value_from(state);
      throw rt::JsError("unhandled codex reset redemption state: " + rt::jsonStringify(unreachableState));
    }

    /** Ordering that every accepted record must satisfy, independent of its state. */
    bool redemptionRecordTimestampsAreOrdered(double createdAtMs, double updatedAtMs, std::optional<double> submittedAtMs, std::optional<double> verifiedAtMs) {
      return updatedAtMs >= createdAtMs && (!submittedAtMs || *submittedAtMs >= createdAtMs) && (!submittedAtMs || updatedAtMs >= *submittedAtMs) &&
             (!verifiedAtMs || (submittedAtMs && *verifiedAtMs >= *submittedAtMs)) && (!verifiedAtMs || updatedAtMs >= *verifiedAtMs);
    }

    /**
     * Field-by-field validation of one stored record. The predicate form keeps the
     * narrowing at the call site, so the parser only assembles an object it has
     * already proven field by field, in the same order as before.
     */
    bool hasValidRedemptionRecordFields(const json::object& value) {
      if (!isNonEmptyText(rt::get(value, "account_id_hash")) || !isNonEmptyText(rt::get(value, "credential_version")) ||
          !isNonEmptyText(rt::get(value, "quota_generation")) || !isNonEmptyText(rt::get(value, "idempotency_key_hash")) ||
          !isState(rt::get(value, "state")) || !isNonEmptyText(rt::get(value, "owner_token")) ||
          !isSafeNonnegativeInteger(rt::get(value, "routing_generation")) || !isSafeNonnegativeInteger(rt::get(value, "fence")) ||
          !isSafeMs(rt::get(value, "lease_expires_at_ms")) || !isSafeMs(rt::get(value, "created_at_ms")) || !isSafeMs(rt::get(value, "updated_at_ms")))
        return false;
      const json::value* providerReceiptId = rt::get(value, "provider_receipt_id");
      const json::value* submittedAtMs = rt::get(value, "submitted_at_ms");
      const json::value* verifiedAtMs = rt::get(value, "verified_at_ms");
      const json::value* lastErrorCode = rt::get(value, "last_error_code");
      if (!isNull(providerReceiptId) && !isNonEmptyText(providerReceiptId)) return false;
      if (!isNull(submittedAtMs) && !isSafeMs(submittedAtMs)) return false;
      if (!isNull(verifiedAtMs) && !isSafeMs(verifiedAtMs)) return false;
      if (!isNull(lastErrorCode) && !isNonEmptyText(lastErrorCode, 128)) return false;
      if (!redemptionRecordTimestampsAreOrdered(rt::toNumber(*rt::get(value, "created_at_ms")), rt::toNumber(*rt::get(value, "updated_at_ms")),
                                                numberOrNull(submittedAtMs), numberOrNull(verifiedAtMs)))
        return false;
      return redemptionRecordStateIsConsistent(*isState(rt::get(value, "state")), textOrNull(providerReceiptId), numberOrNull(submittedAtMs),
                                               numberOrNull(verifiedAtMs), textOrNull(lastErrorCode));
    }
  }

  std::optional<CodexResetRedemptionRecord> parseCodexResetRedemptionRecord(const json::value& value) { return parseCodexResetRedemptionRecord(&value); }

  std::optional<CodexResetRedemptionRecord> parseCodexResetRedemptionRecord(const json::value* value) {
    if (!value || !value->is_object() || versionIsNotOne(*value)) return std::nullopt;
    const json::object& fields = value->get_object();
    if (!hasValidRedemptionRecordFields(fields)) return std::nullopt;
    return CodexResetRedemptionRecord{
      .v = 1,
      .account_id_hash = text(rt::get(fields, "account_id_hash")),
      .credential_version = text(rt::get(fields, "credential_version")),
      .quota_generation = text(rt::get(fields, "quota_generation")),
      .routing_generation = rt::toNumber(*rt::get(fields, "routing_generation")),
      .idempotency_key_hash = text(rt::get(fields, "idempotency_key_hash")),
      .state = *isState(rt::get(fields, "state")),
      .owner_token = text(rt::get(fields, "owner_token")),
      .fence = rt::toNumber(*rt::get(fields, "fence")),
      .lease_expires_at_ms = rt::toNumber(*rt::get(fields, "lease_expires_at_ms")),
      .provider_receipt_id = textOrNull(rt::get(fields, "provider_receipt_id")),
      .created_at_ms = rt::toNumber(*rt::get(fields, "created_at_ms")),
      .updated_at_ms = rt::toNumber(*rt::get(fields, "updated_at_ms")),
      .submitted_at_ms = numberOrNull(rt::get(fields, "submitted_at_ms")),
      .verified_at_ms = numberOrNull(rt::get(fields, "verified_at_ms")),
      .last_error_code = textOrNull(rt::get(fields, "last_error_code")),
    };
  }
}
