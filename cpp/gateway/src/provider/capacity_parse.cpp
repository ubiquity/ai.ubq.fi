#include "uos/provider/capacity_parse.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/config.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"
#include "uos/utils.hpp"

#include <utility>

namespace uos::provider::capacity_parse {
  using capacity_contract::CapacityState;
  using capacity_contract::ProviderCapacityAdditionalRateLimit;
  using capacity_contract::ProviderCapacityCodexSource;
  using capacity_contract::ProviderCapacityFailureKind;
  using capacity_contract::ProviderCapacityHistoryPoint;
  using capacity_contract::ProviderCapacityMeteredSource;
  using capacity_contract::ProviderCapacitySnapshot;
  using capacity_contract::ProviderCapacityWindow;

  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    // `isRecord` (src/utils.ts: `typeof value === "object" && value !== null`, arrays included)
    // over a stored value; members of a record that is not a plain object read as undefined.

    // `value === n` for a number literal.
    bool isNumberLiteral(const rt::KvValue& value, double literal) { return value.isNumber() && value.asNumber() == literal; }
    bool isStringLiteral(const rt::KvValue& value, std::string_view literal) { return value.isString() && value.asString() == literal; }

    std::optional<CapacityState> capacityState(const rt::KvValue& value) {
      if (isStringLiteral(value, "available") || isStringLiteral(value, "stale") || isStringLiteral(value, "unavailable")) return value.asString();
      return std::nullopt;
    }

    std::optional<double> optionalStoredNumber(const rt::KvValue& value) {
      return value.isNumber() && rt::Number::isFinite(value.asNumber()) ? std::optional<double>(value.asNumber()) : std::nullopt;
    }

    std::optional<double> optionalStoredTimestamp(const rt::KvValue& value) {
      return isSafeTimestamp(value) ? std::optional<double>(value.asNumber()) : std::nullopt;
    }

    std::optional<bool> optionalStoredBoolean(const rt::KvValue& value) { return value.isBool() ? std::optional<bool>(value.asBool()) : std::nullopt; }

    // The number guards on a `typeof value === "number"` read (std::nullopt: another type).
    std::optional<double> parsePercent(std::optional<double> value) {
      return value && rt::Number::isFinite(*value) && *value >= 0 && *value <= 100 ? value : std::nullopt;
    }

    std::optional<double> parseWindowSeconds(std::optional<double> value) {
      return value && rt::Number::isSafeInteger(*value) && *value >= 0 ? value : std::nullopt;
    }

    std::optional<double> parseResetAtMs(std::optional<double> value) {
      if (!value || !rt::Number::isSafeInteger(*value) || *value < 0) return std::nullopt;
      const double milliseconds = *value * 1'000;
      return rt::Number::isSafeInteger(milliseconds) ? std::optional<double>(milliseconds) : std::nullopt;
    }

    std::optional<double> storedNumber(const rt::KvValue& value) { return value.isNumber() ? std::optional<double>(value.asNumber()) : std::nullopt; }

    std::optional<ProviderCapacityWindow> parseCodexWindow(const json::value* value) {
      if (!utils::isRecord(value)) return std::nullopt;
      const std::optional<double> limitWindowSeconds = parseWindowSeconds(rt::getNumber(rt::get(value, "limit_window_seconds")));
      const std::optional<double> usedPercent = parsePercent(rt::getNumber(rt::get(value, "used_percent")));
      const std::optional<double> resetAtMs = parseResetAtMs(rt::getNumber(rt::get(value, "reset_at")));
      if (!limitWindowSeconds && !usedPercent && !resetAtMs) return std::nullopt;
      return ProviderCapacityWindow{
        .limit_window_seconds = limitWindowSeconds,
        .used_percent = usedPercent,
        .reset_at_ms = resetAtMs,
      };
    }

    bool isUnanchoredAdditionalWindow(const std::optional<ProviderCapacityWindow>& window, double snapshotAtMs) {
      if (!window || window->used_percent != 0.0 || !window->limit_window_seconds || !window->reset_at_ms) return false;
      const double fullWindowMs = *window->limit_window_seconds * 1'000;
      if (!rt::Number::isSafeInteger(fullWindowMs)) return false;
      const double expectedResetAtMs = snapshotAtMs + fullWindowMs;
      return rt::Number::isSafeInteger(expectedResetAtMs) &&
             rt::Math::abs(*window->reset_at_ms - expectedResetAtMs) <= capacity_contract::ADDITIONAL_WINDOW_UNANCHORED_TOLERANCE_MS;
    }

    std::optional<ProviderCapacityAdditionalRateLimit> parseCodexAdditionalRateLimit(const json::value& value) {
      if (!utils::isRecord(value)) return std::nullopt;
      const json::value* name = rt::get(value, "limit_name");
      const std::string limitName = name && name->is_string() ? rt::trim(name->get_string()) : std::string();
      if (limitName.empty()) return std::nullopt;
      std::optional<std::string> meteredFeature;
      if (const json::value* feature = rt::get(value, "metered_feature"); feature && feature->is_string()) {
        std::string trimmed = rt::trim(feature->get_string());
        if (!trimmed.empty()) meteredFeature = std::move(trimmed);
      }
      const json::value* rateLimitMember = rt::get(value, "rate_limit");
      const json::value* rateLimit = utils::isRecord(rateLimitMember) ? rateLimitMember : nullptr;
      std::optional<ProviderCapacityWindow> parsedPrimary = parseCodexWindow(rt::get(rateLimit, "primary_window"));
      std::optional<ProviderCapacityWindow> parsedSecondary = parseCodexWindow(rt::get(rateLimit, "secondary_window"));
      if (!parsedPrimary && !parsedSecondary) return std::nullopt;
      return ProviderCapacityAdditionalRateLimit{
        .limit_name = limitName,
        .metered_feature = std::move(meteredFeature),
        .windows =
          {
            // Keep OpenAI's unused model window for the admin card. Routing filters
            // this unanchored value separately because its reset is not evidence of
            // a shared quota cycle.
            .primary = std::move(parsedPrimary),
            .secondary = std::move(parsedSecondary),
          },
      };
    }

    ProviderCapacityCodexSource::Windows emptyWindows() { return {.primary = std::nullopt, .secondary = std::nullopt}; }

    bool isStoredWindow(const rt::KvValue& value) {
      if (!rt::kvIsObject(value)) return false;
      const rt::KvValue& limitWindowSeconds = value["limit_window_seconds"];
      const rt::KvValue& usedPercent = value["used_percent"];
      const rt::KvValue& resetAtMs = value["reset_at_ms"];
      return (limitWindowSeconds.isNull() || parseWindowSeconds(storedNumber(limitWindowSeconds))) &&
             (usedPercent.isNull() || parsePercent(storedNumber(usedPercent))) && (resetAtMs.isNull() || isSafeTimestamp(resetAtMs));
    }

    std::optional<ProviderCapacityWindow> readStoredWindow(const rt::KvValue& value) {
      if (!isStoredWindow(value)) return std::nullopt;
      return ProviderCapacityWindow{
        .limit_window_seconds = storedNumber(value["limit_window_seconds"]),
        .used_percent = storedNumber(value["used_percent"]),
        .reset_at_ms = storedNumber(value["reset_at_ms"]),
      };
    }

    std::optional<ProviderCapacityAdditionalRateLimit> readStoredAdditionalRateLimit(const rt::KvValue& value) {
      if (!rt::kvIsObject(value) || !value["limit_name"].isString() || rt::trim(value["limit_name"].asString()).empty()) return std::nullopt;
      const rt::KvValue& windows = value["windows"];
      if (!rt::kvIsObject(windows)) return std::nullopt;
      const rt::KvValue& meteredFeature = value["metered_feature"];
      if (!meteredFeature.isNull() && !meteredFeature.isString()) return std::nullopt;
      std::optional<std::string> feature;
      if (meteredFeature.isString()) {
        std::string trimmed = rt::trim(meteredFeature.asString());
        if (!trimmed.empty()) feature = std::move(trimmed);
      }
      return ProviderCapacityAdditionalRateLimit{
        .limit_name = rt::trim(value["limit_name"].asString()),
        .metered_feature = std::move(feature),
        .windows =
          {
            .primary = readStoredWindow(windows["primary"]),
            .secondary = readStoredWindow(windows["secondary"]),
          },
      };
    }

    std::vector<ProviderCapacityAdditionalRateLimit> readStoredAdditionalRateLimits(const rt::KvValue& value) {
      std::vector<ProviderCapacityAdditionalRateLimit> limits;
      if (!value.isArray()) return limits;
      for (const rt::KvValue& candidate : value.asArray()) {
        // `flatMap` skips holes.
        if (candidate.is<rt::KvHole>()) continue;
        if (std::optional<ProviderCapacityAdditionalRateLimit> parsed = readStoredAdditionalRateLimit(candidate)) limits.push_back(std::move(*parsed));
      }
      return limits;
    }

    bool isProviderCapacityFailureKind(const rt::KvValue& value) {
      return isStringLiteral(value, "not_configured") || isStringLiteral(value, "http_error") || isStringLiteral(value, "upstream_error") ||
             isStringLiteral(value, "unreachable") || isStringLiteral(value, "invalid_response");
    }

    std::optional<double> readStoredFailureStatus(const rt::KvValue& value) {
      return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && value.asNumber() >= 100 && value.asNumber() <= 599
               ? std::optional<double>(value.asNumber())
               : std::nullopt;
    }

    struct StoredCodexFailure {
      std::optional<ProviderCapacityFailureKind> failure_kind;
      std::optional<double> failure_status;
    };

    // A stored record may omit its failure fields. An unavailable source then means
    // "not configured", while an available source must not claim any failure at all;
    // a present-but-unknown kind invalidates the whole record.
    std::optional<StoredCodexFailure> readStoredCodexFailure(const rt::KvValue& source, const CapacityState& state) {
      const rt::KvValue& storedKind = source["failure_kind"];
      if (!storedKind.isNull() && !storedKind.isUndefined() && !isProviderCapacityFailureKind(storedKind)) return std::nullopt;
      const std::optional<ProviderCapacityFailureKind> defaultFailureKind =
        state == "unavailable" ? std::optional<ProviderCapacityFailureKind>("not_configured") : std::nullopt;
      const std::optional<ProviderCapacityFailureKind> failureKind = isProviderCapacityFailureKind(storedKind) ? std::optional(storedKind.asString()) : defaultFailureKind;
      const std::optional<double> failureStatus = readStoredFailureStatus(source["failure_status"]);
      if (state == "available" && (failureKind || failureStatus)) return std::nullopt;
      return StoredCodexFailure{.failure_kind = failureKind, .failure_status = failureStatus};
    }

    std::optional<ProviderCapacityMeteredSource> readStoredMeteredSource(const rt::KvValue& value, double snapshotAtMs) {
      if (!rt::kvIsObject(value) || !isStringLiteral(value["source"], "metered")) return std::nullopt;
      const std::optional<CapacityState> state = capacityState(value["state"]);
      const rt::KvValue& observed = value["source_observed_at_ms"];
      const rt::KvValue& wallet = value["wallet"];
      if (!state || !rt::kvIsObject(wallet) || !(observed.isNull() || isSafeTimestamp(observed))) return std::nullopt;
      const rt::KvValue& confidence = wallet["confidence"];
      const rt::KvValue& cacheState = wallet["cache_state"];
      const bool knownConfidence = isStringLiteral(confidence, "provisional") || isStringLiteral(confidence, "refill_observed") ||
                                   isStringLiteral(confidence, "inferred_adjustment");
      const bool knownCacheState = isStringLiteral(cacheState, "fresh") || isStringLiteral(cacheState, "refreshed") || isStringLiteral(cacheState, "stale") ||
                                   isStringLiteral(cacheState, "wait");
      return ProviderCapacityMeteredSource{
        .label = "Metered fallback",
        .state = *state,
        .source_observed_at_ms = storedNumber(observed),
        .snapshot_at_ms = snapshotAtMs,
        .wallet =
          {
            .balance_credits = optionalStoredNumber(wallet["balance_credits"]),
            .baseline_credits = optionalStoredNumber(wallet["baseline_credits"]),
            .refill_cycle_remaining_percent = optionalStoredNumber(wallet["refill_cycle_remaining_percent"]),
            .refill_cycle_used_percent = optionalStoredNumber(wallet["refill_cycle_used_percent"]),
            .unlimited_quota = optionalStoredBoolean(wallet["unlimited_quota"]),
            .total_available = optionalStoredNumber(wallet["total_available"]),
            .total_granted = optionalStoredNumber(wallet["total_granted"]),
            .total_used = optionalStoredNumber(wallet["total_used"]),
            .cycle_started_at_ms = optionalStoredTimestamp(wallet["cycle_started_at_ms"]),
            .last_credit_at_ms = optionalStoredTimestamp(wallet["last_credit_at_ms"]),
            .confidence = knownConfidence ? std::optional<std::string>(confidence.asString()) : std::nullopt,
            .cache_state = knownCacheState ? std::optional<std::string>(cacheState.asString()) : std::nullopt,
            .reset_at_ms = nullptr,
          },
      };
    }

    // `sources.find((source) => rt::kvIsObject(source) && source.source === "codex" && source.slot === slot)`
    // (`find` visits holes as undefined, which no predicate accepts). Undefined when none matches.
    const rt::KvValue& findCodexSource(const rt::KvArray& sources, double slot) {
      static const rt::KvValue undefined;
      for (const rt::KvValue& source : sources) {
        if (rt::kvIsObject(source) && isStringLiteral(source["source"], "codex") && isNumberLiteral(source["slot"], slot)) return source;
      }
      return undefined;
    }

    const rt::KvValue& findMeteredSource(const rt::KvArray& sources) {
      static const rt::KvValue undefined;
      for (const rt::KvValue& source : sources) {
        if (rt::kvIsObject(source) && isStringLiteral(source["source"], "metered")) return source;
      }
      return undefined;
    }
  }

  std::string codexAccountLabel(double slot, const std::optional<std::string>& email) {
    const std::string trimmedEmail = email ? rt::trim(*email) : std::string();
    // A blank label would hide the account, so an empty or whitespace-only email
    // still falls back to the slot label.
    if (!trimmedEmail.empty()) return trimmedEmail;
    return "Codex account " + rt::numberToString(slot);
  }

  rt::KvKey providerCapacityLastAvailableKey(double slot) { return rt::KvKey{capacity_contract::PROVIDER_CAPACITY_LAST_AVAILABLE_KEY_PREFIX, slot}; }

  double safeNow(const std::function<double()>& now) {
    const double value = rt::Math::trunc(now());
    return rt::Number::isSafeInteger(value) && value >= 0 ? value : rt::dateNow();
  }

  bool isSafeTimestamp(const rt::KvValue& value) { return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && value.asNumber() >= 0; }

  std::string codexUsageUrl() { return rt::URL("/backend-api/wham/usage", config::config.codexBaseUrl).href(); }

  std::vector<ProviderCapacityAdditionalRateLimit> additionalRateLimitsForRouting(const std::vector<ProviderCapacityAdditionalRateLimit>& limits,
                                                                                  double snapshotAtMs) {
    std::vector<ProviderCapacityAdditionalRateLimit> routed;
    for (const ProviderCapacityAdditionalRateLimit& limit : limits) {
      std::optional<ProviderCapacityWindow> primary = isUnanchoredAdditionalWindow(limit.windows.primary, snapshotAtMs) ? std::nullopt : limit.windows.primary;
      std::optional<ProviderCapacityWindow> secondary =
        isUnanchoredAdditionalWindow(limit.windows.secondary, snapshotAtMs) ? std::nullopt : limit.windows.secondary;
      if (!primary && !secondary) continue;
      ProviderCapacityAdditionalRateLimit kept = limit;
      kept.windows = {.primary = std::move(primary), .secondary = std::move(secondary)};
      routed.push_back(std::move(kept));
    }
    return routed;
  }

  std::optional<ProviderCapacityOverageUsage> parseCodexOverageUsage(const json::value& value) {
    if (!utils::isRecord(value)) return std::nullopt;
    // The live /backend-api/wham/usage payload nests both fields under `credits`.
    const json::value* creditsMember = rt::get(value, "credits");
    const json::value* credits = utils::isRecord(creditsMember) ? creditsMember : nullptr;
    if (!credits) return std::nullopt;
    std::optional<std::string> balance = utils::getString(rt::get(credits, "balance"));
    const std::optional<bool> overageLimitReached = rt::getBool(rt::get(credits, "overage_limit_reached"));
    if (!balance && !overageLimitReached) return std::nullopt;
    return ProviderCapacityOverageUsage{.balance = std::move(balance), .overage_limit_reached = overageLimitReached};
  }

  std::optional<ProviderCapacityOverageUsage> parseCodexOverageUsage(const json::value* value) {
    if (!value) return std::nullopt;
    return parseCodexOverageUsage(*value);
  }

  std::optional<ParseCodexUsageResult> parseCodexUsage(const json::value& value) {
    const json::value* rateLimit = rt::get(value, "rate_limit");
    if (!utils::isRecord(value) || !utils::isRecord(rateLimit)) return std::nullopt;
    std::vector<ProviderCapacityAdditionalRateLimit> additionalRateLimits;
    if (const json::array* candidates = rt::getArray(rt::get(value, "additional_rate_limits"))) {
      for (const json::value& candidate : *candidates) {
        if (std::optional<ProviderCapacityAdditionalRateLimit> parsed = parseCodexAdditionalRateLimit(candidate)) additionalRateLimits.push_back(std::move(*parsed));
      }
    }
    return ParseCodexUsageResult{
      .primary = parseCodexWindow(rt::get(rateLimit, "primary_window")),
      .secondary = parseCodexWindow(rt::get(rateLimit, "secondary_window")),
      .additional_rate_limits = std::move(additionalRateLimits),
    };
  }

  std::optional<ParseCodexUsageResult> parseCodexUsage(const json::value* value) {
    if (!value) return std::nullopt;
    return parseCodexUsage(*value);
  }

  ProviderCapacityCodexSource unavailableCodexSource(double slot, double snapshotAtMs, ProviderCapacityFailureKind failureKind, std::optional<double> failureStatus,
                                                     bool observed, std::optional<std::string> label, std::optional<std::string> accountCohortId) {
    return ProviderCapacityCodexSource{
      .label = label ? std::move(*label) : codexAccountLabel(slot),
      .slot = slot,
      .account_cohort_id = std::move(accountCohortId),
      .state = "unavailable",
      .source_observed_at_ms = observed ? std::optional<double>(snapshotAtMs) : std::nullopt,
      .snapshot_at_ms = snapshotAtMs,
      .failure_kind = std::move(failureKind),
      .failure_status = failureStatus,
      .windows = emptyWindows(),
      .additional_rate_limits = {},
    };
  }

  ProviderCapacityMeteredSource unavailableMeteredSource(double snapshotAtMs) {
    return ProviderCapacityMeteredSource{
      .label = "Metered fallback",
      .state = "unavailable",
      .source_observed_at_ms = std::nullopt,
      .snapshot_at_ms = snapshotAtMs,
      .wallet =
        {
          .balance_credits = std::nullopt,
          .baseline_credits = std::nullopt,
          .refill_cycle_remaining_percent = std::nullopt,
          .refill_cycle_used_percent = std::nullopt,
          .unlimited_quota = std::nullopt,
          .total_available = std::nullopt,
          .total_granted = std::nullopt,
          .total_used = std::nullopt,
          .cycle_started_at_ms = std::nullopt,
          .last_credit_at_ms = std::nullopt,
          .confidence = std::nullopt,
          .cache_state = std::nullopt,
          .reset_at_ms = nullptr,
        },
    };
  }

  std::optional<ProviderCapacityCodexSource> readStoredCodexSource(const rt::KvValue& value, double fallbackSnapshotAtMs) {
    if (!rt::kvIsObject(value) || !isStringLiteral(value["source"], "codex") || (!isNumberLiteral(value["slot"], 1) && !isNumberLiteral(value["slot"], 2))) {
      return std::nullopt;
    }
    const std::optional<CapacityState> state = capacityState(value["state"]);
    const rt::KvValue& observed = value["source_observed_at_ms"];
    const rt::KvValue& storedSnapshotAtMs = value["snapshot_at_ms"];
    const rt::KvValue snapshotAtMs = storedSnapshotAtMs.isUndefined() ? rt::KvValue(fallbackSnapshotAtMs) : storedSnapshotAtMs;
    if (!state || !(observed.isNull() || isSafeTimestamp(observed)) || !isSafeTimestamp(snapshotAtMs)) return std::nullopt;
    const std::optional<StoredCodexFailure> failure = readStoredCodexFailure(value, *state);
    if (!failure) return std::nullopt;
    const rt::KvValue& windows = value["windows"];
    if (!rt::kvIsObject(windows)) return std::nullopt;
    const double slot = value["slot"].asNumber();
    const rt::KvValue& label = value["label"];
    const rt::KvValue& accountCohortId = value["account_cohort_id"];
    const bool available = *state == "available";
    return ProviderCapacityCodexSource{
      .label = label.isString() && !rt::trim(label.asString()).empty() ? rt::trim(label.asString()) : codexAccountLabel(slot),
      .slot = slot,
      .account_cohort_id = accountCohortId.isString() && capacity_contract::SHA256_HEX.test(accountCohortId.asString())
                             ? std::optional<std::string>(accountCohortId.asString())
                             : std::nullopt,
      .state = *state,
      .source_observed_at_ms = storedNumber(observed),
      .snapshot_at_ms = snapshotAtMs.asNumber(),
      .failure_kind = available ? std::nullopt : failure->failure_kind,
      .failure_status = available ? std::nullopt : failure->failure_status,
      .windows =
        {
          .primary = readStoredWindow(windows["primary"]),
          .secondary = readStoredWindow(windows["secondary"]),
        },
      .additional_rate_limits = readStoredAdditionalRateLimits(value["additional_rate_limits"]),
    };
  }

  std::optional<ProviderCapacitySnapshot> readStoredSnapshot(const rt::KvValue& value) {
    if (!rt::kvIsObject(value) || !isSafeTimestamp(value["snapshot_at_ms"]) || !value["sources"].isArray()) return std::nullopt;
    const double snapshotAtMs = value["snapshot_at_ms"].asNumber();
    const rt::KvArray& sources = value["sources"].asArray();
    std::optional<ProviderCapacityCodexSource> codexOne = readStoredCodexSource(findCodexSource(sources, 1), snapshotAtMs);
    std::optional<ProviderCapacityCodexSource> codexTwo = readStoredCodexSource(findCodexSource(sources, 2), snapshotAtMs);
    std::optional<ProviderCapacityMeteredSource> metered = readStoredMeteredSource(findMeteredSource(sources), snapshotAtMs);
    if (!codexOne || !codexTwo || !metered) return std::nullopt;
    return ProviderCapacitySnapshot{
      .snapshot_at_ms = snapshotAtMs,
      // Old records used a shorter value. All records now follow the sampler
      // freshness boundary so a missed 15-minute run is tolerated once.
      .stale_after_ms = capacity_contract::PROVIDER_CAPACITY_SOURCE_STALE_MS,
      .sources = {std::move(*codexOne), std::move(*codexTwo), std::move(*metered)},
    };
  }

  rt::Task<std::optional<ProviderCapacitySnapshot>> readCapacitySnapshot(std::shared_ptr<rt::Kv> kv) {
    std::optional<ProviderCapacitySnapshot> snapshot;
    try {
      auto reading = kv->get(capacity_contract::PROVIDER_CAPACITY_SNAPSHOT_KEY);
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      snapshot = readStoredSnapshot(entry.value);
    } catch (...) {
      snapshot = std::nullopt;
    }
    co_return snapshot;
  }

  std::optional<ProviderCapacityHistoryPoint> readStoredHistoryPoint(const rt::KvValue& value) {
    if (!rt::kvIsObject(value) || !isSafeTimestamp(value["bucket_start_at_ms"]) || !isSafeTimestamp(value["sampled_at_ms"])) {
      return std::nullopt;
    }
    if (!value["sources"].isArray()) return std::nullopt;
    const rt::KvArray& sources = value["sources"].asArray();
    const double sampledAtMs = value["sampled_at_ms"].asNumber();
    std::optional<ProviderCapacityCodexSource> codexSourceOne = readStoredCodexSource(findCodexSource(sources, 1), sampledAtMs);
    std::optional<ProviderCapacityCodexSource> codexSourceTwo = readStoredCodexSource(findCodexSource(sources, 2), sampledAtMs);
    if (!codexSourceOne || !codexSourceTwo) return std::nullopt;
    std::optional<ProviderCapacityMeteredSource> meteredSource = readStoredMeteredSource(findMeteredSource(sources), sampledAtMs);
    return ProviderCapacityHistoryPoint{
      .bucket_start_at_ms = value["bucket_start_at_ms"].asNumber(),
      .sampled_at_ms = sampledAtMs,
      .sources = {std::move(*codexSourceOne), std::move(*codexSourceTwo), meteredSource ? std::move(*meteredSource) : unavailableMeteredSource(sampledAtMs)},
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityOverageUsage& value) {
    out = Writer().orNull("balance", value.balance).orNull("overage_limit_reached", value.overage_limit_reached).done();
  }

  ProviderCapacityOverageUsage tag_invoke(const json::value_to_tag<ProviderCapacityOverageUsage>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityOverageUsage");
    return ProviderCapacityOverageUsage{.balance = in.orNull<std::string>("balance"), .overage_limit_reached = in.orNull<bool>("overage_limit_reached")};
  }
}
