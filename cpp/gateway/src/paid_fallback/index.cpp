#include "uos/paid_fallback/index.hpp"

#include "uos/api_keys.hpp"
#include "uos/codex/index.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/ledger_admission.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/paid_fallback/ledger_settlement.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

namespace uos::paid_fallback::index {
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
    using rt::record::Reader;
    using rt::record::Writer;
    namespace contracts = authority::native_paid_contracts;
    using ::uos::api_keys::MICROCREDITS_PER_CREDIT;
    using ::uos::api_keys::PAID_FALLBACK_NO_LIMIT;
    using ::uos::types::ApiKeyRecord;

    // `{ consistency: "strong" }`
    rt::KvGetOptions strongRead() { return rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}; }

    // A stored KV value as the TS reads it through property access: what JSON can hold is kept, an
    // undefined member is absent, an undefined or hole element is null, and every other part (bigint,
    // KvU64, Date, Map, buffers, ...) becomes `{}`, an object no number/string/boolean/null check accepts.

    // `entry.value` of a read: null for an absent row.

    // `!readMeteredApiKey() && !readSurplusApiKey()`: both read a key or null.
    bool noPaidProviderKey() {
      const std::optional<std::string> metered = provider::metered::readMeteredApiKey();
      const std::optional<std::string> surplus = provider::surplus::readSurplusApiKey();
      return !(metered && !metered->empty()) && !(surplus && !surplus->empty());
    }

    bool isNonNegativeSafeInteger(const std::optional<double>& value) { return value && rt::Number::isSafeInteger(*value) && *value >= 0; }

    bool isNonNegativeFiniteNumber(double value) { return rt::Number::isFinite(value) && value >= 0; }

    bool isPositiveSafeInteger(const std::optional<double>& value) { return value && rt::Number::isSafeInteger(*value) && *value > 0; }

    // `record.paid_fallback_max_exposure_microcredits?.[model] ?? null`. Its only readers test it with
    // `typeof value === "number"`, so a member of another type reads as null here.
    std::optional<double> exposureOf(const ApiKeyRecord& record, const std::string& model) {
      if (!record.paid_fallback_max_exposure_microcredits) return std::nullopt;
      const json::value* exposure = rt::get(*record.paid_fallback_max_exposure_microcredits, model);
      if (exposure == nullptr || exposure->is_null()) return std::nullopt;
      return rt::getNumber(*exposure);
    }

    // `{ kind: "eligible"; unlimited: boolean } | Exclude<PaidFallbackReservationDecision, { kind: "reserved" }>`
    struct PaidFallbackEligibilityEligible {
      bool unlimited;
    };
    using PaidFallbackEligibility = std::variant<PaidFallbackEligibilityEligible, PaidFallbackReservationDecisionSkip, PaidFallbackReservationDecisionBlocked>;

    PaidFallbackEligibility evaluatePaidFallbackEligibility(const ApiKeyRecord& record, const std::string& model, bool allowUnrosteredModel) {
      if (!record.paid_fallback_enabled) return PaidFallbackReservationDecisionSkip{.reason = "disabled"};
      const bool unlimited = record.paid_fallback_limit_microcredits == PAID_FALLBACK_NO_LIMIT;
      if (!rt::includes(record.paid_fallback_model_ids, model)) {
        if (allowUnrosteredModel && unlimited && isPositiveSafeInteger(record.paid_fallback_quota_per_credit)) {
          return PaidFallbackEligibilityEligible{.unlimited = true};
        }
        return PaidFallbackReservationDecisionSkip{.reason = "model_not_priced"};
      }
      const std::optional<double> exposure = exposureOf(record, model);
      if ((!unlimited && !isPositiveSafeInteger(record.paid_fallback_limit_microcredits)) || !isPositiveSafeInteger(record.paid_fallback_quota_per_credit) ||
          (!unlimited && !isPositiveSafeInteger(exposure))) {
        return PaidFallbackReservationDecisionBlocked{.reason = "invalid_policy", .reset_at_ms = record.usage_reset_at_ms};
      }
      return PaidFallbackEligibilityEligible{.unlimited = unlimited};
    }

    /**
     * Maximum gateway exposure for one priced model, or null when that model's
     * pricing inputs cannot produce a finite, positive credit bound.
     */
    std::optional<double> maximumExposureForModel(std::optional<double> context, const json::value* coefficient, double quotaPerCredit) {
      const std::optional<double> number = rt::getNumber(coefficient);
      if (!rt::truthy(context) || !number || !rt::Number::isFinite(*number) || *number <= 0) return std::nullopt;
      const double exposure = rt::Math::ceil((*context * *number * MICROCREDITS_PER_CREDIT) / quotaPerCredit);
      return rt::Number::isSafeInteger(exposure) && exposure > 0 ? std::optional<double>(exposure) : std::nullopt;
    }

    // `PaidFallbackPolicyEntry = Readonly<{ record: ApiKeyRecord; check: Readonly<{ key: KvKey; versionstamp: string | null }> }>`
    // `windowMs` is the stored `record.window_ms` itself: the policy version prints it as it is.
    struct PaidFallbackPolicyEntry {
      ApiKeyRecord record;
      rt::AtomicCheck check;
      rt::KvValue windowMs;
    };

    // `idEntry.value` as the ApiKeyRecord the TS casts it to: `kv.get<ApiKeyRecord>` checks nothing
    // beyond hasStrictPaidFallbackKeyPolicy, so a row missing members that check does not cover (an
    // older row without usage_quota_version, say) is still admitted. The policy members come from the
    // checked row; the unchecked members reservePaidFallback reads are usage_reset_at_ms and window_ms
    // (NaN when not a number, which is what JS arithmetic on an absent member gives) and
    // paid_fallback_max_exposure_microcredits (exposureOf reads an object only); the members it never
    // reads keep a well-typed stored value and are otherwise empty.
    ApiKeyRecord castStrictKeyRecord(const json::value& stored) {
      const auto text = [&stored](std::string_view key) {
        const json::value* member = rt::get(stored, key);
        return member != nullptr && member->is_string() ? std::string(member->get_string()) : std::string();
      };
      const auto numberOr = [&stored](std::string_view key, double fallback) { return rt::getNumber(rt::get(stored, key)).value_or(fallback); };
      const double absent = std::numeric_limits<double>::quiet_NaN();
      std::vector<std::string> modelIds;
      for (const json::value& model : rt::get(stored, "paid_fallback_model_ids")->as_array()) modelIds.emplace_back(model.as_string());
      const json::value* reservationRequestId = rt::get(stored, "paid_fallback_reservation_request_id");
      const json::value* maximumExposure = rt::get(stored, "paid_fallback_max_exposure_microcredits");
      return ApiKeyRecord{
        .id = text("id"),
        .name = text("name"),
        .prefix = text("prefix"),
        .hash = text("hash"),
        .created_at_ms = numberOr("created_at_ms", 0),
        .expires_at_ms = numberOr("expires_at_ms", 0),
        .revoked_at_ms = rt::getNumber(rt::get(stored, "revoked_at_ms")),
        .usage_limit_requests = numberOr("usage_limit_requests", 0),
        .usage_requests = numberOr("usage_requests", 0),
        .usage_reset_at_ms = numberOr("usage_reset_at_ms", absent),
        .window_ms = numberOr("window_ms", absent),
        .usage_quota_version = numberOr("usage_quota_version", 0),
        .paid_fallback_enabled = rt::get(stored, "paid_fallback_enabled")->as_bool(),
        .paid_fallback_limit_microcredits = numberOr("paid_fallback_limit_microcredits", 0),
        .paid_fallback_spent_microcredits = numberOr("paid_fallback_spent_microcredits", 0),
        .paid_fallback_reserved_microcredits = numberOr("paid_fallback_reserved_microcredits", 0),
        .paid_fallback_reservation_request_id = reservationRequestId->is_string() ? std::optional<std::string>(std::string(reservationRequestId->get_string()))
                                                                                   : std::nullopt,
        .paid_fallback_model_ids = std::move(modelIds),
        .paid_fallback_quota_per_credit = numberOr("paid_fallback_quota_per_credit", 0),
        .paid_fallback_max_exposure_microcredits =
          maximumExposure != nullptr && maximumExposure->is_object() ? std::optional<json::object>(maximumExposure->get_object()) : std::nullopt,
        .paid_fallback_pricing_checked_at_ms = rt::getNumber(rt::get(stored, "paid_fallback_pricing_checked_at_ms")),
      };
    }

    // `kv.get<ApiKeyRecord>` then `hasStrictPaidFallbackKeyPolicy(idEntry.value)`.
    rt::Task<std::optional<PaidFallbackPolicyEntry>> loadStrictKeyRecord(std::shared_ptr<rt::Kv> kv, std::string keyId) {
      const rt::KvGetOptions strong = strongRead();
      const rt::KvKey idKey = api_keys::apiKeyIdKey(keyId);
      const rt::KvEntryMaybe<> idEntry = co_await kv->get(idKey, strong);
      const json::value stored = rt::storedJsonView(idEntry.value, STORED_JSON_VIEW_OPTIONS);
      if (!rt::truthy(stored) || !authority::native_paid_policy::hasStrictPaidFallbackKeyPolicy(stored)) co_return std::nullopt;
      co_return PaidFallbackPolicyEntry{
        .record = castStrictKeyRecord(stored),
        .check = rt::AtomicCheck(idEntry.key, idEntry.versionstamp),
        .windowMs = idEntry.value["window_ms"],
      };
    }

    double advanceUsageWindow(double resetAtMs, double windowMs, double nowMs) {
      if (nowMs < resetAtMs) return resetAtMs;
      const double initialStart = resetAtMs - windowMs;
      const double elapsedWindows = rt::Math::floor((nowMs - initialStart) / windowMs);
      return initialStart + (elapsedWindows + 1) * windowMs;
    }

    /**
     * Whether a blocked admission may be retried with another attempt. A due
     * pending settlement can free the reserved exposure that blocked this
     * admission, so it is reconciled here through the existing due gate instead of
     * leaving a deferred reservation stranded until another terminal event happens.
     * The caller's attempt cap keeps a genuinely unresolved window fail-closed.
     */
    rt::Task<bool> retryBlockedPaidFallbackAdmission(ledger_admission::PaidFallbackAdmissionBlockedReason reason, double attempt) {
      if (attempt >= 2) co_return false;
      if (reason == "concurrent_update") co_return true;
      if (reason != "limit_exceeded") co_return false;
      try {
        const rt::Promise<double> reconciling(ledger_backfill::reconcileDuePaidFallbacksV3(rt::dateNow()));
        co_await reconciling.wait();
      } catch (...) {
      }
      // The turn of the derived `.catch` promise.
      co_await rt::microtask();
      co_return true;
    }

    /**
     * Settlement gets one best-effort sweep because a request reached a terminal
     * state, not because a minute passed. A missing provider log is deliberately
     * left to the next authorized event; no autonomous retry is armed here. Tests
     * replace the sweep so a background provider-log read cannot race their KV
     * budgets; `null` restores the real one.
     */
    void defaultPaidFallbackTerminalSweep() { rt::spawn(ledger_backfill::reconcileDuePaidFallbacksV3(), rt::ignoreRejection); }

    // `let paidFallbackTerminalSweep = defaultPaidFallbackTerminalSweep`
    std::function<void()>& paidFallbackTerminalSweep() {
      static std::function<void()> sweep = defaultPaidFallbackTerminalSweep;
      return sweep;
    }

    // `{ provider, provider_request_id: providerRequestId, dispatch_state: "dispatched" }`
    contracts::PaidFallbackRequestLifecyclePatchV3 dispatchedPatch(::uos::types::PaidFallbackProvider provider, const std::optional<std::string>& providerRequestId) {
      contracts::PaidFallbackRequestLifecyclePatchV3 patch;
      patch.provider = std::move(provider);
      if (providerRequestId) patch.provider_request_id = *providerRequestId;
      else patch.provider_request_id = nullptr;
      patch.dispatch_state = "dispatched";
      return patch;
    }
  }

  PaidFallbackPolicyFields defaultPaidFallbackPolicy() {
    return PaidFallbackPolicyFields{
      .paid_fallback_enabled = false,
      .paid_fallback_limit_microcredits = 0,
      .paid_fallback_spent_microcredits = 0,
      .paid_fallback_reserved_microcredits = 0,
      .paid_fallback_reservation_request_id = std::nullopt,
      .paid_fallback_model_ids = {},
      .paid_fallback_quota_per_credit = 0,
      .paid_fallback_max_exposure_microcredits = json::object(),
      .paid_fallback_pricing_checked_at_ms = std::nullopt,
    };
  }

  rt::Task<InitializePaidFallbackPolicyResult> initializePaidFallbackPolicy() {
    rt::Task<InitializePaidFallbackPolicyResult> initializing = initializePaidFallbackPolicy(std::nullopt);
    InitializePaidFallbackPolicyResult result = co_await std::move(initializing);
    co_return result;
  }

  rt::Task<InitializePaidFallbackPolicyResult> initializePaidFallbackPolicy(std::optional<rt::AbortSignal> signal) {
    if (noPaidProviderKey()) {
      throw provider::metered::MeteredError("Metered paid fallback cannot be enabled because no paid provider API key is configured.", "metered_api_key_missing", 503);
    }

    // The compact runtime catalog intentionally omits context bounds to fit one
    // KV read unit. Paid-admission exposure must use the immutable full catalog.
    const rt::Promise<std::optional<models::codex_models::CodexModelsSnapshot>> loading(codex::index::loadFullCodexModelsSnapshot());
    const std::optional<models::codex_models::CodexModelsSnapshot> snapshot = co_await loading.wait();
    std::vector<std::string> modelIds;
    rt::OrderedMap<std::string, double> contextByModel;
    rt::OrderedSet<std::string> seen;
    const json::array* models = snapshot ? rt::getArray(*snapshot, "models") : nullptr;
    if (models != nullptr) {
      for (const json::value& model : *models) {
        std::optional<std::string> id = utils::getString(rt::get(model, "slug"));
        if (!id) id = utils::getString(rt::get(model, "id"));
        if (!id) id = utils::getString(rt::get(model, "model"));
        if (!id) id = utils::getString(rt::get(model, "name"));
        const std::optional<std::string> normalized = id ? std::optional<std::string>(rt::trim(*id)) : std::nullopt;
        if (!rt::truthy(normalized) || seen.has(*normalized)) continue;
        seen.add(*normalized);
        modelIds.push_back(*normalized);
        std::vector<double> context;
        for (const std::string_view field : {std::string_view("context_window"), std::string_view("max_context_window"), std::string_view("auto_compact_token_limit")}) {
          const std::optional<double> value = rt::getNumber(rt::get(model, field));
          if (value && rt::Number::isSafeInteger(*value) && *value > 0) context.push_back(*value);
        }
        if (!context.empty()) contextByModel.set(*normalized, rt::Math::max(std::span<const double>(context)));
      }
    }
    if (modelIds.empty()) {
      throw provider::metered::MeteredError("Metered paid fallback cannot be enabled before the Codex model catalog is initialized.", "metered_pricing_invalid", 503);
    }

    provider::metered::InitializeMeteredPricingOptions options;
    options.codexModelIds = modelIds;
    options.signal = std::move(signal);
    const rt::Promise<provider::metered::MeteredPricingSnapshot> pricingRead(provider::metered::initializeMeteredPricing(std::move(options)));
    const provider::metered::MeteredPricingSnapshot pricing = co_await pricingRead.wait();
    if (pricing.eligible_model_ids.empty()) {
      throw provider::metered::MeteredError("Metered paid fallback found no priced models in the current Codex catalog.", "metered_pricing_invalid", 503);
    }
    json::object maximumExposure;
    for (const std::string& model : pricing.eligible_model_ids) {
      const double* context = contextByModel.get(model);
      const std::optional<double> exposure =
        maximumExposureForModel(context ? std::optional<double>(*context) : std::nullopt, rt::get(pricing.model_quota_coefficients, model), pricing.quota_per_credit);
      if (!exposure) continue;
      rt::objectSet(maximumExposure, model, rt::makeNumber(*exposure));
    }
    bool missingExposure = false;
    for (const std::string& model : pricing.eligible_model_ids) {
      if (!isPositiveSafeInteger(rt::getNumber(rt::get(maximumExposure, model)))) {
        missingExposure = true;
        break;
      }
    }
    if (missingExposure) {
      throw provider::metered::MeteredError("Metered paid fallback cannot be enabled because a priced Codex model has no finite context bound.", "metered_pricing_invalid",
                                            503);
    }
    co_return InitializePaidFallbackPolicyResult{
      .paid_fallback_model_ids = pricing.eligible_model_ids,
      .paid_fallback_quota_per_credit = pricing.quota_per_credit,
      .paid_fallback_max_exposure_microcredits = std::move(maximumExposure),
      .paid_fallback_pricing_checked_at_ms = pricing.checked_at_ms,
    };
  }

  rt::Task<PaidFallbackReservationDecision> reservePaidFallback(ReservePaidFallbackInput input) {
    const rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    if (!kv) co_return PaidFallbackReservationDecisionBlocked{.reason = "invalid_policy", .reset_at_ms = std::nullopt};
    for (double attempt = 0; attempt < 3; attempt += 1) {
      // This strongly-read check is part of the admission CAS. A just-committed
      // disable or cap reduction therefore prevents new paid exposure even when
      // a request had already read the older policy snapshot.
      const rt::Promise<std::optional<PaidFallbackPolicyEntry>> loading(loadStrictKeyRecord(kv, input.keyId));
      const std::optional<PaidFallbackPolicyEntry> policy = co_await loading.wait();
      if (!policy) co_return PaidFallbackReservationDecisionBlocked{.reason = "invalid_policy", .reset_at_ms = std::nullopt};
      const ApiKeyRecord& record = policy->record;
      const PaidFallbackEligibility eligibility = evaluatePaidFallbackEligibility(record, input.model, input.allowUnrosteredModel == true);
      if (const auto* skip = std::get_if<PaidFallbackReservationDecisionSkip>(&eligibility)) co_return *skip;
      if (const auto* blocked = std::get_if<PaidFallbackReservationDecisionBlocked>(&eligibility)) co_return *blocked;
      if (noPaidProviderKey()) {
        co_return PaidFallbackReservationDecisionSkip{.reason = "provider_unconfigured"};
      }
      const double windowResetAtMs = advanceUsageWindow(record.usage_reset_at_ms, record.window_ms, input.createdAtMs);
      const std::string policyVersion = rt::kvToString(policy->windowMs) + ":" + rt::numberToString(record.paid_fallback_pricing_checked_at_ms.value_or(0));
      ledger_state::AdmissionInput admission;
      admission.keyId = input.keyId;
      admission.requestId = input.requestId;
      admission.createdAtMs = input.createdAtMs;
      admission.model = input.model;
      admission.route = input.route;
      admission.path = input.path;
      admission.stream = input.stream;
      admission.reasoning = input.reasoning;
      admission.policyVersion = policyVersion;
      admission.policyCheck = policy->check;
      admission.limitMicrocredits = record.paid_fallback_limit_microcredits;
      admission.maximumExposureMicrocredits = exposureOf(record, input.model);
      admission.initialSettledMicrocredits = 0;
      admission.quotaPerCredit = record.paid_fallback_quota_per_credit;
      admission.windowResetAtMs = windowResetAtMs;
      // Persist the billable-ambiguity boundary in the admission transaction.
      // Provider fetch must never begin from a row that still looks safely
      // undispatched.
      admission.dispatchIntent = true;
      const rt::Promise<ledger_admission::AdmitPaidFallbackV3Result> admitting(ledger_admission::admitPaidFallbackV3(std::move(admission)));
      const ledger_admission::AdmitPaidFallbackV3Result admitted = co_await admitting.wait();
      if (const auto* reserved = std::get_if<ledger_admission::AdmitPaidFallbackV3ResultReserved>(&admitted)) {
        co_return PaidFallbackReservationDecisionReserved{.reservation = reserved->reservation};
      }
      const std::string reason = std::get<ledger_admission::AdmitPaidFallbackV3ResultBlocked>(admitted).reason;
      const rt::Promise<bool> retrying(retryBlockedPaidFallbackAdmission(reason, attempt));
      const bool retry = co_await retrying.wait();
      if (retry) continue;
      co_return PaidFallbackReservationDecisionBlocked{.reason = reason, .reset_at_ms = windowResetAtMs};
    }
    co_return PaidFallbackReservationDecisionBlocked{.reason = "concurrent_update", .reset_at_ms = std::nullopt};
  }

  rt::Task<void> recordMeteredUpstreamResponse(PaidFallbackReservation reservation, rt::Response _response, std::optional<std::string> providerRequestId) {
    rt::Task<void> recording = recordMeteredUpstreamResponse(std::move(reservation), std::move(_response), std::move(providerRequestId), std::string("metered"));
    co_await std::move(recording);
  }

  rt::Task<void> recordMeteredUpstreamResponse(PaidFallbackReservation reservation, rt::Response, std::optional<std::string> providerRequestId,
                                               ::uos::types::PaidFallbackProvider provider) {
    const rt::Promise<void> updating(ledger_admission::updatePaidFallbackRequestV3(reservation, dispatchedPatch(std::move(provider), providerRequestId)));
    co_await updating.wait();
  }

  void setPaidFallbackTerminalSweepForTest(std::function<void()> sweep) {
    paidFallbackTerminalSweep() = sweep ? std::move(sweep) : std::function<void()>(defaultPaidFallbackTerminalSweep);
  }

  rt::Task<void> recordMeteredAmbiguousFailure(PaidFallbackReservation reservation) {
    rt::Task<void> recording = recordMeteredAmbiguousFailure(std::move(reservation), std::string("metered"), std::nullopt);
    co_await std::move(recording);
  }

  rt::Task<void> recordMeteredAmbiguousFailure(PaidFallbackReservation reservation, ::uos::types::PaidFallbackProvider provider) {
    rt::Task<void> recording = recordMeteredAmbiguousFailure(std::move(reservation), std::move(provider), std::nullopt);
    co_await std::move(recording);
  }

  rt::Task<void> recordMeteredAmbiguousFailure(PaidFallbackReservation reservation, ::uos::types::PaidFallbackProvider provider,
                                               std::optional<std::string> providerRequestId) {
    const rt::Promise<void> updating(ledger_admission::updatePaidFallbackRequestV3(reservation, dispatchedPatch(std::move(provider), providerRequestId)));
    co_await updating.wait();
    const rt::Promise<double> marking(ledger_backfill::markPaidFallbackTerminalV3(reservation, std::string("ambiguous")));
    co_await marking.wait();
    // A terminal event makes pending marks due and starts one best-effort sweep.
    // Fire-and-forget so provider-log reads never delay the response; deferred
    // work remains durable for a later event instead of using a periodic timer.
    paidFallbackTerminalSweep()();
  }

  rt::Task<void> recordMeteredUndispatchedCancellation(PaidFallbackReservation reservation) {
    const rt::Promise<void> releasing(ledger_admission::releasePaidFallbackBeforeProviderFetchV3(std::move(reservation)));
    co_await releasing.wait();
  }

  rt::Task<void> recordMeteredPrefetchCancellation(PaidFallbackReservation reservation) {
    const rt::Promise<void> releasing(ledger_admission::releasePaidFallbackBeforeProviderFetchV3(std::move(reservation)));
    co_await releasing.wait();
  }

  rt::Task<void> recordMeteredTerminal(PaidFallbackReservation reservation, std::string terminalState) {
    rt::Task<void> recording = recordMeteredTerminal(std::move(reservation), std::move(terminalState), std::string("metered"));
    co_await std::move(recording);
  }

  rt::Task<void> recordMeteredTerminal(PaidFallbackReservation reservation, std::string terminalState, ::uos::types::PaidFallbackProvider provider) {
    const rt::Promise<double> marking(ledger_backfill::markPaidFallbackTerminalV3(std::move(reservation), std::move(terminalState), std::move(provider)));
    co_await marking.wait();
    // Same event-driven settlement as the ambiguous path: the request is over, so
    // make one non-blocking attempt at what its terminal state made due. Deferred
    // provider-log work remains durable and waits for another authorized event.
    paidFallbackTerminalSweep()();
  }

  rt::Task<void> recordSurplusUsage(PaidFallbackReservation reservation, std::string settlementRequestId, std::string model, SurplusUsage usage,
                                    SurplusBillingPricing pricing) {
    if (rt::trim(settlementRequestId).empty() || rt::trim(model).empty() || !isNonNegativeSafeInteger(usage.input_tokens) || !isNonNegativeSafeInteger(usage.output_tokens)) {
      co_return;
    }
    // Absent cache counters settle as zero; non-null counts keep their exact value.
    const double cachedInputTokens = usage.cached_input_tokens.value_or(0);
    const double cacheWriteInputTokens = usage.cache_write_input_tokens.value_or(0);
    if (!isNonNegativeSafeInteger(cachedInputTokens) || cachedInputTokens > *usage.input_tokens || !isNonNegativeSafeInteger(cacheWriteInputTokens) ||
        cacheWriteInputTokens > *usage.input_tokens) {
      co_return;
    }
    const double uncachedInputTokens = *usage.input_tokens - cachedInputTokens;
    std::optional<double> buyerCostMicro;
    double chargedCredits = 0;
    if (usage.buyer_cost_micro != rt::undefined) {
      // A present malformed charge cannot become a catalog-price estimate.
      if (!isNonNegativeSafeInteger(usage.buyer_cost_micro.toOptional())) co_return;
      buyerCostMicro = *usage.buyer_cost_micro;
      chargedCredits = *buyerCostMicro / MICROCREDITS_PER_CREDIT;
    } else {
      if (pricing.require_provider_cost == true) co_return;
      if (!isNonNegativeFiniteNumber(pricing.input_price_per_token) || !isNonNegativeFiniteNumber(pricing.output_price_per_token)) co_return;
      const double cacheReadPrice = pricing.cache_read_price_per_token.value_or(pricing.input_price_per_token);
      const double cacheWritePrice = pricing.cache_write_price_per_token.value_or(pricing.input_price_per_token);
      if (!isNonNegativeFiniteNumber(cacheReadPrice) || !isNonNegativeFiniteNumber(cacheWritePrice)) co_return;
      chargedCredits = uncachedInputTokens * pricing.input_price_per_token + cachedInputTokens * cacheReadPrice + cacheWriteInputTokens * cacheWritePrice +
                       *usage.output_tokens * pricing.output_price_per_token;
    }
    const double providerQuota = rt::Math::round(chargedCredits * reservation.quota_per_credit);
    if (!rt::Number::isSafeInteger(providerQuota) || providerQuota < 0) co_return;
    ledger_settlement::PaidFallbackUsageSettlementV3 settlement{
      .settlement_request_id = settlementRequestId,
      .provider_quota = providerQuota,
      .input_tokens = *usage.input_tokens,
      .cached_input_tokens = {},
      .output_tokens = *usage.output_tokens,
      .model = model,
      .created_at_ms = rt::dateNow(),
      .buyer_cost_micro = buyerCostMicro,
    };
    if (!buyerCostMicro) settlement.cached_input_tokens = cachedInputTokens;
    else if (usage.cached_input_tokens) settlement.cached_input_tokens = *usage.cached_input_tokens;
    else settlement.cached_input_tokens = nullptr;
    const rt::Promise<bool> settling(ledger_settlement::settlePaidFallbackUsageV3(reservation, std::move(settlement)));
    co_await settling.wait();
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackPolicyFields& value) {
    out = Writer()
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

  PaidFallbackPolicyFields tag_invoke(const json::value_to_tag<PaidFallbackPolicyFields>&, const json::value& value) {
    const Reader in(value, "PaidFallbackPolicyFields");
    return {
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

  void tag_invoke(const json::value_from_tag&, json::value& out, const InitializePaidFallbackPolicyResult& value) {
    out = Writer()
            .required("paid_fallback_model_ids", value.paid_fallback_model_ids)
            .required("paid_fallback_quota_per_credit", value.paid_fallback_quota_per_credit)
            .orUndefined("paid_fallback_max_exposure_microcredits", value.paid_fallback_max_exposure_microcredits)
            .orNull("paid_fallback_pricing_checked_at_ms", value.paid_fallback_pricing_checked_at_ms)
            .done();
  }

  InitializePaidFallbackPolicyResult tag_invoke(const json::value_to_tag<InitializePaidFallbackPolicyResult>&, const json::value& value) {
    const Reader in(value, "InitializePaidFallbackPolicyResult");
    return {
      .paid_fallback_model_ids = in.required<std::vector<std::string>>("paid_fallback_model_ids"),
      .paid_fallback_quota_per_credit = in.required<double>("paid_fallback_quota_per_credit"),
      .paid_fallback_max_exposure_microcredits = in.orUndefined<json::object>("paid_fallback_max_exposure_microcredits"),
      .paid_fallback_pricing_checked_at_ms = in.orNull<double>("paid_fallback_pricing_checked_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SurplusBillingPricing& value) {
    out = Writer()
            .required("input_price_per_token", value.input_price_per_token)
            .required("output_price_per_token", value.output_price_per_token)
            .orUndefined("cache_read_price_per_token", value.cache_read_price_per_token)
            .orUndefined("cache_write_price_per_token", value.cache_write_price_per_token)
            .orUndefined("require_provider_cost", value.require_provider_cost)
            .done();
  }

  SurplusBillingPricing tag_invoke(const json::value_to_tag<SurplusBillingPricing>&, const json::value& value) {
    const Reader in(value, "SurplusBillingPricing");
    return {
      .input_price_per_token = in.required<double>("input_price_per_token"),
      .output_price_per_token = in.required<double>("output_price_per_token"),
      .cache_read_price_per_token = in.orUndefined<double>("cache_read_price_per_token"),
      .cache_write_price_per_token = in.orUndefined<double>("cache_write_price_per_token"),
      .require_provider_cost = in.orUndefined<bool>("require_provider_cost"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SurplusUsage& value) {
    out = Writer()
            .orNull("input_tokens", value.input_tokens)
            .orNull("cached_input_tokens", value.cached_input_tokens)
            .orNull("cache_write_input_tokens", value.cache_write_input_tokens)
            .orNull("output_tokens", value.output_tokens)
            .nullable("buyer_cost_micro", value.buyer_cost_micro)
            .done();
  }

  SurplusUsage tag_invoke(const json::value_to_tag<SurplusUsage>&, const json::value& value) {
    const Reader in(value, "SurplusUsage");
    return {
      .input_tokens = in.orNull<double>("input_tokens"),
      .cached_input_tokens = in.orNull<double>("cached_input_tokens"),
      .cache_write_input_tokens = in.orNull<double>("cache_write_input_tokens"),
      .output_tokens = in.orNull<double>("output_tokens"),
      .buyer_cost_micro = in.nullable<double>("buyer_cost_micro"),
    };
  }
}
