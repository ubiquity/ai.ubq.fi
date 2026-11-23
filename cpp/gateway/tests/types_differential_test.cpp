// Differential cases for src/types.ts recorded from Bun 1.4.0. types.ts declares types
// only; its C++ port adds the JSON form of each record, so each case builds a record,
// checks value_from against the JSON.stringify output of the same object literal under
// Bun, and parses that JSON back with value_to. The listed TS tests import these types
// for modules that are not ported yet. The rejection cases pin the C++-only contract
// documented in uos/types.hpp (no JS counterpart: the TS casts without checking).
#include "uos/types.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace uos;
using namespace uos::types;

namespace {
  constexpr std::string_view CodexAuthStateJson = R"({"access_token":"at","refresh_token":"rt","account_id":"acct","updated_at_ms":1700000000123,"native_owner":{"codex_home":"/h","generation_hash":"g","native_refreshed_at":"2026-01-01T00:00:00.000Z","auth_mode":"chatgptAuthTokens","chatgpt_user_id":"u"}})";
  constexpr std::string_view CodexAuthStatePlainJson = R"({"access_token":"a2","refresh_token":"r2","account_id":"b","updated_at_ms":0})";
  constexpr std::string_view CodexAuthStateMinimalOwnerJson = R"({"access_token":"a3","refresh_token":"r3","account_id":"c","updated_at_ms":5,"native_owner":{"codex_home":"/x","generation_hash":"y"}})";
  constexpr std::string_view CodexAuthPoolStateJson = R"({"accounts":[{"access_token":"at","refresh_token":"rt","account_id":"acct","updated_at_ms":1700000000123,"native_owner":{"codex_home":"/h","generation_hash":"g","native_refreshed_at":"2026-01-01T00:00:00.000Z","auth_mode":"chatgptAuthTokens","chatgpt_user_id":"u"}},{"access_token":"a2","refresh_token":"r2","account_id":"b","updated_at_ms":0}],"updated_at_ms":1.5})";
  constexpr std::string_view CodexAuthPoolStateEmptyJson = R"({"accounts":[],"updated_at_ms":0})";
  constexpr std::string_view CodexResetRedemptionRecordJson = R"({"v":1,"account_id_hash":"h","credential_version":"cv","quota_generation":"qg","routing_generation":7,"idempotency_key_hash":"ik","state":"submitted","owner_token":"ot","fence":3,"lease_expires_at_ms":1e+21,"provider_receipt_id":null,"created_at_ms":10,"updated_at_ms":11,"submitted_at_ms":12,"verified_at_ms":null,"last_error_code":null})";
  constexpr std::string_view CodexResetGlobalDailyRecordJson = R"({"v":1,"day":"2026-11-16","submission_count":2,"updated_at_ms":9007199254740992})";
  constexpr std::string_view CodexResetShadowDecisionRecordJson = R"({"v":1,"episode_hash":"e","created_at_ms":1,"expires_at_ms":2,"decision_reason":"all_exhausted","selected_account_id_hash":"s","selected_credit_id_hash":null,"selected_credit_expires_at_ms":0.25,"fences":[{"slot":0,"account_id_hash":"a","quota_generation":"q","routing_generation":4,"quota_reset_at_ms":99},{"slot":1,"account_id_hash":"b","quota_generation":"r","routing_generation":5,"quota_reset_at_ms":100}]})";
  constexpr std::string_view ApiKeyRecordJson = R"({"id":"k","name":"n","prefix":"u_ab","hash":"h","created_at_ms":1,"expires_at_ms":-1,"revoked_at_ms":null,"usage_limit_requests":-1,"usage_requests":0,"usage_reset_at_ms":2,"window_ms":3600000,"usage_quota_version":3,"paid_fallback_enabled":true,"paid_fallback_limit_microcredits":1000000,"paid_fallback_spent_microcredits":0.5,"paid_fallback_reserved_microcredits":0,"paid_fallback_reservation_request_id":"req","paid_fallback_model_ids":["gpt-5","deepseek"],"paid_fallback_quota_per_credit":500000,"paid_fallback_max_exposure_microcredits":{"gpt-5":80,"deepseek":1.25},"paid_fallback_pricing_checked_at_ms":null})";
  constexpr std::string_view ApiKeyRecordWithoutJson = R"({"id":"k","name":"n","prefix":"u_ab","hash":"h","created_at_ms":1,"expires_at_ms":-1,"revoked_at_ms":77,"usage_limit_requests":-1,"usage_requests":0,"usage_reset_at_ms":2,"window_ms":3600000,"usage_quota_version":3,"paid_fallback_enabled":true,"paid_fallback_limit_microcredits":1000000,"paid_fallback_spent_microcredits":0.5,"paid_fallback_reserved_microcredits":0,"paid_fallback_reservation_request_id":"req","paid_fallback_model_ids":[],"paid_fallback_quota_per_credit":500000,"paid_fallback_pricing_checked_at_ms":78})";
  constexpr std::string_view ApiKeyHashRecordJson = R"({"id":"k","expires_at_ms":1,"revoked_at_ms":null,"usage_limit_requests":10,"usage_requests":2,"usage_reset_at_ms":3,"window_ms":4,"usage_quota_version":2,"paid_fallback_enabled":false,"paid_fallback_limit_microcredits":0,"paid_fallback_spent_microcredits":0,"paid_fallback_reserved_microcredits":0,"paid_fallback_reservation_request_id":null})";
  constexpr std::string_view ApiKeyUsageWindowV3Json = R"({"v":3,"key_id":"k","policy_version":"v3:p","window_start_ms":1,"window_reset_at_ms":2,"committed_requests":3,"reserved_requests":4,"updated_at_ms":5})";
  constexpr std::string_view ApiKeyUsageRequestV3Json = R"({"v":3,"key_id":"k","request_id":"r","route":"responses","state":"dispatched","reserved_at_ms":1,"lease_expires_at_ms":2,"provider":"removed_provider","dispatched_at_ms":3,"released_at_ms":null,"release_reason":null})";
  constexpr std::string_view ApiKeyUsageRequestV3NullJson = R"({"v":3,"key_id":"k","request_id":"r","route":"chat","state":"released","reserved_at_ms":1,"lease_expires_at_ms":2,"provider":null,"dispatched_at_ms":null,"released_at_ms":4,"release_reason":"client_closed"})";
  constexpr std::string_view PaidFallbackProviderUsageV3Json = R"({"request_count":1,"input_tokens":2,"output_tokens":3,"total_tokens":5,"spend_microcredits":0.30000000000000004})";
  constexpr std::string_view ApiKeyRequestLogRecordJson = R"({"id":"l","key_id":"k","route":"responses","path":"/v1/responses","method":"POST","status_code":200,"stream":true,"model":"m","reasoning":null,"created_at_ms":1,"provider":"surplus","fallback_reason":"quota","provider_request_id":null,"completed_at_ms":2,"latency_ms":1,"input_tokens":10,"output_tokens":null,"provider_quota":400000,"quota_per_credit":500000,"spend_microcredits":800000,"paid_fallback_window_reset_at_ms":null,"billing_status":"pending"})";
  constexpr std::string_view PaidFallbackWindowV3Json = R"({"v":3,"key_id":"k","policy_version":"p","window_reset_at_ms":1,"limit_microcredits":-1,"settled_microcredits":2,"reserved_microcredits":3,"pending_count":4,"updated_at_ms":5})";
  constexpr std::string_view PaidFallbackRequestV3Json = R"({"v":3,"key_id":"k","request_id":"r","policy_version":"p","route":"responses","path":"/v1/responses","model":"m","stream":false,"reasoning":"high","provider":"metered","window_reset_at_ms":1,"reserved_microcredits":80,"quota_per_credit":500000,"provider_request_id":"pr","provider_quota":null,"input_tokens":12,"cached_input_tokens":3,"output_tokens":4,"dispatch_state":"dispatched","terminal_state":"completed","spend_microcredits":null,"billing_state":"pending","reconciliation_attempts":0,"last_reconciliation_at_ms":null,"dispatched_at_ms":2,"terminal_at_ms":null,"settled_at_ms":null,"created_at_ms":1700000000000,"updated_at_ms":1700000000001,"usage_rollup_at_ms":null})";
  constexpr std::string_view PaidFallbackRequestV3LegacyJson = R"({"v":3,"key_id":"k","request_id":"r","policy_version":"p","route":"responses","path":"/v1/responses","model":"m","stream":false,"reasoning":"high","window_reset_at_ms":1,"reserved_microcredits":80,"quota_per_credit":500000,"provider_request_id":"pr","provider_quota":null,"input_tokens":12,"output_tokens":4,"dispatch_state":"dispatched","terminal_state":"completed","spend_microcredits":null,"billing_state":"pending","reconciliation_attempts":0,"last_reconciliation_at_ms":null,"dispatched_at_ms":2,"terminal_at_ms":null,"settled_at_ms":null,"created_at_ms":1700000000000,"updated_at_ms":1700000000001})";
  constexpr std::string_view PaidFallbackRequestV3NullCachedJson = R"({"v":3,"key_id":"k","request_id":"r","policy_version":"p","route":"responses","path":"/v1/responses","model":"m","stream":false,"reasoning":"high","provider":"metered","window_reset_at_ms":1,"reserved_microcredits":80,"quota_per_credit":500000,"provider_request_id":"pr","provider_quota":null,"input_tokens":12,"cached_input_tokens":null,"output_tokens":4,"dispatch_state":"dispatched","terminal_state":"completed","spend_microcredits":null,"billing_state":"pending","reconciliation_attempts":0,"last_reconciliation_at_ms":null,"dispatched_at_ms":2,"terminal_at_ms":null,"settled_at_ms":null,"created_at_ms":1700000000000,"updated_at_ms":1700000000001,"usage_rollup_at_ms":1700000000002})";
  constexpr std::string_view KernelAuthUsageRecordJson = R"({"owner":"o","repo":"r","total_requests":1,"stream_requests":2,"non_stream_requests":3,"completed_requests":4,"error_requests":5,"input_tokens":6,"output_tokens":7,"total_tokens":13,"first_seen_at_ms":8,"last_seen_at_ms":9,"last_model":"m","last_reasoning":null,"last_route":"r"})";
  constexpr std::string_view KernelAuthUsageDailyRecordJson = R"({"owner":"o","repo":"r","days":[{"day":"2026-11-15","request_count":3},{"day":"2026-11-16","request_count":0}],"updated_at_ms":1})";
  constexpr std::string_view KernelAuthLimitRecordJson = R"({"owner":"o","repo":"r","usage_limit_requests":1,"usage_requests":2,"usage_reset_at_ms":3,"window_ms":4,"expires_at_ms":5,"created_at_ms":6,"updated_at_ms":7})";
  constexpr std::string_view KernelOrgUsageRecordJson = R"({"owner":"o","total_requests":1,"stream_requests":2,"non_stream_requests":3,"completed_requests":4,"error_requests":5,"input_tokens":6,"output_tokens":7,"total_tokens":13,"first_seen_at_ms":8,"last_seen_at_ms":9,"last_model":null,"last_reasoning":"low","last_route":null})";
  constexpr std::string_view KernelOrgUsageDailyRecordJson = R"({"owner":"o","days":[{"day":"2026-11-16","request_count":12}],"updated_at_ms":2})";
  constexpr std::string_view KernelOrgLimitRecordJson = R"({"owner":"o","usage_limit_requests":1,"usage_requests":2,"usage_reset_at_ms":3,"window_ms":4,"expires_at_ms":5,"created_at_ms":6,"updated_at_ms":7})";
  constexpr std::string_view KernelPolicyQueueItemJson = R"({"owner":"o","repo":"r","request_count":1,"first_seen_at_ms":2,"last_seen_at_ms":3,"last_route":null})";
  constexpr std::string_view MessageContentItemsJson = R"([{"type":"input_text","text":"hi"},{"type":"input_text","text":"cached","prompt_cache_breakpoint":{"mode":"explicit"}},{"type":"output_text","text":"out"},{"type":"input_image","image_url":"https://x/i.png"},{"type":"input_image","image_url":"data:image/png;base64,AA==","file_id":"f","detail":null,"prompt_cache_breakpoint":{"mode":"explicit"}},{"type":"input_image","file_id":"f2","detail":"original"},{"type":"input_file","file_id":"file-1"},{"type":"input_file","file_data":"data","file_url":"https://f","filename":null,"detail":"low","prompt_cache_breakpoint":{"mode":"explicit"}},{"type":"input_file","filename":"a.pdf"}])";
  constexpr std::string_view ResponseMessageItemJson = R"({"type":"message","role":"developer","content":[{"type":"input_text","text":"hi"},{"type":"input_text","text":"cached","prompt_cache_breakpoint":{"mode":"explicit"}},{"type":"output_text","text":"out"}]})";
  constexpr std::string_view ResponseInputItemsJson = R"([{"type":"message","role":"developer","content":[{"type":"input_text","text":"hi"},{"type":"input_text","text":"cached","prompt_cache_breakpoint":{"mode":"explicit"}},{"type":"output_text","text":"out"}]},{"type":"function_call_output","call_id":"c","output":"{\"a\":1}"},{"type":"message","role":"system","content":"plain text"},{"type":"reasoning","summary":[],"id":"rs_1","n":1,"big":1e+300}])";

  template <class T> void checkJson(const T& value, std::string_view expected, const char* label) {
    const std::string written = rt::jsonStringify(json::value_from(value));
    if (written != expected) rt::test::report(__FILE__, __LINE__, std::string(label) + " value_from\n      actual:   " + written + "\n      expected: " + std::string(expected), false);
    const T parsed = json::value_to<T>(rt::jsonParse(expected));
    if (!(parsed == value)) rt::test::report(__FILE__, __LINE__, std::string(label) + " value_to differs from the record", false);
    const std::string again = rt::jsonStringify(json::value_from(parsed));
    if (again != expected) rt::test::report(__FILE__, __LINE__, std::string(label) + " round trip\n      actual:   " + again, false);
  }

  template <class T> std::string rejection(std::string_view text) {
    try {
      (void)json::value_to<T>(rt::jsonParse(text));
    } catch (const rt::TypeError& error) {
      return error.message;
    }
    return "<accepted>";
  }

  CodexAuthState ownerState() {
    return {
      .access_token = "at",
      .refresh_token = "rt",
      .account_id = "acct",
      .updated_at_ms = 1'700'000'000'123,
      .native_owner = CodexAuthState::NativeOwner{
        .codex_home = "/h",
        .generation_hash = "g",
        .native_refreshed_at = "2026-01-01T00:00:00.000Z",
        .auth_mode = "chatgptAuthTokens",
        .chatgpt_user_id = "u",
      },
    };
  }
  CodexAuthState plainState() { return {.access_token = "a2", .refresh_token = "r2", .account_id = "b", .updated_at_ms = 0}; }

  ApiKeyRecord apiKey() {
    json::object exposure;
    exposure["gpt-5"] = 80;
    exposure["deepseek"] = 1.25;
    return {
      .id = "k",
      .name = "n",
      .prefix = "u_ab",
      .hash = "h",
      .created_at_ms = 1,
      .expires_at_ms = -1,
      .revoked_at_ms = std::nullopt,
      .usage_limit_requests = -1,
      .usage_requests = 0,
      .usage_reset_at_ms = 2,
      .window_ms = 3'600'000,
      .usage_quota_version = 3,
      .paid_fallback_enabled = true,
      .paid_fallback_limit_microcredits = 1'000'000,
      .paid_fallback_spent_microcredits = 0.5,
      .paid_fallback_reserved_microcredits = 0,
      .paid_fallback_reservation_request_id = "req",
      .paid_fallback_model_ids = {"gpt-5", "deepseek"},
      .paid_fallback_quota_per_credit = 500000,
      .paid_fallback_max_exposure_microcredits = exposure,
      .paid_fallback_pricing_checked_at_ms = std::nullopt,
    };
  }

  PaidFallbackRequestV3 paidRequest() {
    return {
      .key_id = "k",
      .request_id = "r",
      .policy_version = "p",
      .route = "responses",
      .path = "/v1/responses",
      .model = "m",
      .stream = false,
      .reasoning = "high",
      .provider = "metered",
      .window_reset_at_ms = 1,
      .reserved_microcredits = 80,
      .quota_per_credit = 500000,
      .provider_request_id = "pr",
      .provider_quota = std::nullopt,
      .input_tokens = 12,
      .cached_input_tokens = 3.0,
      .output_tokens = 4,
      .dispatch_state = "dispatched",
      .terminal_state = "completed",
      .spend_microcredits = std::nullopt,
      .billing_state = "pending",
      .reconciliation_attempts = 0,
      .last_reconciliation_at_ms = std::nullopt,
      .dispatched_at_ms = 2,
      .terminal_at_ms = std::nullopt,
      .settled_at_ms = std::nullopt,
      .created_at_ms = 1'700'000'000'000,
      .updated_at_ms = 1'700'000'000'001,
      .usage_rollup_at_ms = nullptr,
    };
  }

  std::vector<MessageContentItem> contentItems() {
    return {
      MessageContentItemInputText{.text = "hi"},
      MessageContentItemInputText{.text = "cached", .prompt_cache_breakpoint = PromptCacheBreakpoint{}},
      MessageContentItemOutputText{.text = "out"},
      MessageContentItemInputImage{.image_url = "https://x/i.png"},
      MessageContentItemInputImage{.image_url = "data:image/png;base64,AA==", .file_id = "f", .detail = nullptr, .prompt_cache_breakpoint = PromptCacheBreakpoint{}},
      MessageContentItemInputImage{.file_id = "f2", .detail = std::string("original")},
      MessageContentItemInputFile{.file_id = "file-1"},
      MessageContentItemInputFile{.file_data = "data", .file_url = "https://f", .filename = nullptr, .detail = "low", .prompt_cache_breakpoint = PromptCacheBreakpoint{}},
      MessageContentItemInputFile{.filename = std::string("a.pdf")},
    };
  }

  template <class T> json::value arrayJson(const std::vector<T>& items) {
    json::array out;
    for (const T& item : items) out.push_back(json::value_from(item));
    return out;
  }
}

TEST("Codex auth records") {
  checkJson(ownerState(), CodexAuthStateJson, "CodexAuthState");
  checkJson(plainState(), CodexAuthStatePlainJson, "CodexAuthStatePlain");
  checkJson(CodexAuthState{.access_token = "a3", .refresh_token = "r3", .account_id = "c", .updated_at_ms = 5, .native_owner = CodexAuthState::NativeOwner{.codex_home = "/x", .generation_hash = "y"}},
            CodexAuthStateMinimalOwnerJson, "CodexAuthStateMinimalOwner");
  checkJson(CodexAuthPoolState{.accounts = {ownerState(), plainState()}, .updated_at_ms = 1.5}, CodexAuthPoolStateJson, "CodexAuthPoolState");
  checkJson(CodexAuthPoolState{.accounts = {}, .updated_at_ms = -0.0}, CodexAuthPoolStateEmptyJson, "CodexAuthPoolStateEmpty");
}

TEST("Codex reset records") {
  checkJson(CodexResetRedemptionRecord{
              .account_id_hash = "h",
              .credential_version = "cv",
              .quota_generation = "qg",
              .routing_generation = 7,
              .idempotency_key_hash = "ik",
              .state = CodexResetRedemptionState::submitted,
              .owner_token = "ot",
              .fence = 3,
              .lease_expires_at_ms = 1e21,
              .provider_receipt_id = std::nullopt,
              .created_at_ms = 10,
              .updated_at_ms = 11,
              .submitted_at_ms = 12,
              .verified_at_ms = std::nullopt,
              .last_error_code = std::nullopt,
            },
            CodexResetRedemptionRecordJson, "CodexResetRedemptionRecord");
  checkJson(CodexResetGlobalDailyRecord{.day = "2026-11-16", .submission_count = 2, .updated_at_ms = 9007199254740993.0}, CodexResetGlobalDailyRecordJson, "CodexResetGlobalDailyRecord");
  checkJson(CodexResetShadowDecisionRecord{
              .episode_hash = "e",
              .created_at_ms = 1,
              .expires_at_ms = 2,
              .decision_reason = "all_exhausted",
              .selected_account_id_hash = "s",
              .selected_credit_id_hash = std::nullopt,
              .selected_credit_expires_at_ms = 0.25,
              .fences =
                {
                  {.slot = 0, .account_id_hash = "a", .quota_generation = "q", .routing_generation = 4, .quota_reset_at_ms = 99},
                  {.slot = 1, .account_id_hash = "b", .quota_generation = "r", .routing_generation = 5, .quota_reset_at_ms = 100},
                },
            },
            CodexResetShadowDecisionRecordJson, "CodexResetShadowDecisionRecord");
}

TEST("CodexResetRedemptionState names") {
  const std::vector<std::pair<CodexResetRedemptionState, std::string>> names = {
    {CodexResetRedemptionState::claimed, "claimed"},
    {CodexResetRedemptionState::submitted, "submitted"},
    {CodexResetRedemptionState::unknown, "unknown"},
    {CodexResetRedemptionState::verified, "verified"},
    {CodexResetRedemptionState::rejected, "rejected"},
  };
  for (const auto& [state, name] : names) {
    EXPECT_EQ(std::string(toString(state)), name);
    EXPECT(parseCodexResetRedemptionState(name) == state);
  }
  EXPECT(parseCodexResetRedemptionState("Claimed") == std::nullopt);
  EXPECT(parseCodexResetRedemptionState("") == std::nullopt);
}

TEST("API key records") {
  checkJson(apiKey(), ApiKeyRecordJson, "ApiKeyRecord");
  ApiKeyRecord without = apiKey();
  without.paid_fallback_max_exposure_microcredits = std::nullopt;
  without.paid_fallback_model_ids = {};
  without.revoked_at_ms = 77;
  without.paid_fallback_pricing_checked_at_ms = 78;
  checkJson(without, ApiKeyRecordWithoutJson, "ApiKeyRecordWithout");
  checkJson(ApiKeyHashRecord{
              .id = "k",
              .expires_at_ms = 1,
              .revoked_at_ms = std::nullopt,
              .usage_limit_requests = 10,
              .usage_requests = 2,
              .usage_reset_at_ms = 3,
              .window_ms = 4,
              .usage_quota_version = 2,
              .paid_fallback_enabled = false,
              .paid_fallback_limit_microcredits = 0,
              .paid_fallback_spent_microcredits = 0,
              .paid_fallback_reserved_microcredits = 0,
              .paid_fallback_reservation_request_id = std::nullopt,
            },
            ApiKeyHashRecordJson, "ApiKeyHashRecord");
  checkJson(ApiKeyUsageWindowV3{.key_id = "k", .policy_version = "v3:p", .window_start_ms = 1, .window_reset_at_ms = 2, .committed_requests = 3, .reserved_requests = 4, .updated_at_ms = 5},
            ApiKeyUsageWindowV3Json, "ApiKeyUsageWindowV3");
  checkJson(ApiKeyUsageRequestV3{
              .key_id = "k",
              .request_id = "r",
              .route = "responses",
              .state = "dispatched",
              .reserved_at_ms = 1,
              .lease_expires_at_ms = 2,
              .provider = "removed_provider",
              .dispatched_at_ms = 3,
              .released_at_ms = std::nullopt,
              .release_reason = std::nullopt,
            },
            ApiKeyUsageRequestV3Json, "ApiKeyUsageRequestV3");
  checkJson(ApiKeyUsageRequestV3{
              .key_id = "k",
              .request_id = "r",
              .route = "chat",
              .state = "released",
              .reserved_at_ms = 1,
              .lease_expires_at_ms = 2,
              .provider = std::nullopt,
              .dispatched_at_ms = std::nullopt,
              .released_at_ms = 4,
              .release_reason = "client_closed",
            },
            ApiKeyUsageRequestV3NullJson, "ApiKeyUsageRequestV3Null");
  checkJson(ApiKeyRequestLogRecord{
              .id = "l",
              .key_id = "k",
              .route = "responses",
              .path = "/v1/responses",
              .method = "POST",
              .status_code = 200,
              .stream = true,
              .model = "m",
              .reasoning = std::nullopt,
              .created_at_ms = 1,
              .provider = "surplus",
              .fallback_reason = "quota",
              .provider_request_id = std::nullopt,
              .completed_at_ms = 2,
              .latency_ms = 1,
              .input_tokens = 10,
              .output_tokens = std::nullopt,
              .provider_quota = 400000,
              .quota_per_credit = 500000,
              .spend_microcredits = 800000,
              .paid_fallback_window_reset_at_ms = std::nullopt,
              .billing_status = "pending",
            },
            ApiKeyRequestLogRecordJson, "ApiKeyRequestLogRecord");
}

TEST("paid fallback records keep undefined and null apart") {
  checkJson(PaidFallbackProviderUsageV3{.request_count = 1, .input_tokens = 2, .output_tokens = 3, .total_tokens = 5, .spend_microcredits = 0.1 + 0.2}, PaidFallbackProviderUsageV3Json,
            "PaidFallbackProviderUsageV3");
  checkJson(PaidFallbackWindowV3{.key_id = "k", .policy_version = "p", .window_reset_at_ms = 1, .limit_microcredits = -1, .settled_microcredits = 2, .reserved_microcredits = 3, .pending_count = 4, .updated_at_ms = 5},
            PaidFallbackWindowV3Json, "PaidFallbackWindowV3");
  checkJson(paidRequest(), PaidFallbackRequestV3Json, "PaidFallbackRequestV3");
  PaidFallbackRequestV3 legacy = paidRequest();
  legacy.provider = std::nullopt;
  legacy.cached_input_tokens = rt::undefined;
  legacy.usage_rollup_at_ms = rt::undefined;
  checkJson(legacy, PaidFallbackRequestV3LegacyJson, "PaidFallbackRequestV3Legacy");
  PaidFallbackRequestV3 nullCached = paidRequest();
  nullCached.cached_input_tokens = nullptr;
  nullCached.usage_rollup_at_ms = 1'700'000'000'002.0;
  checkJson(nullCached, PaidFallbackRequestV3NullCachedJson, "PaidFallbackRequestV3NullCached");
}

TEST("kernel usage, limit and queue records") {
  checkJson(KernelAuthUsageRecord{
              .owner = "o",
              .repo = "r",
              .total_requests = 1,
              .stream_requests = 2,
              .non_stream_requests = 3,
              .completed_requests = 4,
              .error_requests = 5,
              .input_tokens = 6,
              .output_tokens = 7,
              .total_tokens = 13,
              .first_seen_at_ms = 8,
              .last_seen_at_ms = 9,
              .last_model = "m",
              .last_reasoning = std::nullopt,
              .last_route = "r",
            },
            KernelAuthUsageRecordJson, "KernelAuthUsageRecord");
  checkJson(KernelAuthUsageDailyRecord{.owner = "o", .repo = "r", .days = {{.day = "2026-11-15", .request_count = 3}, {.day = "2026-11-16", .request_count = 0}}, .updated_at_ms = 1},
            KernelAuthUsageDailyRecordJson, "KernelAuthUsageDailyRecord");
  checkJson(KernelAuthLimitRecord{.owner = "o", .repo = "r", .usage_limit_requests = 1, .usage_requests = 2, .usage_reset_at_ms = 3, .window_ms = 4, .expires_at_ms = 5, .created_at_ms = 6, .updated_at_ms = 7},
            KernelAuthLimitRecordJson, "KernelAuthLimitRecord");
  checkJson(KernelOrgUsageRecord{
              .owner = "o",
              .total_requests = 1,
              .stream_requests = 2,
              .non_stream_requests = 3,
              .completed_requests = 4,
              .error_requests = 5,
              .input_tokens = 6,
              .output_tokens = 7,
              .total_tokens = 13,
              .first_seen_at_ms = 8,
              .last_seen_at_ms = 9,
              .last_model = std::nullopt,
              .last_reasoning = "low",
              .last_route = std::nullopt,
            },
            KernelOrgUsageRecordJson, "KernelOrgUsageRecord");
  checkJson(KernelOrgUsageDailyRecord{.owner = "o", .days = {{.day = "2026-11-16", .request_count = 12}}, .updated_at_ms = 2}, KernelOrgUsageDailyRecordJson, "KernelOrgUsageDailyRecord");
  checkJson(KernelOrgLimitRecord{.owner = "o", .usage_limit_requests = 1, .usage_requests = 2, .usage_reset_at_ms = 3, .window_ms = 4, .expires_at_ms = 5, .created_at_ms = 6, .updated_at_ms = 7},
            KernelOrgLimitRecordJson, "KernelOrgLimitRecord");
  checkJson(KernelPolicyQueueItem{.owner = "o", .repo = "r", .request_count = 1, .first_seen_at_ms = 2, .last_seen_at_ms = 3, .last_route = std::nullopt}, KernelPolicyQueueItemJson,
            "KernelPolicyQueueItem");
}

TEST("Responses message items and input items") {
  const std::vector<MessageContentItem> items = contentItems();
  EXPECT_EQ(rt::jsonStringify(arrayJson(items)), std::string(MessageContentItemsJson));
  std::vector<MessageContentItem> parsed;
  const json::value itemsValue = rt::jsonParse(MessageContentItemsJson);
  for (const json::value& item : itemsValue.as_array()) parsed.push_back(json::value_to<MessageContentItem>(item));
  EXPECT(parsed == items);

  const ResponseMessageItem message{.role = "developer", .content = {items[0], items[1], items[2]}};
  checkJson(message, ResponseMessageItemJson, "ResponseMessageItem");

  json::object functionOutput;
  functionOutput["type"] = "function_call_output";
  functionOutput["call_id"] = "c";
  functionOutput["output"] = R"({"a":1})";
  json::object systemText;
  systemText["type"] = "message";
  systemText["role"] = "system";
  systemText["content"] = "plain text";
  json::object reasoning;
  reasoning["type"] = "reasoning";
  reasoning["summary"] = json::array();
  reasoning["id"] = "rs_1";
  reasoning["n"] = 1.0;
  reasoning["big"] = 1e300;
  const std::vector<ResponseInputItem> input = {message, functionOutput, systemText, reasoning};
  EXPECT_EQ(rt::jsonStringify(arrayJson(input)), std::string(ResponseInputItemsJson));
  std::vector<std::size_t> alternatives;
  std::vector<ResponseInputItem> parsedInput;
  const json::value inputValue = rt::jsonParse(ResponseInputItemsJson);
  for (const json::value& item : inputValue.as_array()) {
    parsedInput.push_back(json::value_to<ResponseInputItem>(item));
    alternatives.push_back(parsedInput.back().index());
  }
  EXPECT_EQ(alternatives, (std::vector<std::size_t>{0, 1, 1, 1}));
  EXPECT(std::get<ResponseMessageItem>(parsedInput[0]) == message);
  EXPECT_EQ(rt::jsonStringify(arrayJson(parsedInput)), std::string(ResponseInputItemsJson));
}

TEST("value_to rejects members the C++ type cannot hold") {
  EXPECT_EQ(rejection<CodexAuthState>(R"({"access_token":"a"})"), "CodexAuthState.refresh_token is undefined, not string");
  EXPECT_EQ(rejection<CodexAuthState>("[]"), "CodexAuthState is array, not object");
  EXPECT_EQ(rejection<CodexAuthState>("null"), "CodexAuthState is null, not object");
  EXPECT_EQ(rejection<CodexAuthState>(R"({"access_token":"a","refresh_token":"r","account_id":"b","updated_at_ms":"1"})"), "CodexAuthState.updated_at_ms is string, not number");
  EXPECT_EQ(rejection<CodexAuthState>(R"({"access_token":"a","refresh_token":"r","account_id":"b","updated_at_ms":1,"native_owner":null})"),
            "CodexAuthState.native_owner is null, not object or undefined");
  EXPECT_EQ(rejection<CodexAuthState>(R"({"access_token":"a","refresh_token":"r","account_id":"b","updated_at_ms":1,"native_owner":{"codex_home":"/h"}})"),
            "CodexAuthState.native_owner.generation_hash is undefined, not string");
  EXPECT_EQ(rejection<KernelPolicyQueueItem>(R"({"owner":"o","repo":"r","request_count":1,"first_seen_at_ms":2,"last_seen_at_ms":3})"),
            "KernelPolicyQueueItem.last_route is undefined, not string or null");
  EXPECT_EQ(rejection<KernelPolicyQueueItem>(R"({"owner":"o","repo":"r","request_count":1,"first_seen_at_ms":2,"last_seen_at_ms":3,"last_route":7})"),
            "KernelPolicyQueueItem.last_route is number, not string or null");
  json::value modelIds = rt::jsonParse(ApiKeyRecordJson);
  modelIds.as_object()["paid_fallback_model_ids"] = rt::jsonParse(R"(["a",1])");
  EXPECT_EQ(rejection<ApiKeyRecord>(rt::jsonStringify(modelIds)), "ApiKeyRecord.paid_fallback_model_ids[] is number, not string");
  json::value exposure = rt::jsonParse(ApiKeyRecordJson);
  exposure.as_object()["paid_fallback_max_exposure_microcredits"] = json::array();
  EXPECT_EQ(rejection<ApiKeyRecord>(rt::jsonStringify(exposure)), "ApiKeyRecord.paid_fallback_max_exposure_microcredits is array, not object or undefined");
  json::value state = rt::jsonParse(CodexResetRedemptionRecordJson);
  state.as_object()["state"] = "bogus";
  EXPECT_EQ(rejection<CodexResetRedemptionRecord>(rt::jsonStringify(state)), R"(CodexResetRedemptionState is string, not "claimed" | "submitted" | "unknown" | "verified" | "rejected")");
  rt::objectDelete(state.as_object(), "state");
  EXPECT_EQ(rejection<CodexResetRedemptionRecord>(rt::jsonStringify(state)), "CodexResetRedemptionRecord.state is undefined, not string");
  json::value cached = rt::jsonParse(PaidFallbackRequestV3Json);
  cached.as_object()["cached_input_tokens"] = "3";
  EXPECT_EQ(rejection<PaidFallbackRequestV3>(rt::jsonStringify(cached)), "PaidFallbackRequestV3.cached_input_tokens is string, not number, null or undefined");
  EXPECT_EQ(rejection<MessageContentItem>(R"({"type":"input_audio"})"), R"(MessageContentItem.type is string, not "input_text" | "output_text" | "input_image" | "input_file")");
  EXPECT_EQ(rejection<MessageContentItem>(R"({"type":"input_text"})"), "MessageContentItem.text is undefined, not string");
  EXPECT_EQ(rejection<ResponseMessageItem>(R"({"type":"reasoning","role":"user","content":[]})"), R"(ResponseMessageItem.type is string, not "message")");
  EXPECT_EQ(rejection<ResponseInputItem>(R"({"role":"user"})"), "ResponseInputItem.type is undefined, not string");
  EXPECT_EQ(rejection<ResponseInputItem>("1"), "ResponseInputItem is number, not object");
}

TEST("value_to ignores members the type does not declare") {
  json::value extra = rt::jsonParse(ApiKeyUsageWindowV3Json);
  extra.as_object()["legacy_counter"] = 9;
  const ApiKeyUsageWindowV3 window = json::value_to<ApiKeyUsageWindowV3>(extra);
  EXPECT_EQ(rt::jsonStringify(json::value_from(window)), std::string(ApiKeyUsageWindowV3Json));
  json::value version = rt::jsonParse(ApiKeyUsageWindowV3Json);
  version.as_object()["v"] = 2;
  EXPECT_EQ(json::value_to<ApiKeyUsageWindowV3>(version).v, 2.0);
}

ASYNC_TEST("rt::Kv typed reads use the record conversions") {
  rt::MemoryKv kv([] { return 1'700'000'000'000.0; });
  const ApiKeyUsageWindowV3 window{.key_id = "k", .policy_version = "v3:p", .window_start_ms = 1, .window_reset_at_ms = 2, .committed_requests = 3, .reserved_requests = 4, .updated_at_ms = 5};
  const rt::KvValue stored(json::value_from(window));
  co_await kv.set(rt::KvKey{"uos_ai", "window", "k"}, stored);
  const rt::KvEntryMaybe<std::optional<ApiKeyUsageWindowV3>> entry = co_await kv.get<std::optional<ApiKeyUsageWindowV3>>(rt::KvKey{"uos_ai", "window", "k"});
  EXPECT(entry.value == window);
  const rt::KvEntryMaybe<std::optional<ApiKeyUsageWindowV3>> missing = co_await kv.get<std::optional<ApiKeyUsageWindowV3>>(rt::KvKey{"uos_ai", "window", "absent"});
  EXPECT(missing.value == std::nullopt);
}
