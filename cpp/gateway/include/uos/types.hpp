#pragma once
// Records persisted in KV and items sent upstream. Each snake_case record also has
// boost::json conversions (json::value_from / json::value_to, which rt::Kv typed
// reads use): value_from writes the members in declaration order (an undefined
// optional member is omitted, a null one written as null, numbers canonical);
// value_to reads a JSON object and throws rt::TypeError naming the first member the
// C++ type cannot hold (absent where required, null where only undefined is allowed,
// another JS type). Unknown members are not kept: storage that may hold other
// shapes is read as json::value and validated first, as the TS type guards do.
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::types {
  struct CodexAuthState {
    struct NativeOwner {
      std::string codex_home;
      std::string generation_hash;
      std::optional<std::string> native_refreshed_at{};
      /** A native external consumer receives access authorization, never rotating credentials. */
      std::optional<std::string> auth_mode{};  // "chatgptAuthTokens"
      std::optional<std::string> chatgpt_user_id{};
      friend bool operator==(const NativeOwner&, const NativeOwner&) = default;
    };

    std::string access_token;
    std::string refresh_token;
    std::string account_id;
    double updated_at_ms;
    /** Local native Codex owns this account's rotating credential lineage. */
    std::optional<NativeOwner> native_owner{};
    friend bool operator==(const CodexAuthState&, const CodexAuthState&) = default;
  };

  struct CodexAuthPoolState {
    std::vector<CodexAuthState> accounts;
    double updated_at_ms;
    friend bool operator==(const CodexAuthPoolState&, const CodexAuthPoolState&) = default;
  };

  /**
   * A durable audit record for a banked Codex reset. This is intentionally
   * separate from account-routing state: routing answers whether an account is
   * currently usable, while this record fences an expensive external mutation.
   * It never contains an access token, refresh token, raw account identifier,
   * or raw provider idempotency key.
   */
  enum class CodexResetRedemptionState { claimed, submitted, unknown, verified, rejected };
  std::string_view toString(CodexResetRedemptionState state);
  // The state named by `text`, or std::nullopt for any other string.
  std::optional<CodexResetRedemptionState> parseCodexResetRedemptionState(std::string_view text);

  struct CodexResetRedemptionRecord {
    double v = 1;
    std::string account_id_hash;
    std::string credential_version;
    std::string quota_generation;
    double routing_generation;
    std::string idempotency_key_hash;
    CodexResetRedemptionState state;
    std::string owner_token;
    double fence;
    double lease_expires_at_ms;
    std::optional<std::string> provider_receipt_id;
    double created_at_ms;
    double updated_at_ms;
    std::optional<double> submitted_at_ms;
    std::optional<double> verified_at_ms;
    std::optional<std::string> last_error_code;
    friend bool operator==(const CodexResetRedemptionRecord&, const CodexResetRedemptionRecord&) = default;
  };

  /** A UTC-day budget consumed at the durable external-submission boundary. */
  struct CodexResetGlobalDailyRecord {
    double v = 1;
    std::string day;
    double submission_count;
    double updated_at_ms;
    friend bool operator==(const CodexResetGlobalDailyRecord&, const CodexResetGlobalDailyRecord&) = default;
  };

  /**
   * Redacted, read-only evidence of one complete account-pool exhaustion
   * episode. It is deliberately separate from the redemption ledger: this
   * record can attest a shadow decision but can never authorize a provider
   * consume on its own.
   */
  struct CodexResetShadowDecisionRecord {
    struct Fence {
      double slot;
      std::string account_id_hash;
      std::string quota_generation;
      double routing_generation;
      double quota_reset_at_ms;
      friend bool operator==(const Fence&, const Fence&) = default;
    };

    double v = 1;
    std::string episode_hash;
    double created_at_ms;
    double expires_at_ms;
    std::string decision_reason;
    std::optional<std::string> selected_account_id_hash;
    std::optional<std::string> selected_credit_id_hash;
    std::optional<double> selected_credit_expires_at_ms;
    std::vector<Fence> fences;
    friend bool operator==(const CodexResetShadowDecisionRecord&, const CodexResetShadowDecisionRecord&) = default;
  };

  struct ApiKeyRecord {
    std::string id;
    std::string name;
    std::string prefix;
    std::string hash;
    double created_at_ms;
    double expires_at_ms;
    std::optional<double> revoked_at_ms;
    double usage_limit_requests;
    double usage_requests;
    double usage_reset_at_ms;
    double window_ms;
    /**
     * Runtime request-quota ledger version. Version 3 reserves capacity before
     * route handling and commits it immediately before the first provider
     * transport; V2 counters are migration input only.
     */
    // Read back from KV, where rows written before the V2 -> V3 quota-ledger
    // migration still carry their old version. Typing this as the literal 3 made
    // the migration's own `!== 3` upgrade and validation checks look dead.
    double usage_quota_version;
    bool paid_fallback_enabled;
    double paid_fallback_limit_microcredits;
    double paid_fallback_spent_microcredits;
    double paid_fallback_reserved_microcredits;
    std::optional<std::string> paid_fallback_reservation_request_id;
    std::vector<std::string> paid_fallback_model_ids;
    double paid_fallback_quota_per_credit;
    std::optional<json::object> paid_fallback_max_exposure_microcredits{};  // Record<string, number>
    std::optional<double> paid_fallback_pricing_checked_at_ms;
    friend bool operator==(const ApiKeyRecord&, const ApiKeyRecord&) = default;
  };

  struct ApiKeyHashRecord {
    std::string id;
    double expires_at_ms;
    std::optional<double> revoked_at_ms;
    double usage_limit_requests;
    double usage_requests;
    double usage_reset_at_ms;
    double window_ms;
    // Read back from KV, where rows written before the V2 -> V3 quota-ledger
    // migration still carry their old version. Typing this as the literal 3 made
    // the migration's own `!== 3` upgrade and validation checks look dead.
    double usage_quota_version;
    bool paid_fallback_enabled;
    double paid_fallback_limit_microcredits;
    double paid_fallback_spent_microcredits;
    double paid_fallback_reserved_microcredits;
    std::optional<std::string> paid_fallback_reservation_request_id;
    friend bool operator==(const ApiKeyHashRecord&, const ApiKeyHashRecord&) = default;
  };

  struct ApiKeyUsageWindowV3 {
    double v = 3;
    std::string key_id;
    std::string policy_version;
    double window_start_ms;
    double window_reset_at_ms;
    double committed_requests;
    double reserved_requests;
    double updated_at_ms;
    friend bool operator==(const ApiKeyUsageWindowV3&, const ApiKeyUsageWindowV3&) = default;
  };

  struct ApiKeyUsageRequestV3 {
    double v = 3;
    std::string key_id;
    std::string request_id;
    std::string route;
    std::string state;  // "reserved" | "dispatched" | "released"
    double reserved_at_ms;
    double lease_expires_at_ms;
    // "cerebras" | "chatgpt_codex" | "claude_code" | "deepseek" | "gemini" | "lithos" |
    // "openrouter" | "removed_provider" | "metered" | "surplus" | "voyage" | null
    std::optional<std::string> provider;
    std::optional<double> dispatched_at_ms;
    std::optional<double> released_at_ms;
    std::optional<std::string> release_reason;
    friend bool operator==(const ApiKeyUsageRequestV3&, const ApiKeyUsageRequestV3&) = default;
  };

  using PaidFallbackProvider = std::string;  // "metered" | "surplus"

  struct PaidFallbackProviderUsageV3 {
    double request_count;
    double input_tokens;
    double output_tokens;
    double total_tokens;
    double spend_microcredits;
    friend bool operator==(const PaidFallbackProviderUsageV3&, const PaidFallbackProviderUsageV3&) = default;
  };

  struct ApiKeyRequestLogRecord {
    std::string id;
    std::string key_id;
    std::string route;
    std::string path;
    std::string method;
    double status_code;
    bool stream;
    std::optional<std::string> model;
    std::optional<std::string> reasoning;
    double created_at_ms;
    // "cerebras" | "chatgpt_codex" | "deepseek" | "lithos" | "openrouter" | "voyage" | PaidFallbackProvider
    std::string provider;
    std::optional<std::string> fallback_reason;
    std::optional<std::string> provider_request_id;
    std::optional<double> completed_at_ms;
    std::optional<double> latency_ms;
    std::optional<double> input_tokens;
    std::optional<double> output_tokens;
    std::optional<double> provider_quota;
    std::optional<double> quota_per_credit;
    std::optional<double> spend_microcredits;
    std::optional<double> paid_fallback_window_reset_at_ms;
    std::string billing_status;  // "not_applicable" | "pending" | "reconciled" | "not_billed" | "unresolved"
    friend bool operator==(const ApiKeyRequestLogRecord&, const ApiKeyRequestLogRecord&) = default;
  };

  struct PaidFallbackWindowV3 {
    double v = 3;
    std::string key_id;
    std::string policy_version;
    double window_reset_at_ms;
    double limit_microcredits;
    double settled_microcredits;
    double reserved_microcredits;
    double pending_count;
    double updated_at_ms;
    friend bool operator==(const PaidFallbackWindowV3&, const PaidFallbackWindowV3&) = default;
  };

  struct PaidFallbackRequestV3 {
    double v = 3;
    std::string key_id;
    std::string request_id;
    std::string policy_version;
    std::string route;
    std::string path;
    std::string model;
    bool stream;
    std::optional<std::string> reasoning;
    // Added after the V3 ledger launch. Missing values are legacy Metered rows.
    std::optional<PaidFallbackProvider> provider{};
    double window_reset_at_ms;
    double reserved_microcredits;
    double quota_per_credit;
    std::optional<std::string> provider_request_id;
    std::optional<double> provider_quota;
    std::optional<double> input_tokens;
    rt::Nullable<double> cached_input_tokens{};
    std::optional<double> output_tokens;
    std::string dispatch_state;  // "reserved" | "dispatched" | "not_dispatched"
    std::string terminal_state;  // "pending" | "completed" | "failed" | "incomplete" | "cancelled" | "ambiguous"
    std::optional<double> spend_microcredits;
    std::string billing_state;  // "pending" | "settled" | "not_billed" | "unresolved"
    double reconciliation_attempts;
    std::optional<double> last_reconciliation_at_ms;
    std::optional<double> dispatched_at_ms;
    std::optional<double> terminal_at_ms;
    std::optional<double> settled_at_ms;
    double created_at_ms;
    double updated_at_ms;
    /**
     * Set when this row's usage was folded into the hourly rollup, either by the
     * settlement write or by the one-time backfill. Prevents a later backfill
     * run from double-counting settle-path rows.
     */
    rt::Nullable<double> usage_rollup_at_ms{};
    friend bool operator==(const PaidFallbackRequestV3&, const PaidFallbackRequestV3&) = default;
  };

  struct KernelAuthUsageRecord {
    std::string owner;
    std::string repo;
    double total_requests;
    double stream_requests;
    double non_stream_requests;
    double completed_requests;
    double error_requests;
    double input_tokens;
    double output_tokens;
    double total_tokens;
    double first_seen_at_ms;
    double last_seen_at_ms;
    std::optional<std::string> last_model;
    std::optional<std::string> last_reasoning;
    std::optional<std::string> last_route;
    friend bool operator==(const KernelAuthUsageRecord&, const KernelAuthUsageRecord&) = default;
  };

  struct KernelAuthUsageDay {
    std::string day;
    double request_count;
    friend bool operator==(const KernelAuthUsageDay&, const KernelAuthUsageDay&) = default;
  };

  struct KernelAuthUsageDailyRecord {
    std::string owner;
    std::string repo;
    std::vector<KernelAuthUsageDay> days;
    double updated_at_ms;
    friend bool operator==(const KernelAuthUsageDailyRecord&, const KernelAuthUsageDailyRecord&) = default;
  };

  struct KernelAuthLimitRecord {
    std::string owner;
    std::string repo;
    double usage_limit_requests;
    double usage_requests;
    double usage_reset_at_ms;
    double window_ms;
    double expires_at_ms;
    double created_at_ms;
    double updated_at_ms;
    friend bool operator==(const KernelAuthLimitRecord&, const KernelAuthLimitRecord&) = default;
  };

  struct KernelOrgUsageRecord {
    std::string owner;
    double total_requests;
    double stream_requests;
    double non_stream_requests;
    double completed_requests;
    double error_requests;
    double input_tokens;
    double output_tokens;
    double total_tokens;
    double first_seen_at_ms;
    double last_seen_at_ms;
    std::optional<std::string> last_model;
    std::optional<std::string> last_reasoning;
    std::optional<std::string> last_route;
    friend bool operator==(const KernelOrgUsageRecord&, const KernelOrgUsageRecord&) = default;
  };

  struct KernelOrgUsageDay {
    std::string day;
    double request_count;
    friend bool operator==(const KernelOrgUsageDay&, const KernelOrgUsageDay&) = default;
  };

  struct KernelOrgUsageDailyRecord {
    std::string owner;
    std::vector<KernelOrgUsageDay> days;
    double updated_at_ms;
    friend bool operator==(const KernelOrgUsageDailyRecord&, const KernelOrgUsageDailyRecord&) = default;
  };

  struct KernelOrgLimitRecord {
    std::string owner;
    double usage_limit_requests;
    double usage_requests;
    double usage_reset_at_ms;
    double window_ms;
    double expires_at_ms;
    double created_at_ms;
    double updated_at_ms;
    friend bool operator==(const KernelOrgLimitRecord&, const KernelOrgLimitRecord&) = default;
  };

  struct KernelPolicyQueueItem {
    std::string owner;
    std::string repo;
    double request_count;
    double first_seen_at_ms;
    double last_seen_at_ms;
    std::optional<std::string> last_route;
    friend bool operator==(const KernelPolicyQueueItem&, const KernelPolicyQueueItem&) = default;
  };

  // Views over the parsed request body (the same object as the raw record): the TS reads
  // the optional `unknown` members model, messages, reasoning_effort and stream through it.
  using ChatCompletionRequest = json::object;

  // Optional `unknown` members client_metadata, model, input, instructions, reasoning, stream.
  using ResponsesRequest = json::object;

  struct PromptCacheBreakpoint {
    std::string mode = "explicit";
    friend bool operator==(const PromptCacheBreakpoint&, const PromptCacheBreakpoint&) = default;
  };

  struct MessageContentItemInputText {
    static constexpr std::string_view type = "input_text";
    std::string text;
    std::optional<PromptCacheBreakpoint> prompt_cache_breakpoint{};
    friend bool operator==(const MessageContentItemInputText&, const MessageContentItemInputText&) = default;
  };

  struct MessageContentItemOutputText {
    static constexpr std::string_view type = "output_text";
    std::string text;
    friend bool operator==(const MessageContentItemOutputText&, const MessageContentItemOutputText&) = default;
  };

  struct MessageContentItemInputImage {
    static constexpr std::string_view type = "input_image";
    std::optional<std::string> image_url{};
    std::optional<std::string> file_id{};
    rt::Nullable<std::string> detail{};  // "auto" | "low" | "high" | "original" | null
    std::optional<PromptCacheBreakpoint> prompt_cache_breakpoint{};
    friend bool operator==(const MessageContentItemInputImage&, const MessageContentItemInputImage&) = default;
  };

  struct MessageContentItemInputFile {
    static constexpr std::string_view type = "input_file";
    std::optional<std::string> file_id{};
    std::optional<std::string> file_data{};
    std::optional<std::string> file_url{};
    rt::Nullable<std::string> filename{};
    std::optional<std::string> detail{};  // "auto" | "low" | "high"
    std::optional<PromptCacheBreakpoint> prompt_cache_breakpoint{};
    friend bool operator==(const MessageContentItemInputFile&, const MessageContentItemInputFile&) = default;
  };

  using MessageContentItem = std::variant<MessageContentItemInputText, MessageContentItemOutputText, MessageContentItemInputImage, MessageContentItemInputFile>;

  struct ResponseMessageItem {
    static constexpr std::string_view type = "message";
    std::string role;  // "user" | "assistant" | "developer"
    std::vector<MessageContentItem> content;
    friend bool operator==(const ResponseMessageItem&, const ResponseMessageItem&) = default;
  };

  // The Responses API can accept additional input item types beyond "message"
  // (e.g. tool-calling items like reasoning/function_call/function_call_output).
  // The second alternative is any record with a string `type`, kept as is.
  using ResponseInputItem = std::variant<ResponseMessageItem, json::object>;

  // ---- JSON conversions (see the file comment) -------------------------------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthState::NativeOwner& value);
  CodexAuthState::NativeOwner tag_invoke(const json::value_to_tag<CodexAuthState::NativeOwner>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthState& value);
  CodexAuthState tag_invoke(const json::value_to_tag<CodexAuthState>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthPoolState& value);
  CodexAuthPoolState tag_invoke(const json::value_to_tag<CodexAuthPoolState>&, const json::value& value);
  // A JSON string naming the state; any other value throws rt::TypeError.
  void tag_invoke(const json::value_from_tag&, json::value& out, CodexResetRedemptionState value);
  CodexResetRedemptionState tag_invoke(const json::value_to_tag<CodexResetRedemptionState>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexResetRedemptionRecord& value);
  CodexResetRedemptionRecord tag_invoke(const json::value_to_tag<CodexResetRedemptionRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexResetGlobalDailyRecord& value);
  CodexResetGlobalDailyRecord tag_invoke(const json::value_to_tag<CodexResetGlobalDailyRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexResetShadowDecisionRecord::Fence& value);
  CodexResetShadowDecisionRecord::Fence tag_invoke(const json::value_to_tag<CodexResetShadowDecisionRecord::Fence>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexResetShadowDecisionRecord& value);
  CodexResetShadowDecisionRecord tag_invoke(const json::value_to_tag<CodexResetShadowDecisionRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyRecord& value);
  ApiKeyRecord tag_invoke(const json::value_to_tag<ApiKeyRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyHashRecord& value);
  ApiKeyHashRecord tag_invoke(const json::value_to_tag<ApiKeyHashRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyUsageWindowV3& value);
  ApiKeyUsageWindowV3 tag_invoke(const json::value_to_tag<ApiKeyUsageWindowV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyUsageRequestV3& value);
  ApiKeyUsageRequestV3 tag_invoke(const json::value_to_tag<ApiKeyUsageRequestV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackProviderUsageV3& value);
  PaidFallbackProviderUsageV3 tag_invoke(const json::value_to_tag<PaidFallbackProviderUsageV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyRequestLogRecord& value);
  ApiKeyRequestLogRecord tag_invoke(const json::value_to_tag<ApiKeyRequestLogRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackWindowV3& value);
  PaidFallbackWindowV3 tag_invoke(const json::value_to_tag<PaidFallbackWindowV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackRequestV3& value);
  PaidFallbackRequestV3 tag_invoke(const json::value_to_tag<PaidFallbackRequestV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAuthUsageRecord& value);
  KernelAuthUsageRecord tag_invoke(const json::value_to_tag<KernelAuthUsageRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAuthUsageDay& value);
  KernelAuthUsageDay tag_invoke(const json::value_to_tag<KernelAuthUsageDay>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAuthUsageDailyRecord& value);
  KernelAuthUsageDailyRecord tag_invoke(const json::value_to_tag<KernelAuthUsageDailyRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAuthLimitRecord& value);
  KernelAuthLimitRecord tag_invoke(const json::value_to_tag<KernelAuthLimitRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelOrgUsageRecord& value);
  KernelOrgUsageRecord tag_invoke(const json::value_to_tag<KernelOrgUsageRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelOrgUsageDay& value);
  KernelOrgUsageDay tag_invoke(const json::value_to_tag<KernelOrgUsageDay>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelOrgUsageDailyRecord& value);
  KernelOrgUsageDailyRecord tag_invoke(const json::value_to_tag<KernelOrgUsageDailyRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelOrgLimitRecord& value);
  KernelOrgLimitRecord tag_invoke(const json::value_to_tag<KernelOrgLimitRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelPolicyQueueItem& value);
  KernelPolicyQueueItem tag_invoke(const json::value_to_tag<KernelPolicyQueueItem>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheBreakpoint& value);
  PromptCacheBreakpoint tag_invoke(const json::value_to_tag<PromptCacheBreakpoint>&, const json::value& value);
  // The alternative is chosen by `type`.
  void tag_invoke(const json::value_from_tag&, json::value& out, const MessageContentItem& value);
  MessageContentItem tag_invoke(const json::value_to_tag<MessageContentItem>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ResponseMessageItem& value);
  ResponseMessageItem tag_invoke(const json::value_to_tag<ResponseMessageItem>&, const json::value& value);
  // A record with `type: "message"` that converts to ResponseMessageItem becomes one;
  // any other record with a string `type` stays a json::object.
  void tag_invoke(const json::value_from_tag&, json::value& out, const ResponseInputItem& value);
  ResponseInputItem tag_invoke(const json::value_to_tag<ResponseInputItem>&, const json::value& value);
}
