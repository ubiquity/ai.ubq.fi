#include "uos/provider/claude_code_quota.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace uos::provider::claude_code_quota {
  namespace {
    constexpr rt::StoredJsonViewOptions STORED_JSON_VIEW_OPTIONS{
      .undefinedMember = rt::StoredJsonViewOptions::UndefinedView::omit,
      .undefinedElement = rt::StoredJsonViewOptions::UndefinedView::null,
      .hole = rt::StoredJsonViewOptions::UndefinedView::null,
      .topLevelUndefined = rt::StoredJsonViewOptions::UndefinedView::null,
      .bigint = rt::StoredJsonViewOptions::BigIntView::emptyObject,
      .kvU64Members = false,
      .typedArrayMembers = false,
    };
    using authority::contracts::Record;
    using claude_code_accounts::ClaudeCodeAccountBinding;
    using claude_code_usage_schema::ClaudeCodeUsageSnapshot;
    using rt::record::Reader;
    using rt::record::Writer;

    constexpr std::string_view HEADER_PREFIX = "anthropic-ratelimit-unified-";
    constexpr std::string_view STANDARD_HEADER_PREFIX = "anthropic-ratelimit-";

    const rt::KvGetOptions STRONG{.consistency = rt::KvConsistencyLevel::strong};

    // `let observationRecorder: ObservationRecorder | null` (construct-on-first-use: capacity.cpp
    // registers into it from a namespace-scope initialiser).
    ObservationRecorder& observationRecorder() {
      static ObservationRecorder slot;
      return slot;
    }

    using RecorderFunction = rt::Task<void> (*)(std::shared_ptr<rt::Kv>, ClaudeCodeAcceptedObservation);
    // `left === right` on recorders: the same plain function.
    bool sameRecorder(const ObservationRecorder& left, const ObservationRecorder& right) {
      const RecorderFunction* a = left.target<RecorderFunction>();
      const RecorderFunction* b = right.target<RecorderFunction>();
      return a != nullptr && b != nullptr && *a == *b;
    }

    // `typeof value === "object" && value !== null` (utils.isRecord) on a stored value.

    bool isText(const rt::KvValue& value, std::string_view text) { return value.isString() && value.asString() == text; }

    // A stored value as the JSON readers of claude-code-usage-schema see it: undefined members are
    // absent and every value JSON cannot hold (bigint, Map, Date, ...) reads as an empty object.

    // `Object.entries(value)` for a value that is an object: only a plain object or an array can
    // yield members that are records (a Map, Date, typed array, ... has none).
    std::vector<std::pair<std::string, const rt::KvValue*>> ownEntries(const rt::KvValue& value) {
      std::vector<std::pair<std::string, const rt::KvValue*>> entries;
      if (value.isObject()) {
        for (const auto& [key, member] : value.asObject()) entries.emplace_back(key, &member);
      } else if (value.isArray()) {
        const rt::KvArray& items = value.asArray();
        for (std::size_t index = 0; index < items.size(); ++index)
          if (!items[index].is<rt::KvHole>()) entries.emplace_back(rt::numberToString(static_cast<double>(index)), &items[index]);
      }
      return entries;
    }

    template <class T> json::object recordJson(const Record<T>& record) {
      json::object out;
      for (const std::string& key : record.keys()) rt::objectDefine(out, key, json::value_from(*record.get(key)));
      return out;
    }
    template <class T> Record<T> recordFrom(const json::object& object) {
      Record<T> out;
      for (const json::key_value_pair* entry : rt::objectEntries(object)) out.define(std::string(entry->key()), json::value_to<T>(entry->value()));
      return out;
    }

    std::optional<double> finiteNumber(const rt::KvValue& value) {
      if (!value.isNumber() || !rt::Number::isFinite(value.asNumber())) return std::nullopt;
      return value.asNumber();
    }

    std::optional<double> positiveSafeInteger(const rt::KvValue& value) {
      const std::optional<double> parsed = finiteNumber(value);
      if (parsed && rt::Number::isSafeInteger(*parsed) && *parsed > 0) return parsed;
      return std::nullopt;
    }

    std::optional<ClaudeCodePoolProof> parsePoolProof(const rt::KvValue& proof) {
      if (!rt::kvIsObject(proof) || !(isText(proof["kind"], "auth_invalid") || isText(proof["kind"], "quota_exhausted"))) return std::nullopt;
      const rt::KvValue& observedAtMs = proof["observed_at_ms"];
      const rt::KvValue& expiresAtMs = proof["expires_at_ms"];
      const rt::KvValue& evidenceGeneration = proof["evidence_generation"];
      if (!observedAtMs.isNumber() || !rt::Number::isSafeInteger(observedAtMs.asNumber()) || observedAtMs.asNumber() <= 0 || !expiresAtMs.isNumber() ||
          !rt::Number::isSafeInteger(expiresAtMs.asNumber()) || expiresAtMs.asNumber() <= observedAtMs.asNumber() || !evidenceGeneration.isString())
        return std::nullopt;
      const rt::KvValue& modelId = proof["model_id"];
      if (!(modelId.isNull() || modelId.isString())) return std::nullopt;
      const std::string& kind = proof["kind"].asString();
      if (kind == "auth_invalid" ? !modelId.isNull() : !modelId.truthy()) return std::nullopt;
      return ClaudeCodePoolProof{
        .kind = kind,
        .model_id = modelId.isNull() ? std::nullopt : std::optional<std::string>(modelId.asString()),
        .observed_at_ms = observedAtMs.asNumber(),
        .expires_at_ms = expiresAtMs.asNumber(),
        .evidence_generation = evidenceGeneration.asString(),
      };
    }

    Record<ClaudeCodePoolProof> parsePoolProofs(const rt::KvValue& value) {
      Record<ClaudeCodePoolProof> proofs;
      if (!rt::kvIsObject(value)) return proofs;
      for (const auto& [key, proof] : ownEntries(value))
        if (std::optional<ClaudeCodePoolProof> parsed = parsePoolProof(*proof)) proofs.set(key, std::move(*parsed));
      return proofs;
    }

    std::optional<double> finiteNonNegative(double value) {
      if (rt::Number::isFinite(value) && value >= 0) return value;
      return std::nullopt;
    }

    // `value !== null && value.trim() !== "" ? value : null`: the untrimmed text.
    std::optional<std::string> trimmedOrNull(const std::optional<std::string>& value) {
      if (value && !rt::trim(*value).empty()) return value;
      return std::nullopt;
    }

    std::optional<double> parseUtilization(const std::optional<std::string>& raw) {
      if (!raw || rt::trim(*raw).empty()) return std::nullopt;
      return finiteNonNegative(rt::stringToNumber(*raw));
    }

    std::optional<double> parseResetSeconds(const std::optional<std::string>& raw) {
      if (!raw || rt::trim(*raw).empty()) return std::nullopt;
      const double seconds = rt::stringToNumber(*raw);
      if (!rt::Number::isSafeInteger(seconds) || seconds <= 0) return std::nullopt;
      return seconds * 1'000;
    }

    std::optional<ClaudeCodeQuotaWindow> parseWindow(const rt::Headers& headers, std::string_view window, double observedAtMs) {
      const std::string prefix = std::string(HEADER_PREFIX) + std::string(window);
      const std::optional<std::string> status = headers.get(prefix + "-status");
      const std::optional<double> utilization = parseUtilization(headers.get(prefix + "-utilization"));
      const std::optional<double> resetAtMs = parseResetSeconds(headers.get(prefix + "-reset"));
      if (!status && !utilization && !resetAtMs) return std::nullopt;
      return ClaudeCodeQuotaWindow{.status = trimmedOrNull(status), .utilization = utilization, .reset_at_ms = resetAtMs, .observed_at_ms = observedAtMs};
    }

    std::optional<ClaudeCodeStandardWindow> parseStandardWindow(const rt::Headers& headers, std::string_view family, double observedAtMs) {
      const std::string prefix = std::string(STANDARD_HEADER_PREFIX) + std::string(family);
      const std::optional<double> limit = parseUtilization(headers.get(prefix + "-limit"));
      const std::optional<double> remaining = parseUtilization(headers.get(prefix + "-remaining"));
      const std::optional<double> resetAtMs = parseResetSeconds(headers.get(prefix + "-reset"));
      if (!limit && !remaining && !resetAtMs) return std::nullopt;
      return ClaudeCodeStandardWindow{.limit = limit, .remaining = remaining, .reset_at_ms = resetAtMs, .observed_at_ms = observedAtMs};
    }

    bool observationIsEmpty(const ClaudeCodeQuotaObservation& observation) {
      return !observation.usage_snapshot && observation.windows.size() == 0 && observation.standard.size() == 0 && !observation.representative_claim;
    }

    // A present-but-malformed optional number is rejected; an absent one passes.
    template <class Invalid> bool optionalNumberInvalid(const rt::KvValue& raw, const std::optional<double>& parsed, Invalid invalid) {
      return !raw.isNullish() && (!parsed || invalid(*parsed));
    }

    struct ResetAtMs {
      bool ok;
      std::optional<double> value;
    };
    ResetAtMs parseResetAtMs(const rt::KvValue& value) {
      const std::optional<double> resetAtMs = finiteNumber(value);
      const bool invalid = optionalNumberInvalid(value, resetAtMs, [](double parsed) { return parsed <= 0 || !rt::Number::isSafeInteger(parsed); });
      if (invalid) return {.ok = false, .value = std::nullopt};
      return {.ok = true, .value = resetAtMs};
    }

    std::optional<ClaudeCodeQuotaWindow> parseWindowState(const rt::KvValue& value) {
      if (!rt::kvIsObject(value)) return std::nullopt;
      const std::optional<double> observedAtMs = finiteNumber(value["observed_at_ms"]);
      if (!observedAtMs || *observedAtMs <= 0) return std::nullopt;
      const std::optional<double> utilization = finiteNumber(value["utilization"]);
      if (optionalNumberInvalid(value["utilization"], utilization, [](double parsed) { return parsed < 0; })) return std::nullopt;
      const ResetAtMs reset = parseResetAtMs(value["reset_at_ms"]);
      if (!reset.ok) return std::nullopt;
      const rt::KvValue& status = value["status"];
      return ClaudeCodeQuotaWindow{
        .status = status.isString() ? std::optional<std::string>(status.asString()) : std::nullopt,
        .utilization = utilization,
        .reset_at_ms = reset.value,
        .observed_at_ms = rt::Math::trunc(*observedAtMs),
      };
    }

    std::optional<ClaudeCodeStandardWindow> parseStandardWindowState(const rt::KvValue& value) {
      if (!rt::kvIsObject(value)) return std::nullopt;
      const std::optional<double> observedAtMs = finiteNumber(value["observed_at_ms"]);
      if (!observedAtMs || *observedAtMs <= 0) return std::nullopt;
      const std::optional<double> limit = finiteNumber(value["limit"]);
      if (optionalNumberInvalid(value["limit"], limit, [](double parsed) { return parsed < 0; })) return std::nullopt;
      const std::optional<double> remaining = finiteNumber(value["remaining"]);
      if (optionalNumberInvalid(value["remaining"], remaining, [](double parsed) { return parsed < 0; })) return std::nullopt;
      const ResetAtMs reset = parseResetAtMs(value["reset_at_ms"]);
      if (!reset.ok) return std::nullopt;
      return ClaudeCodeStandardWindow{.limit = limit, .remaining = remaining, .reset_at_ms = reset.value, .observed_at_ms = rt::Math::trunc(*observedAtMs)};
    }

    ClaudeCodeQuotaWindows parseStoredWindows(const rt::KvValue& value) {
      ClaudeCodeQuotaWindows windows;
      if (!rt::kvIsObject(value)) return windows;
      for (const std::string_view window : CLAUDE_CODE_WINDOWS)
        if (std::optional<ClaudeCodeQuotaWindow> parsed = parseWindowState(value[window])) windows.set(std::string(window), std::move(*parsed));
      return windows;
    }

    ClaudeCodeStandardWindows parseStoredStandardWindows(const rt::KvValue& value) {
      ClaudeCodeStandardWindows standard;
      if (!rt::kvIsObject(value)) return standard;
      for (const std::string_view family : CLAUDE_CODE_STANDARD_FAMILIES)
        if (std::optional<ClaudeCodeStandardWindow> parsed = parseStandardWindowState(value[family])) standard.set(std::string(family), std::move(*parsed));
      return standard;
    }

    // `CLAUDE_CODE_USAGE_STATES.has(state)`
    bool claudeCodeUsageState(std::string_view state) {
      static constexpr std::array<std::string_view, 9> STATES = {
        "ok", "scope_missing", "auth_invalid", "rate_limited", "unsupported_credential", "unreachable", "upstream_error", "http_error", "invalid_response",
      };
      for (const std::string_view known : STATES)
        if (known == state) return true;
      return false;
    }

    std::optional<ClaudeCodeUsageState> parseClaudeCodeUsageState(const rt::KvValue& value) {
      if (!rt::kvIsObject(value) || !isText(value["source"], "oauth_usage") || !value["state"].isString() || !claudeCodeUsageState(value["state"].asString()))
        return std::nullopt;
      const std::optional<double> attemptedAtMs = positiveSafeInteger(value["attempted_at_ms"]);
      const rt::KvValue& rawObservedAtMs = value["observed_at_ms"];
      const std::optional<double> observedAtMs = rawObservedAtMs.isNull() ? std::nullopt : positiveSafeInteger(rawObservedAtMs);
      const std::optional<double> retryAtMs = positiveSafeInteger(value["retry_at_ms"]);
      if (!attemptedAtMs || (!rawObservedAtMs.isNull() && !observedAtMs)) return std::nullopt;
      const rt::KvValue& status = value["status"];
      if (!status.isNull() &&
          (!status.isNumber() || !rt::Number::isInteger(status.asNumber()) || status.asNumber() < 100 || status.asNumber() > 599))
        return std::nullopt;
      const rt::KvValue& message = value["message"];
      if (!message.isNull() && !message.isString()) return std::nullopt;
      const std::string& state = value["state"].asString();
      ClaudeCodeUsageState parsed{
        .source = "oauth_usage",
        .state = state,
        .status = status.isNull() ? std::nullopt : std::optional<double>(status.asNumber()),
        .message = sanitizeClaudeCodeErrorMessage(message),
        .attempted_at_ms = *attemptedAtMs,
        .observed_at_ms = observedAtMs,
      };
      if (state == "rate_limited" && retryAtMs && *retryAtMs > *attemptedAtMs) parsed.retry_at_ms = retryAtMs;
      return parsed;
    }

    std::optional<ClaudeCodeWindowId> parseBlockWindow(const rt::KvValue& value) {
      for (const std::string_view window : CLAUDE_CODE_WINDOWS)
        if (isText(value, window)) return std::string(window);
      return std::nullopt;
    }

    std::optional<ClaudeCodeHeaderObservation> parseHeaderObservation(const rt::KvValue& value) {
      if (!rt::kvIsObject(value) || !isText(value["source"], "response_headers") || !value["usage_snapshot"].isUndefined() || !positiveSafeInteger(value["observed_at_ms"]))
        return std::nullopt;
      const rt::KvValue& claim = value["representative_claim"];
      return ClaudeCodeHeaderObservation{
        .source = "response_headers",
        .windows = parseStoredWindows(value["windows"]),
        .standard = parseStoredStandardWindows(value["standard"]),
        .representative_claim = claim.isString() ? std::optional<std::string>(claim.asString()) : std::nullopt,
        .observed_at_ms = value["observed_at_ms"].asNumber(),
      };
    }

    // `newerWindow(previous, next)`: `previous && previous.observed_at_ms > next.observed_at_ms ? previous : next`
    template <class T> const T& newerWindow(const T* previous, const T& next) {
      return previous != nullptr && previous->observed_at_ms > next.observed_at_ms ? *previous : next;
    }
    double snapshotObservedAt(const ClaudeCodeUsageSnapshot& snapshot) {
      const json::value* observed = rt::get(snapshot, "observed_at_ms");
      return observed != nullptr && rt::isNumber(*observed) ? rt::toNumber(*observed) : std::numeric_limits<double>::quiet_NaN();
    }

    ClaudeCodeQuotaState withHeaderObservation(ClaudeCodeQuotaState state, const QuotaOutcome& input) {
      if (input.observation_source != std::optional<std::string>("response_headers") || !input.observation || observationIsEmpty(*input.observation)) return state;
      const std::optional<ClaudeCodeHeaderObservation> previous = state.response_headers;
      const ClaudeCodeQuotaObservation& observation = *input.observation;
      ClaudeCodeQuotaWindows windows = previous ? previous->windows : ClaudeCodeQuotaWindows();
      ClaudeCodeStandardWindows standard = previous ? previous->standard : ClaudeCodeStandardWindows();
      for (const std::string_view window : CLAUDE_CODE_WINDOWS)
        if (const ClaudeCodeQuotaWindow* observed = observation.windows.get(window)) {
          const ClaudeCodeQuotaWindow newer = newerWindow(windows.get(window), *observed);
          windows.set(std::string(window), newer);
        }
      for (const std::string_view family : CLAUDE_CODE_STANDARD_FAMILIES)
        if (const ClaudeCodeStandardWindow* observed = observation.standard.get(family)) {
          const ClaudeCodeStandardWindow newer = newerWindow(standard.get(family), *observed);
          standard.set(std::string(family), newer);
        }
      std::optional<std::string> claim = observation.representative_claim;
      if (!claim && previous) claim = previous->representative_claim;
      state.response_headers = ClaudeCodeHeaderObservation{
        .source = "response_headers",
        .windows = std::move(windows),
        .standard = std::move(standard),
        .representative_claim = std::move(claim),
        .observed_at_ms = rt::Math::max(previous ? previous->observed_at_ms : 0.0, observation.observed_at_ms),
      };
      return state;
    }

    std::optional<std::pair<std::string, ClaudeCodePoolProof>> newPoolProof(const QuotaOutcome& input, double observedAt) {
      if (!input.binding || !input.model_id || input.model_id->empty()) return std::nullopt;
      const std::optional<double> reset = input.exhaustion ? input.exhaustion->reset_at_ms : std::nullopt;
      const bool authInvalid = input.auth_invalid.value_or(false);
      std::optional<double> expiry;
      if (authInvalid) expiry = observedAt + CLAUDE_CODE_AUTH_INVALID_PROOF_TTL_MS;
      else if (reset && rt::truthy(*reset) && *reset > observedAt) expiry = *reset + CLAUDE_CODE_RESET_MARGIN_MS;
      if (!expiry) return std::nullopt;
      return std::pair<std::string, ClaudeCodePoolProof>{
        authInvalid ? std::string("credential") : "model:" + *input.model_id,
        ClaudeCodePoolProof{
          .kind = authInvalid ? "auth_invalid" : "quota_exhausted",
          .model_id = authInvalid ? std::nullopt : input.model_id,
          .observed_at_ms = observedAt,
          .expires_at_ms = *expiry,
          .evidence_generation = input.binding->evidence_generation,
        },
      };
    }

    Record<ClaudeCodePoolProof> poolProofsForOutcome(const std::optional<ClaudeCodeQuotaState>& previous, const QuotaOutcome& input, double nowMs) {
      const double observedAt = input.response_observed_at_ms.value_or(nowMs);
      const std::optional<std::string> modelId = input.model_id;
      rt::OrderedMap<std::string, ClaudeCodePoolProof> proofs;
      if (previous && previous->pool_proofs)
        for (const std::string& key : previous->pool_proofs->keys()) {
          const ClaudeCodePoolProof& proof = *previous->pool_proofs->get(key);
          if (proof.expires_at_ms > nowMs) proofs.set(key, proof);
        }
      if (input.served.value_or(false)) {
        std::vector<std::string> served;
        for (const auto& [key, proof] : proofs)
          if (proof.observed_at_ms <= observedAt && (proof.kind == "auth_invalid" || proof.model_id == modelId)) served.push_back(key);
        for (const std::string& key : served) proofs.delete_(key);
      }
      const std::optional<std::pair<std::string, ClaudeCodePoolProof>> next = newPoolProof(input, observedAt);
      if (next && next->second.expires_at_ms > nowMs) {
        const ClaudeCodePoolProof* previousProof = proofs.get(next->first);
        if (previousProof == nullptr || previousProof->observed_at_ms <= observedAt) proofs.set(next->first, next->second);
      }
      Record<ClaudeCodePoolProof> out;
      for (const auto& [key, proof] : proofs) out.define(key, proof);
      return out;
    }

    ClaudeCodeQuotaState quotaStateForOutcome(const std::optional<ClaudeCodeQuotaState>& previous, const QuotaOutcome& input, double nowMs) {
      const ClaudeCodeQuotaObservation observation =
        input.observation ? *input.observation
                          : ClaudeCodeQuotaObservation{.windows = {}, .standard = {}, .representative_claim = std::nullopt, .observed_at_ms = nowMs};
      ClaudeCodeQuotaState withCooldown = withHeaderObservation(mergeClaudeCodeQuotaState(previous, observation, input.exhaustion, nowMs), input);
      if (input.transient) {
        const std::optional<double> retryAfterMs = input.transient->retry_after_ms;
        withCooldown.cooldown_until_ms = nowMs + (retryAfterMs && *retryAfterMs > 0 ? *retryAfterMs : CLAUDE_CODE_TRANSIENT_COOLDOWN_MS);
      }
      if (input.config_error) {
        const std::optional<double> configRetryAfterMs = input.config_error->retry_after_ms;
        const double retryAfterMs = configRetryAfterMs && *configRetryAfterMs > 0 ? *configRetryAfterMs : CLAUDE_CODE_CONFIG_COOLDOWN_MS;
        withCooldown.cooldown_until_ms = nowMs + retryAfterMs;
        withCooldown.last_error = ClaudeCodeQuotaError{
          .kind = input.config_error->kind,
          .status = input.config_error->status,
          .message = sanitizeClaudeCodeErrorMessage(input.config_error->message),
          .at_ms = nowMs,
        };
      } else if (input.served.value_or(false)) {
        withCooldown.last_error = std::nullopt;
      }
      if (input.usage && (!previous || !previous->usage || input.usage->attempted_at_ms >= previous->usage->attempted_at_ms)) withCooldown.usage = input.usage;
      if (input.binding) withCooldown.pool_proofs = poolProofsForOutcome(previous, input, nowMs);
      return withCooldown;
    }

    rt::Task<void> publishAcceptedObservation(std::shared_ptr<rt::Kv> kv, QuotaOutcome input, ClaudeCodeQuotaState withCooldown, double nowMs) {
      const ObservationRecorder recorder = observationRecorder();
      if (!(input.binding && input.observation && !observationIsEmpty(*input.observation) && recorder)) co_return;
      ClaudeCodeAcceptedObservation event{};
      static_cast<claude_code_accounts::ClaudeCodeAccountIdentity&>(event) = claude_code_accounts::claudeCodeAccountIdentity(*input.binding);
      event.v = 1;
      event.observation_id = rt::crypto::randomUUID();
      const double sourceObservedAtMs = input.observation->observed_at_ms;
      event.source_observed_at_ms = rt::Number::isFinite(sourceObservedAtMs) && sourceObservedAtMs > 0 ? std::optional<double>(sourceObservedAtMs) : std::nullopt;
      event.sampled_at_ms = nowMs;
      event.response_status = input.response_status;
      event.state = input.auth_invalid.value_or(false) || claudeCodeQuotaBlocked(withCooldown, nowMs) || claudeCodeQuotaCoolingDown(withCooldown, nowMs)
                      ? "unavailable"
                      : "available";
      event.observation = *input.observation;
      if (input.observation_source && !input.observation_source->empty()) event.source = input.observation_source;
      bool failed = false;
      try {
        rt::Promise<void> recording(recorder(kv, std::move(event)));
        co_await recording.wait();
      } catch (...) {
        failed = true;
      }
      if (failed) rt::console::warn("[ai.ubq.fi] Claude observation history write failed after accepted quota persistence.");
    }
  }

  std::function<void()> registerClaudeCodeObservationRecorder(ObservationRecorder recorder) {
    ObservationRecorder& slot = observationRecorder();
    if (slot && !sameRecorder(slot, recorder)) throw rt::JsError("Claude observation recorder is already installed");
    slot = recorder;
    return [recorder]() {
      ObservationRecorder& installed = observationRecorder();
      if (sameRecorder(installed, recorder)) installed = nullptr;
    };
  }

  rt::KvKey claudeCodeQuotaKey(const std::optional<ClaudeCodeAccountBinding>& binding) {
    if (binding) return rt::KvKey{"uos_ai", "claude_code_quota", "v1", binding->account_id, binding->account_cohort_id};
    return CLAUDE_CODE_QUOTA_KV_KEY;
  }
  rt::KvKey claudeCodeQuotaKey() { return CLAUDE_CODE_QUOTA_KV_KEY; }

  std::optional<ClaudeCodePoolProof> claudeCodePoolProofForModel(const std::optional<ClaudeCodeQuotaState>& state, const ClaudeCodeAccountBinding& binding,
                                                                 const std::string& model, double nowMs) {
    if (!state || !state->pool_proofs) return std::nullopt;
    for (const ClaudeCodePoolProof* proof : state->pool_proofs->values())
      if (proof->evidence_generation == binding.evidence_generation && proof->observed_at_ms <= nowMs && proof->expires_at_ms > nowMs &&
          (proof->kind == "auth_invalid" || proof->model_id == std::optional<std::string>(model)))
        return *proof;
    return std::nullopt;
  }
  std::optional<ClaudeCodePoolProof> claudeCodePoolProofForModel(const std::optional<ClaudeCodeQuotaState>& state, const ClaudeCodeAccountBinding& binding,
                                                                 const std::string& model) {
    return claudeCodePoolProofForModel(state, binding, model, rt::dateNow());
  }

  std::optional<ClaudeCodeQuotaObservation> observeClaudeCodeQuota(const rt::Headers& headers, double nowMs) {
    if (!rt::Number::isFinite(nowMs) || nowMs <= 0) return std::nullopt;
    const double observedAtMs = rt::Math::trunc(nowMs);
    ClaudeCodeQuotaWindows windows;
    for (const std::string_view window : CLAUDE_CODE_WINDOWS)
      if (std::optional<ClaudeCodeQuotaWindow> parsed = parseWindow(headers, window, observedAtMs)) windows.set(std::string(window), std::move(*parsed));
    ClaudeCodeStandardWindows standard;
    for (const std::string_view family : CLAUDE_CODE_STANDARD_FAMILIES)
      if (std::optional<ClaudeCodeStandardWindow> parsed = parseStandardWindow(headers, family, observedAtMs)) standard.set(std::string(family), std::move(*parsed));
    const std::optional<std::string> claim = headers.get(std::string(HEADER_PREFIX) + "representative-claim");
    ClaudeCodeQuotaObservation observation{
      .windows = std::move(windows),
      .standard = std::move(standard),
      .representative_claim = trimmedOrNull(claim),
      .observed_at_ms = observedAtMs,
    };
    if (observationIsEmpty(observation)) return std::nullopt;
    return observation;
  }
  std::optional<ClaudeCodeQuotaObservation> observeClaudeCodeQuota(const rt::Headers& headers) { return observeClaudeCodeQuota(headers, rt::dateNow()); }

  std::optional<std::string> sanitizeClaudeCodeErrorMessage(const std::string& raw, double maxLength) {
    static const rt::Regex WHITESPACE("\\s+", "g");
    const std::string collapsed = rt::trim(rt::replace(raw, WHITESPACE, " "));
    if (collapsed.empty()) return std::nullopt;
    return static_cast<double>(rt::length(collapsed)) > maxLength ? rt::slice(collapsed, 0, maxLength) : collapsed;
  }
  std::optional<std::string> sanitizeClaudeCodeErrorMessage(const rt::KvValue& raw, double maxLength) {
    if (!raw.isString()) return std::nullopt;
    return sanitizeClaudeCodeErrorMessage(raw.asString(), maxLength);
  }
  std::optional<std::string> sanitizeClaudeCodeErrorMessage(const std::optional<std::string>& raw, double maxLength) {
    if (!raw) return std::nullopt;
    return sanitizeClaudeCodeErrorMessage(*raw, maxLength);
  }

  std::optional<ClaudeCodeQuotaError> parseClaudeCodeQuotaError(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return std::nullopt;
    const rt::KvValue& rawKind = value["kind"];
    std::optional<std::string> kind;
    if (rawKind.isString() && !rt::trim(rawKind.asString()).empty()) kind = rt::trim(rawKind.asString());
    const std::optional<double> atMs = finiteNumber(value["at_ms"]);
    if (!kind || !atMs || *atMs <= 0) return std::nullopt;
    return ClaudeCodeQuotaError{
      .kind = std::move(*kind),
      .status = finiteNumber(value["status"]),
      .message = sanitizeClaudeCodeErrorMessage(value["message"]),
      .at_ms = rt::Math::trunc(*atMs),
    };
  }

  std::optional<ClaudeCodeQuotaExhaustion> claudeCodeExhaustionFromResponse(double status, const std::optional<ClaudeCodeQuotaObservation>& observation) {
    if (status != 429 || !observation) return std::nullopt;
    for (const std::string_view window : CLAUDE_CODE_WINDOWS) {
      const ClaudeCodeQuotaWindow* observed = observation->windows.get(window);
      if (observed == nullptr) continue;
      const bool statusLimited = observed->status && rt::toLowerCase(rt::trim(*observed->status)) != "allowed";
      const bool utilizationLimited = observed->utilization && *observed->utilization >= 1;
      if (statusLimited || utilizationLimited) return ClaudeCodeQuotaExhaustion{.window = std::string(window), .reset_at_ms = observed->reset_at_ms};
    }
    return std::nullopt;
  }

  std::optional<ClaudeCodeQuotaState> parseClaudeCodeQuotaState(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return std::nullopt;
    const std::optional<double> updatedAtMs = finiteNumber(value["updated_at_ms"]);
    if (!updatedAtMs || *updatedAtMs <= 0) return std::nullopt;
    ClaudeCodeQuotaState state{
      .response_headers = parseHeaderObservation(value["response_headers"]),
      .usage_snapshot = std::nullopt,
      .pool_proofs = std::nullopt,
      .windows = parseStoredWindows(value["windows"]),
      .standard = parseStoredStandardWindows(value["standard"]),
      .usage = parseClaudeCodeUsageState(value["usage"]),
      .representative_claim = value["representative_claim"].isString() ? std::optional<std::string>(value["representative_claim"].asString()) : std::nullopt,
      .blocked_until_ms = finiteNumber(value["blocked_until_ms"]),
      .block_window = parseBlockWindow(value["block_window"]),
      .cooldown_until_ms = finiteNumber(value["cooldown_until_ms"]),
      .last_error = parseClaudeCodeQuotaError(value["last_error"]),
      .updated_at_ms = rt::Math::trunc(*updatedAtMs),
    };
    if (!value["pool_proofs"].isUndefined()) state.pool_proofs = parsePoolProofs(value["pool_proofs"]);
    const json::value snapshotView = rt::storedJsonView(value["usage_snapshot"], STORED_JSON_VIEW_OPTIONS);
    if (claude_code_usage_schema::readClaudeCodeUsageSnapshot(snapshotView)) state.usage_snapshot = claude_code_usage_schema::readClaudeCodeUsageSnapshot(snapshotView);
    return state;
  }

  rt::Task<std::optional<ClaudeCodeQuotaState>> loadClaudeCodeQuotaState(std::shared_ptr<rt::Kv> kv, std::optional<ClaudeCodeAccountBinding> binding) {
    if (!kv) co_return std::nullopt;
    if (binding) {
      rt::Promise<std::optional<rt::KvEntryMaybe<>>> reading(claude_code_accounts::currentClaudeCodeAccountEntry(kv, *binding));
      const std::optional<rt::KvEntryMaybe<>> accountEntry = co_await reading.wait();
      if (!accountEntry) co_return std::nullopt;
    }
    const rt::KvKey key = claudeCodeQuotaKey(binding);
    const rt::KvEntryMaybe<> entry = co_await kv->get(key, STRONG);
    co_return parseClaudeCodeQuotaState(entry.value);
  }
  rt::Task<std::optional<ClaudeCodeQuotaState>> loadClaudeCodeQuotaState(std::shared_ptr<rt::Kv> kv) { return loadClaudeCodeQuotaState(std::move(kv), std::nullopt); }

  ClaudeCodeQuotaState mergeClaudeCodeQuotaState(const std::optional<ClaudeCodeQuotaState>& previous, const ClaudeCodeQuotaObservation& observation,
                                                 const std::optional<ClaudeCodeQuotaExhaustion>& block, double nowMs) {
    ClaudeCodeQuotaWindows windows = previous ? previous->windows : ClaudeCodeQuotaWindows();
    for (const std::string_view window : CLAUDE_CODE_WINDOWS)
      if (const ClaudeCodeQuotaWindow* observed = observation.windows.get(window)) {
        const ClaudeCodeQuotaWindow newer = newerWindow(windows.get(window), *observed);
        windows.set(std::string(window), newer);
      }
    ClaudeCodeStandardWindows standard = previous ? previous->standard : ClaudeCodeStandardWindows();
    for (const std::string_view family : CLAUDE_CODE_STANDARD_FAMILIES)
      if (const ClaudeCodeStandardWindow* observed = observation.standard.get(family)) {
        const ClaudeCodeStandardWindow newer = newerWindow(standard.get(family), *observed);
        standard.set(std::string(family), newer);
      }
    std::optional<double> blockedUntilMs = previous ? previous->blocked_until_ms : std::nullopt;
    std::optional<ClaudeCodeWindowId> blockWindow = previous ? previous->block_window : std::nullopt;
    if (previous && blockedUntilMs && *blockedUntilMs <= nowMs) {
      blockedUntilMs = std::nullopt;
      blockWindow = std::nullopt;
    }
    if (block) {
      const double resetBased = !block->reset_at_ms ? nowMs + CLAUDE_CODE_MIN_BLOCK_MS : *block->reset_at_ms + CLAUDE_CODE_RESET_MARGIN_MS;
      const double candidate = rt::Math::max(resetBased, nowMs + CLAUDE_CODE_MIN_BLOCK_MS);
      if (!blockedUntilMs || candidate > *blockedUntilMs) {
        blockedUntilMs = candidate;
        blockWindow = block->window;
      }
    }
    std::optional<ClaudeCodeUsageSnapshot> usageSnapshot;
    if (observation.usage_snapshot) {
      const ClaudeCodeUsageSnapshot* prior = previous && previous->usage_snapshot ? &*previous->usage_snapshot : nullptr;
      usageSnapshot = prior != nullptr && snapshotObservedAt(*prior) > snapshotObservedAt(*observation.usage_snapshot) ? *prior : *observation.usage_snapshot;
    } else if (previous) {
      usageSnapshot = previous->usage_snapshot;
    }
    std::optional<std::string> claim = observation.representative_claim;
    if (!claim && previous) claim = previous->representative_claim;
    return {
      .response_headers = previous ? previous->response_headers : std::nullopt,
      .usage_snapshot = std::move(usageSnapshot),
      .pool_proofs = std::nullopt,
      .windows = std::move(windows),
      .standard = std::move(standard),
      .usage = previous ? previous->usage : std::nullopt,
      .representative_claim = std::move(claim),
      .blocked_until_ms = blockedUntilMs,
      .block_window = std::move(blockWindow),
      .cooldown_until_ms = previous ? previous->cooldown_until_ms : std::nullopt,
      .last_error = previous ? previous->last_error : std::nullopt,
      .updated_at_ms = nowMs,
    };
  }
  ClaudeCodeQuotaState mergeClaudeCodeQuotaState(const std::optional<ClaudeCodeQuotaState>& previous, const ClaudeCodeQuotaObservation& observation,
                                                 const std::optional<ClaudeCodeQuotaExhaustion>& block) {
    return mergeClaudeCodeQuotaState(previous, observation, block, rt::dateNow());
  }

  bool claudeCodeQuotaBlocked(const std::optional<ClaudeCodeQuotaState>& state, double nowMs) {
    return state && state->blocked_until_ms && *state->blocked_until_ms > nowMs;
  }
  bool claudeCodeQuotaBlocked(const std::optional<ClaudeCodeQuotaState>& state) { return claudeCodeQuotaBlocked(state, rt::dateNow()); }

  bool claudeCodeQuotaCoolingDown(const std::optional<ClaudeCodeQuotaState>& state, double nowMs) {
    return state && state->cooldown_until_ms && *state->cooldown_until_ms > nowMs;
  }
  bool claudeCodeQuotaCoolingDown(const std::optional<ClaudeCodeQuotaState>& state) { return claudeCodeQuotaCoolingDown(state, rt::dateNow()); }

  rt::Task<std::optional<ClaudeCodeQuotaState>> recordClaudeCodeQuotaOutcome(std::shared_ptr<rt::Kv> kv, QuotaOutcome input) {
    const double nowMs = input.nowMs ? *input.nowMs : rt::dateNow();
    std::optional<rt::KvEntryMaybe<>> accountEntry;
    if (kv && input.binding) {
      rt::Promise<std::optional<rt::KvEntryMaybe<>>> reading(claude_code_accounts::currentClaudeCodeAccountEntry(kv, *input.binding));
      accountEntry = co_await reading.wait();
    }
    if (input.binding && !accountEntry) co_return std::nullopt;
    std::optional<rt::KvEntryMaybe<>> quotaEntry;
    if (kv) {
      const rt::KvKey key = claudeCodeQuotaKey(input.binding);
      quotaEntry = co_await kv->get(key, STRONG);
    }
    const std::optional<ClaudeCodeQuotaState> previous = parseClaudeCodeQuotaState(quotaEntry ? quotaEntry->value : rt::KvValue());
    if (!input.observation && !input.exhaustion && !input.transient && !input.config_error && !input.served.value_or(false) && !input.auth_invalid.value_or(false) &&
        !input.usage)
      co_return previous;
    ClaudeCodeQuotaState withCooldown = quotaStateForOutcome(previous, input, nowMs);
    if (!kv) co_return std::nullopt;
    std::vector<rt::AtomicCheck> checks;
    if (accountEntry) checks.emplace_back(*accountEntry);
    if (quotaEntry) checks.emplace_back(*quotaEntry);
    std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
    atomic->check(std::move(checks));
    atomic->set(claudeCodeQuotaKey(input.binding), rt::KvValue(json::value_from(withCooldown)));
    const rt::KvCommitResult stored = co_await atomic->commit();
    if (!stored.ok) {
      const double attempts = input.attempts.value_or(0);
      if (!(attempts < 8)) co_return std::nullopt;
      QuotaOutcome retry = input;
      retry.nowMs = nowMs;
      retry.attempts = attempts + 1;
      rt::Promise<std::optional<ClaudeCodeQuotaState>> retrying(recordClaudeCodeQuotaOutcome(kv, std::move(retry)));
      std::optional<ClaudeCodeQuotaState> retried = co_await retrying.wait();
      co_return retried;
    }
    rt::Promise<void> publishing(publishAcceptedObservation(kv, input, withCooldown, nowMs));
    co_await publishing.wait();
    co_return withCooldown;
  }

  rt::Task<std::optional<ClaudeCodeQuotaState>> clearClaudeCodeQuotaGuards(std::shared_ptr<rt::Kv> kv, std::optional<ClaudeCodeAccountBinding> binding) {
    if (!kv) co_return std::nullopt;
    std::optional<rt::KvEntryMaybe<>> accountEntry;
    if (binding) {
      rt::Promise<std::optional<rt::KvEntryMaybe<>>> reading(claude_code_accounts::currentClaudeCodeAccountEntry(kv, *binding));
      accountEntry = co_await reading.wait();
    }
    if (binding && !accountEntry) co_return std::nullopt;
    const rt::KvKey key = claudeCodeQuotaKey(binding);
    const rt::KvEntryMaybe<> quotaEntry = co_await kv->get(key, STRONG);
    const std::optional<ClaudeCodeQuotaState> previous = parseClaudeCodeQuotaState(quotaEntry.value);
    // Even an empty guard record invalidates older in-flight evidence.
    if (!previous && !accountEntry) co_return std::nullopt;
    ClaudeCodeQuotaState cleared;
    if (previous) {
      cleared = *previous;
    } else {
      const ClaudeCodeQuotaObservation empty{.windows = {}, .standard = {}, .representative_claim = std::nullopt, .observed_at_ms = rt::dateNow()};
      cleared = mergeClaudeCodeQuotaState(std::nullopt, empty, std::nullopt);
    }
    cleared.blocked_until_ms = std::nullopt;
    cleared.block_window = std::nullopt;
    cleared.cooldown_until_ms = std::nullopt;
    cleared.last_error = std::nullopt;
    cleared.pool_proofs = Record<ClaudeCodePoolProof>();
    cleared.updated_at_ms = rt::dateNow();
    std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
    operation->check(quotaEntry);
    if (accountEntry && accountEntry->value.truthy() && binding) {
      // `{ ...accountEntry.value, accounts: accountEntry.value.accounts.map(...) }` over the stored roster.
      rt::KvValue roster = accountEntry->value;
      if (roster.isObject() && roster["accounts"].isArray()) {
        rt::KvArray accounts = roster["accounts"].asArray();
        for (rt::KvValue& account : accounts) {
          if (account.is<rt::KvHole>()) continue;
          if (isText(account["account_id"], binding->account_id) && account.isObject()) {
            rt::KvObject renewed = account.asObject();
            renewed.set("evidence_generation", rt::KvValue(rt::crypto::randomUUID()));
            account = rt::KvValue(std::move(renewed));
          }
        }
        roster.asObject().set("accounts", rt::KvValue(std::move(accounts)));
      }
      operation->check(*accountEntry).set(claude_code_accounts::CLAUDE_CODE_ACCOUNTS_KEY, std::move(roster));
    }
    operation->set(key, rt::KvValue(json::value_from(cleared)));
    const rt::KvCommitResult committed = co_await operation->commit();
    if (!committed.ok) co_return std::nullopt;
    co_return cleared;
  }
  rt::Task<std::optional<ClaudeCodeQuotaState>> clearClaudeCodeQuotaGuards(std::shared_ptr<rt::Kv> kv) { return clearClaudeCodeQuotaGuards(std::move(kv), std::nullopt); }

  // ---- JSON conversions -----------------------------------------------------------------------

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeQuotaWindow& value) {
    out = Writer()
            .orNull("status", value.status)
            .orNull("utilization", value.utilization)
            .orNull("reset_at_ms", value.reset_at_ms)
            .required("observed_at_ms", value.observed_at_ms)
            .done();
  }
  ClaudeCodeQuotaWindow tag_invoke(const json::value_to_tag<ClaudeCodeQuotaWindow>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeQuotaWindow");
    return {
      .status = in.orNull<std::string>("status"),
      .utilization = in.orNull<double>("utilization"),
      .reset_at_ms = in.orNull<double>("reset_at_ms"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeStandardWindow& value) {
    out = Writer()
            .orNull("limit", value.limit)
            .orNull("remaining", value.remaining)
            .orNull("reset_at_ms", value.reset_at_ms)
            .required("observed_at_ms", value.observed_at_ms)
            .done();
  }
  ClaudeCodeStandardWindow tag_invoke(const json::value_to_tag<ClaudeCodeStandardWindow>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeStandardWindow");
    return {
      .limit = in.orNull<double>("limit"),
      .remaining = in.orNull<double>("remaining"),
      .reset_at_ms = in.orNull<double>("reset_at_ms"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeQuotaError& value) {
    out = Writer().required("kind", value.kind).orNull("status", value.status).orNull("message", value.message).required("at_ms", value.at_ms).done();
  }
  ClaudeCodeQuotaError tag_invoke(const json::value_to_tag<ClaudeCodeQuotaError>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeQuotaError");
    return {
      .kind = in.required<std::string>("kind"),
      .status = in.orNull<double>("status"),
      .message = in.orNull<std::string>("message"),
      .at_ms = in.required<double>("at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeQuotaObservation& value) {
    out = Writer()
            .required("windows", recordJson(value.windows))
            .required("standard", recordJson(value.standard))
            .orNull("representative_claim", value.representative_claim)
            .required("observed_at_ms", value.observed_at_ms)
            .orUndefined("usage_snapshot", value.usage_snapshot)
            .done();
  }
  ClaudeCodeQuotaObservation tag_invoke(const json::value_to_tag<ClaudeCodeQuotaObservation>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeQuotaObservation");
    return {
      .windows = recordFrom<ClaudeCodeQuotaWindow>(in.required<json::object>("windows")),
      .standard = recordFrom<ClaudeCodeStandardWindow>(in.required<json::object>("standard")),
      .representative_claim = in.orNull<std::string>("representative_claim"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
      .usage_snapshot = in.orUndefined<json::object>("usage_snapshot"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeHeaderObservation& value) {
    out = Writer()
            .required("source", value.source)
            .required("windows", recordJson(value.windows))
            .required("standard", recordJson(value.standard))
            .orNull("representative_claim", value.representative_claim)
            .required("observed_at_ms", value.observed_at_ms)
            .done();
  }
  ClaudeCodeHeaderObservation tag_invoke(const json::value_to_tag<ClaudeCodeHeaderObservation>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeHeaderObservation");
    const std::string source = in.required<std::string>("source");
    if (source != "response_headers") in.fail("source", rt::get(value, "source"), "\"response_headers\"");
    return {
      .source = source,
      .windows = recordFrom<ClaudeCodeQuotaWindow>(in.required<json::object>("windows")),
      .standard = recordFrom<ClaudeCodeStandardWindow>(in.required<json::object>("standard")),
      .representative_claim = in.orNull<std::string>("representative_claim"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeAcceptedObservation& value) {
    out = Writer()
            .required("account_id", value.account_id)
            .required("account_cohort_id", value.account_cohort_id)
            .required("credential_kind", value.credential_kind)
            .orNull("workspace_id", value.workspace_id)
            .required("v", value.v)
            .required("observation_id", value.observation_id)
            .orNull("source_observed_at_ms", value.source_observed_at_ms)
            .required("sampled_at_ms", value.sampled_at_ms)
            .orNull("response_status", value.response_status)
            .required("state", value.state)
            .required("observation", value.observation)
            .orUndefined("source", value.source)
            .done();
  }
  ClaudeCodeAcceptedObservation tag_invoke(const json::value_to_tag<ClaudeCodeAcceptedObservation>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeAcceptedObservation");
    ClaudeCodeAcceptedObservation out{};
    out.account_id = in.required<std::string>("account_id");
    out.account_cohort_id = in.required<std::string>("account_cohort_id");
    out.credential_kind = in.required<std::string>("credential_kind");
    out.workspace_id = in.orNull<std::string>("workspace_id");
    out.v = in.required<double>("v");
    if (out.v != 1) in.fail("v", rt::get(value, "v"), "1");
    out.observation_id = in.required<std::string>("observation_id");
    out.source_observed_at_ms = in.orNull<double>("source_observed_at_ms");
    out.sampled_at_ms = in.required<double>("sampled_at_ms");
    out.response_status = in.orNull<double>("response_status");
    out.state = in.required<std::string>("state");
    out.observation = in.required<ClaudeCodeQuotaObservation>("observation");
    out.source = in.orUndefined<std::string>("source");
    return out;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeUsageState& value) {
    out = Writer()
            .required("source", value.source)
            .required("state", value.state)
            .orNull("status", value.status)
            .orNull("message", value.message)
            .required("attempted_at_ms", value.attempted_at_ms)
            .orNull("observed_at_ms", value.observed_at_ms)
            .orUndefined("retry_at_ms", value.retry_at_ms)
            .done();
  }
  ClaudeCodeUsageState tag_invoke(const json::value_to_tag<ClaudeCodeUsageState>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeUsageState");
    const std::string source = in.required<std::string>("source");
    if (source != "oauth_usage") in.fail("source", rt::get(value, "source"), "\"oauth_usage\"");
    return {
      .source = source,
      .state = in.required<std::string>("state"),
      .status = in.orNull<double>("status"),
      .message = in.orNull<std::string>("message"),
      .attempted_at_ms = in.required<double>("attempted_at_ms"),
      .observed_at_ms = in.orNull<double>("observed_at_ms"),
      .retry_at_ms = in.orUndefined<double>("retry_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodePoolProof& value) {
    out = Writer()
            .required("kind", value.kind)
            .orNull("model_id", value.model_id)
            .required("observed_at_ms", value.observed_at_ms)
            .required("expires_at_ms", value.expires_at_ms)
            .required("evidence_generation", value.evidence_generation)
            .done();
  }
  ClaudeCodePoolProof tag_invoke(const json::value_to_tag<ClaudeCodePoolProof>&, const json::value& value) {
    const Reader in(value, "ClaudeCodePoolProof");
    return {
      .kind = in.required<std::string>("kind"),
      .model_id = in.orNull<std::string>("model_id"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .evidence_generation = in.required<std::string>("evidence_generation"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeQuotaState& value) {
    const std::optional<json::object> poolProofs = value.pool_proofs ? std::optional<json::object>(recordJson(*value.pool_proofs)) : std::nullopt;
    out = Writer()
            .orUndefined("response_headers", value.response_headers)
            .orUndefined("usage_snapshot", value.usage_snapshot)
            .orUndefined("pool_proofs", poolProofs)
            .required("windows", recordJson(value.windows))
            .required("standard", recordJson(value.standard))
            .orNull("usage", value.usage)
            .orNull("representative_claim", value.representative_claim)
            .orNull("blocked_until_ms", value.blocked_until_ms)
            .orNull("block_window", value.block_window)
            .orNull("cooldown_until_ms", value.cooldown_until_ms)
            .orNull("last_error", value.last_error)
            .required("updated_at_ms", value.updated_at_ms)
            .done();
  }
  ClaudeCodeQuotaState tag_invoke(const json::value_to_tag<ClaudeCodeQuotaState>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeQuotaState");
    const std::optional<json::object> poolProofs = in.orUndefined<json::object>("pool_proofs");
    return {
      .response_headers = in.orUndefined<ClaudeCodeHeaderObservation>("response_headers"),
      .usage_snapshot = in.orUndefined<json::object>("usage_snapshot"),
      .pool_proofs = poolProofs ? std::optional<Record<ClaudeCodePoolProof>>(recordFrom<ClaudeCodePoolProof>(*poolProofs)) : std::nullopt,
      .windows = recordFrom<ClaudeCodeQuotaWindow>(in.required<json::object>("windows")),
      .standard = recordFrom<ClaudeCodeStandardWindow>(in.required<json::object>("standard")),
      .usage = in.orNull<ClaudeCodeUsageState>("usage"),
      .representative_claim = in.orNull<std::string>("representative_claim"),
      .blocked_until_ms = in.orNull<double>("blocked_until_ms"),
      .block_window = in.orNull<std::string>("block_window"),
      .cooldown_until_ms = in.orNull<double>("cooldown_until_ms"),
      .last_error = in.orNull<ClaudeCodeQuotaError>("last_error"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }
}
