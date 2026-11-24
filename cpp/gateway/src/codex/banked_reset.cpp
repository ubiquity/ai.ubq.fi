// Port of src/codex/banked-reset.ts.
#include "uos/codex/banked_reset.hpp"

#include "banked_reset_record_fields.hpp"

#include "uos/codex/reset_settings.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <utility>

namespace uos::codex::banked_reset {
  namespace {
    using ::uos::types::CodexResetGlobalDailyRecord;
    using ::uos::types::CodexResetRedemptionRecord;
    using ::uos::types::CodexResetShadowDecisionRecord;
  }

  rt::KvKey codexResetGlobalDailyKey(const std::string& day) { return rt::KvKey{CODEX_RESET_GLOBAL_DAILY_KV_PREFIX, day}; }

  rt::KvKey codexResetAccountDailyKey(const std::string& accountIdHash, const std::string& day) {
    return rt::KvKey{CODEX_RESET_ACCOUNT_DAILY_KV_PREFIX, accountIdHash, day};
  }

  rt::KvKey codexResetShadowDecisionKey(const std::string& episodeHash) { return rt::KvKey{CODEX_RESET_SHADOW_DECISION_KV_PREFIX, episodeHash}; }

  CodexBankedResetConfig loadCodexBankedResetConfig() {
    return CodexBankedResetConfig{
      .enabled = true,
      .mode = "live",
      .maxPerAccountPerDay = 1,
      .maxPerAccountPerWindow = 1,
    };
  }

  rt::Task<ReadUsageGateResult> readUsageGate(std::shared_ptr<rt::Kv> kv, std::string accountIdHash) {
    return readUsageGate(std::move(kv), std::move(accountIdHash), std::nullopt);
  }

  rt::Task<ReadUsageGateResult> readUsageGate(std::shared_ptr<rt::Kv> kv, std::string accountIdHash,
                                              std::optional<CodexBankedResetCandidate::Authorization> authorization) {
    if (authorization && authorization->accountIdHash == accountIdHash) co_return ReadUsageGateResultAllowed{};
    std::optional<reset_settings::ReadCodexResetUsageResult> usage;
    bool unavailable = false;
    try {
      rt::Promise<reset_settings::ReadCodexResetUsageResult> reading(reset_settings::readCodexResetUsage(kv, accountIdHash));
      usage = co_await reading.wait();
    } catch (...) {
      unavailable = true;
    }
    if (unavailable) co_return ReadUsageGateResultFailure{.code = "configuration_unavailable"};
    if (!usage->allowed) co_return ReadUsageGateResultFailure{.code = "usage_disabled"};
    co_return ReadUsageGateResultAllowed{.entries = std::move(usage->entries)};
  }

  const CodexBankedResetTelemetry defaultTelemetry{
    .event =
      [](CodexBankedResetEvent eventName, CodexBankedResetTelemetryFields fields) {
        try {
          json::object head;
          head["event"] = json::string(eventName);
          const json::object payload = rt::objectSpread(head, fields);
          rt::console::info("[ai.ubq.fi] codex_banked_reset", rt::jsonStringify(payload));
        } catch (...) {
          // Telemetry may never make a reset safer-looking or less safe.
        }
      },
    .metric =
      [](CodexBankedResetMetric metricName, double value, CodexBankedResetTelemetryFields fields) {
        try {
          json::object head;
          head["metric"] = json::string(metricName);
          head["value"] = rt::makeNumber(value);
          const json::object payload = rt::objectSpread(head, fields);
          rt::console::info("[ai.ubq.fi] codex_banked_reset_metric", rt::jsonStringify(payload));
        } catch (...) {
          // Metrics are deliberately best effort.
        }
      },
  };

  namespace {
    using record_fields::isNull;
    using record_fields::numberOrNull;
    using record_fields::text;
    using record_fields::textOrNull;
    using record_fields::versionIsNotOne;
  }

  std::optional<CodexResetGlobalDailyRecord> parseAccountDayRecord(const json::value& value, const std::string& day) {
    return parseAccountDayRecord(&value, day);
  }

  std::optional<CodexResetGlobalDailyRecord> parseAccountDayRecord(const json::value* value, const std::string& day) {
    if (!utils::isRecord(value) || versionIsNotOne(*value)) return std::nullopt;
    const json::value* storedDay = rt::get(*value, "day");
    if (storedDay == nullptr || !storedDay->is_string() || std::string_view(storedDay->get_string()) != day) return std::nullopt;
    if (!isSafeMs(rt::get(*value, "updated_at_ms"))) return std::nullopt;
    if (!isSafeNonnegativeInteger(rt::get(*value, "submission_count"))) return std::nullopt;
    return CodexResetGlobalDailyRecord{
      .v = 1,
      .day = day,
      .submission_count = rt::toNumber(*rt::get(*value, "submission_count")),
      .updated_at_ms = rt::toNumber(*rt::get(*value, "updated_at_ms")),
    };
  }

  namespace {
    std::optional<CodexResetShadowDecisionRecord::Fence> parseShadowDecisionFence(const json::value& value) {
      if (!utils::isRecord(value) || !isSafeNonnegativeInteger(rt::get(value, "slot")) || !isNonEmptyText(rt::get(value, "account_id_hash")) ||
          !isNonEmptyText(rt::get(value, "quota_generation")) || !isSafeNonnegativeInteger(rt::get(value, "routing_generation")) ||
          !isSafeMs(rt::get(value, "quota_reset_at_ms")))
        return std::nullopt;
      return CodexResetShadowDecisionRecord::Fence{
        .slot = rt::toNumber(*rt::get(value, "slot")),
        .account_id_hash = text(rt::get(value, "account_id_hash")),
        .quota_generation = text(rt::get(value, "quota_generation")),
        .routing_generation = rt::toNumber(*rt::get(value, "routing_generation")),
        .quota_reset_at_ms = rt::toNumber(*rt::get(value, "quota_reset_at_ms")),
      };
    }
  }

  std::optional<CodexResetShadowDecisionRecord> parseCodexResetShadowDecisionRecord(const json::value& value) {
    return parseCodexResetShadowDecisionRecord(&value);
  }

  std::optional<CodexResetShadowDecisionRecord> parseCodexResetShadowDecisionRecord(const json::value* value) {
    if (!utils::isRecord(value) || versionIsNotOne(*value) || !isNonEmptyText(rt::get(*value, "episode_hash")) ||
        !isSafeMs(rt::get(*value, "created_at_ms")) || !isSafeMs(rt::get(*value, "expires_at_ms")) ||
        rt::toNumber(*rt::get(*value, "expires_at_ms")) < rt::toNumber(*rt::get(*value, "created_at_ms")) ||
        !isNonEmptyText(rt::get(*value, "decision_reason"), 128) || rt::getArray(*value, "fences") == nullptr)
      return std::nullopt;
    const json::value* selectedAccountIdHash = rt::get(*value, "selected_account_id_hash");
    const json::value* selectedCreditIdHash = rt::get(*value, "selected_credit_id_hash");
    const json::value* selectedCreditExpiresAtMs = rt::get(*value, "selected_credit_expires_at_ms");
    if (!isNull(selectedAccountIdHash) && !isNonEmptyText(selectedAccountIdHash)) return std::nullopt;
    if (!isNull(selectedCreditIdHash) && !isNonEmptyText(selectedCreditIdHash)) return std::nullopt;
    if (!isNull(selectedCreditExpiresAtMs) && !isSafeMs(selectedCreditExpiresAtMs)) return std::nullopt;
    if (isNull(selectedAccountIdHash) != isNull(selectedCreditIdHash)) return std::nullopt;
    std::vector<std::optional<CodexResetShadowDecisionRecord::Fence>> fences;
    for (const json::value& fence : *rt::getArray(*value, "fences")) fences.push_back(parseShadowDecisionFence(fence));
    if (fences.empty() || std::any_of(fences.begin(), fences.end(), [](const auto& fence) { return !fence; })) return std::nullopt;
    std::vector<CodexResetShadowDecisionRecord::Fence> parsedFences;
    for (const auto& fence : fences) parsedFences.push_back(*fence);
    std::vector<double> slots;
    for (const CodexResetShadowDecisionRecord::Fence& fence : parsedFences) {
      if (std::find(slots.begin(), slots.end(), fence.slot) != slots.end()) return std::nullopt;
      slots.push_back(fence.slot);
    }
    return CodexResetShadowDecisionRecord{
      .v = 1,
      .episode_hash = text(rt::get(*value, "episode_hash")),
      .created_at_ms = rt::toNumber(*rt::get(*value, "created_at_ms")),
      .expires_at_ms = rt::toNumber(*rt::get(*value, "expires_at_ms")),
      .decision_reason = text(rt::get(*value, "decision_reason")),
      .selected_account_id_hash = textOrNull(selectedAccountIdHash),
      .selected_credit_id_hash = textOrNull(selectedCreditIdHash),
      .selected_credit_expires_at_ms = numberOrNull(selectedCreditExpiresAtMs),
      .fences = std::move(parsedFences),
    };
  }

  std::optional<std::string> utcDay(double nowMs) {
    if (!isSafeMs(nowMs)) return std::nullopt;
    try {
      return rt::slice(rt::Date(nowMs).toISOString(), 0, 10);
    } catch (...) {
      return std::nullopt;
    }
  }

  bool claimedDuringCurrentUtcDay(const CodexResetRedemptionRecord& record, double nowMs) {
    const std::optional<std::string> claimedDay = utcDay(record.created_at_ms);
    const std::optional<std::string> currentDay = utcDay(nowMs);
    return claimedDay && claimedDay == currentDay;
  }

  CodexBankedResetOutcome outcome(const std::string& kind, const std::string& reason, const std::optional<ResetContext>& context,
                                  const std::optional<CodexResetRedemptionRecord>& record) {
    return CodexBankedResetOutcome{
      .kind = kind,
      .reason = reason,
      .accountIdHash = context ? std::optional<std::string>(context->account.accountIdHash) : std::nullopt,
      .quotaGeneration = context ? std::optional<std::string>(context->account.quotaGeneration) : std::nullopt,
      .idempotencyKeyHash = context ? std::optional<std::string>(context->idempotencyKeyHash) : std::nullopt,
      .record = record,
    };
  }

  std::optional<std::string> safeOwnerToken(const std::function<std::string()>& source) {
    try {
      std::string token = source();
      if (isNonEmptyText(std::string_view(token), 256)) return token;
      return std::nullopt;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<std::string> policyReason(const CodexBankedResetConfig& config, bool manual) {
    try {
      if (!config.enabled) return "feature_disabled";
      if (config.mode == "disabled") return "mode_disabled";
      // This single-candidate state-machine seam makes no provider call in
      // shadow. The production cohort evaluator requires a positive cap before
      // its bounded inventory reads.
      if (config.mode == "shadow" && !manual) return std::nullopt;
      if (config.maxPerAccountPerDay <= 0 || (manual && config.maxPerAccountPerDay != 1)) return "per_account_day_limit_invalid";
      // The non-negotiable at-most-once rule is stronger than a mutable setting.
      if (config.maxPerAccountPerWindow != 1) return "per_account_window_limit_invalid";
      return std::nullopt;
    } catch (...) {
      return "configuration_invalid";
    }
  }

  std::optional<std::string> providerPolicyReason(const CodexBankedResetConfig& config, const banked_reset_provider::CodexUsageResetProvider* provider,
                                                  bool manual) {
    if ((!manual && config.mode != "live") || config.maxPerAccountPerDay == 1) return std::nullopt;
    return banked_reset_provider::providerTreatsRedeemOutcomeAsFinal(provider) ? "terminal_outcome_account_day_limit_must_be_one"
                                                                               : "per_account_day_limit_invalid";
  }

  rt::AbortSignal boundedInventorySignal(const std::optional<rt::AbortSignal>& signal) {
    rt::AbortSignal timeout = rt::AbortSignal::timeout(CODEX_BANKED_RESET_INVENTORY_TIMEOUT_MS);
    if (signal) return rt::AbortSignal::any({*signal, timeout});
    return timeout;
  }

  CodexBankedResetTelemetryFields telemetryFields(const ResetContext& context, const CodexBankedResetCandidate& candidate) {
    return telemetryFields(context, candidate, CodexBankedResetTelemetryFields());
  }

  CodexBankedResetTelemetryFields telemetryFields(const ResetContext& context, const CodexBankedResetCandidate& candidate,
                                                  const CodexBankedResetTelemetryFields& extras) {
    json::object fields;
    fields["request_id"] = candidate.requestId ? json::value(*candidate.requestId) : json::value(nullptr);
    fields["account_id_hash"] = context.account.accountIdHash;
    fields["credential_version"] = context.account.credentialVersion;
    fields["quota_generation"] = context.account.quotaGeneration;
    fields["idempotency_key_hash"] = context.idempotencyKeyHash;
    fields["routing_generation"] = rt::makeNumber(candidate.routingGeneration);
    return rt::objectSpread(fields, extras);
  }

  void emit(const CodexBankedResetTelemetry& telemetry, const CodexBankedResetEvent& event, const CodexBankedResetTelemetryFields& fields) {
    try {
      if (telemetry.event) telemetry.event(event, fields);
    } catch (...) {
      // An observer must not affect state transitions.
    }
  }

  void metric(const CodexBankedResetTelemetry& telemetry, const CodexBankedResetMetric& name, double value, const CodexBankedResetTelemetryFields& fields) {
    try {
      if (telemetry.metric) telemetry.metric(name, value, fields);
    } catch (...) {
      // An observer must not affect state transitions.
    }
  }

  void reportCodexBankedResetEvent(const std::optional<CodexBankedResetTelemetry>& telemetry, const CodexBankedResetEvent& event,
                                   const CodexBankedResetTelemetryFields& fields) {
    emit(telemetry ? *telemetry : defaultTelemetry, event, fields);
  }

  void reportCodexBankedResetMetric(const std::optional<CodexBankedResetTelemetry>& telemetry, const CodexBankedResetMetric& name, double value,
                                    const CodexBankedResetTelemetryFields& fields) {
    metric(telemetry ? *telemetry : defaultTelemetry, name, value, fields);
  }
}
