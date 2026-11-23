#include "uos/metered_quota.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace uos::metered_quota {
  namespace {
    constexpr rt::StoredJsonViewOptions STORED_JSON_VIEW_OPTIONS{
      .undefinedMember = rt::StoredJsonViewOptions::UndefinedView::omit,
      .undefinedElement = rt::StoredJsonViewOptions::UndefinedView::null,
      .hole = rt::StoredJsonViewOptions::UndefinedView::emptyObject,
      .topLevelUndefined = rt::StoredJsonViewOptions::UndefinedView::null,
      .bigint = rt::StoredJsonViewOptions::BigIntView::emptyObject,
      .kvU64Members = false,
      .typedArrayMembers = false,
    };
    using rt::record::Reader;
    using rt::record::Writer;

    // `${METERED_BASE_URL}/api/usage/token/`
    constexpr std::string_view METERED_TOKEN_USAGE_URL = "https://api.openlux.ai/api/usage/token/";

    // `Readonly<{ owner: string; lease_until_ms: number }>`
    struct RefreshLease {
      std::string owner;
      double lease_until_ms;
    };

    // `Readonly<{ invalidated_at_ms: number }>`
    struct QuotaInvalidation {
      double invalidated_at_ms;
    };

    bool isSafeInteger(const json::value* value) { return value && rt::Number::isSafeInteger(*value); }
    bool isNonNegativeSafeInteger(const json::value* value) { return isSafeInteger(value) && rt::toNumber(*value) >= 0; }
    bool isNonNegativeSafeInteger(double value) { return rt::Number::isSafeInteger(value) && value >= 0; }
    bool isPositiveSafeInteger(const json::value* value) { return isSafeInteger(value) && rt::toNumber(*value) > 0; }
    bool isNonNegativeFiniteNumber(const json::value* value) {
      return value && rt::isNumber(*value) && rt::Number::isFinite(*value) && rt::toNumber(*value) >= 0;
    }
    bool isConfidence(const json::value* value) {
      if (!value || !value->is_string()) return false;
      const std::string_view text(value->get_string());
      return text == "provisional" || text == "refill_observed" || text == "inferred_adjustment";
    }
    bool isQuotaInvalidation(const json::value* value) { return utils::isRecord(value) && isNonNegativeSafeInteger(rt::get(*value, "invalidated_at_ms")); }

    bool isNull(const json::value* value) { return value && value->is_null(); }
    bool isBoolean(const json::value* value) { return value && value->is_bool(); }

    // A stored value as the JSON guards read it: undefined members are absent and every
    // value JSON cannot hold (bigint, Map, Date, ...) reads as an empty object, which no
    // guard of this module accepts where the TS would reject the original.

    // ToNumber(ToPrimitive(value)) for the relational comparisons the TS makes on unchecked
    // stored values (`entry.value.lease_until_ms > nowMs`); NaN makes every comparison false.
    double relationalNumber(const rt::KvValue& value) {
      if (value.isNumber()) return value.asNumber();
      if (value.isBool()) return value.asBool() ? 1 : 0;
      if (value.isNull()) return 0;
      if (value.isString()) return rt::stringToNumber(value.asString());
      if (value.isBigInt()) return value.asBigInt().toNumber();
      if (value.is<rt::KvDate>()) return value.as<rt::KvDate>().time;
      if (value.isArray()) return rt::toNumber(rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS));
      return std::numeric_limits<double>::quiet_NaN();
    }

    // `stored?.owner === owner`
    bool ownerEquals(const rt::KvValue& stored, const std::string& owner) {
      const rt::KvValue& member = stored["owner"];
      return member.isString() && member.asString() == owner;
    }

    rt::console::Value consoleValue(const rt::Reason& reason) {
      if (!reason || rt::isError(reason)) return rt::console::Value(reason);
      const std::optional<json::value> value = rt::thrownValue(reason);
      return value ? rt::console::Value(*value) : rt::console::Value();
    }

    std::optional<MeteredAccountCredentials> parseCredentials(const MeteredAccountCredentials& credentials) {
      static const rt::Regex WHITESPACE("\\s");
      std::string apiKey = rt::trim(credentials.apiKey);
      if (apiKey.empty() || WHITESPACE.test(apiKey)) return std::nullopt;
      return MeteredAccountCredentials{.apiKey = std::move(apiKey)};
    }

    std::optional<std::string> getEnv(std::string_view key) {
      try {
        return rt::env::get(key);
      } catch (...) {
        return std::nullopt;
      }
    }

    rt::Headers authenticatedHeaders(const MeteredAccountCredentials& credentials) {
      rt::Headers headers{{"Accept", "application/json"}, {"Authorization", "Bearer " + credentials.apiKey}};
      return headers;
    }

    const json::value* successfulEnvelopeData(const json::value& value) {
      const json::value* success = rt::get(value, "success");
      const json::value* data = rt::get(value, "data");
      if (!utils::isRecord(&value) || !(isBoolean(success) && success->get_bool()) || !utils::isRecord(data)) return nullptr;
      return data;
    }

    rt::Task<json::value> fetchJson(std::string url, provider::metered::MeteredFetch fetcher, rt::Headers headers, rt::AbortSignal signal) {
      rt::RequestInit init;
      init.method = "GET";
      init.headers = std::move(headers);
      init.redirect = "manual";
      init.signal = signal;
      const rt::Promise<rt::Response> fetching(fetcher(url, std::move(init)));
      const rt::Response response = co_await fetching.wait();
      if (!response.ok()) throw rt::JsError("Metered account API returned HTTP " + rt::numberToString(response.status()));
      const std::string contentType = rt::toLowerCase(response.headers().get("Content-Type").value_or(""));
      if (!rt::includes(contentType, "application/json")) throw rt::JsError("Metered account API returned non-JSON data");
      std::optional<json::value> payload;
      try {
        const rt::Promise<json::value> reading(response.json());
        payload = co_await reading.wait();
      } catch (...) {
      }
      if (!payload) throw rt::JsError("Metered account API returned invalid JSON");
      co_return std::move(*payload);
    }

    rt::AbortSignal combinedSignal(const std::optional<rt::AbortSignal>& signal) {
      const rt::AbortSignal timeout = rt::AbortSignal::timeout(METERED_QUOTA_FETCH_TIMEOUT_MS);
      return signal ? rt::AbortSignal::any({*signal, timeout}) : timeout;
    }

    bool hasTokenUsageObservation(const MeteredQuotaObservation& observation) {
      return observation.unlimited_quota.has_value() || observation.total_available.has_value() || observation.total_granted.has_value() ||
             observation.total_used.has_value();
    }

    bool isSafeIntegerNumber(const std::optional<double>& value) { return value && rt::Number::isSafeInteger(*value); }

    MeteredQuotaState tokenUsageQuotaState(const MeteredQuotaObservation& observation) {
      if (!observation.unlimited_quota || !isSafeIntegerNumber(observation.total_available) || !isSafeIntegerNumber(observation.total_granted) ||
          !isSafeIntegerNumber(observation.total_used))
        throw rt::JsError("Metered token usage observation is incomplete");
      return MeteredQuotaState{
        // Token usage is not a wallet refill cycle. Keep the legacy numeric
        // fields neutral so signed totals cannot become spendable credits.
        .current_balance_quota = 0,
        .post_refill_baseline_quota = 0,
        .last_observed_used_quota = 0,
        .quota_per_credit = 1,
        .observed_at_ms = observation.observed_at_ms,
        .cycle_started_at_ms = observation.observed_at_ms,
        .confidence = "provisional",
        .last_known_debits_quota = 0,
        .last_inferred_credit_quota = 0,
        .last_credit_at_ms = std::nullopt,
        .latest_refill_id = std::nullopt,
        .latest_refill_amount_credits = std::nullopt,
        .latest_refill_completed_at_ms = std::nullopt,
        .unlimited_quota = observation.unlimited_quota,
        .total_available = *observation.total_available,
        .total_granted = *observation.total_granted,
        .total_used = *observation.total_used,
      };
    }

    /** A wallet observation whose nullable credit fields have been proven present. */
    struct MeteredWalletObservation {
      double balance_quota;
      double used_quota;
      double quota_per_credit;
      double observed_at_ms;
      std::optional<MeteredRefillObservation> latest_refill;
    };

    MeteredWalletObservation meteredWalletObservation(const MeteredQuotaObservation& observation) {
      if (!observation.balance_quota || !observation.used_quota || !observation.quota_per_credit)
        throw rt::JsError("Metered wallet observation is incomplete");
      return MeteredWalletObservation{
        .balance_quota = *observation.balance_quota,
        .used_quota = *observation.used_quota,
        .quota_per_credit = *observation.quota_per_credit,
        .observed_at_ms = observation.observed_at_ms,
        .latest_refill = observation.latest_refill,
      };
    }

    MeteredQuotaState initialWalletQuotaState(const MeteredWalletObservation& wallet) {
      const double refillBaselineQuota = wallet.latest_refill ? rt::Math::round(wallet.latest_refill->amount_credits * wallet.quota_per_credit) : 0;
      const double postRefillBaselineQuota = rt::Number::isSafeInteger(refillBaselineQuota) && refillBaselineQuota > 0
                                               ? rt::Math::max(wallet.balance_quota, refillBaselineQuota)
                                               : wallet.balance_quota;
      return MeteredQuotaState{
        .current_balance_quota = wallet.balance_quota,
        .post_refill_baseline_quota = postRefillBaselineQuota,
        .last_observed_used_quota = wallet.used_quota,
        .quota_per_credit = wallet.quota_per_credit,
        .observed_at_ms = wallet.observed_at_ms,
        .cycle_started_at_ms = wallet.latest_refill ? wallet.latest_refill->completed_at_ms : wallet.observed_at_ms,
        .confidence = "provisional",
        .last_known_debits_quota = 0,
        .last_inferred_credit_quota = 0,
        .last_credit_at_ms = std::nullopt,
        .latest_refill_id = wallet.latest_refill ? std::optional<std::string>(wallet.latest_refill->id) : std::nullopt,
        .latest_refill_amount_credits = wallet.latest_refill ? std::optional<double>(wallet.latest_refill->amount_credits) : std::nullopt,
        .latest_refill_completed_at_ms = wallet.latest_refill ? std::optional<double>(wallet.latest_refill->completed_at_ms) : std::nullopt,
      };
    }

    MeteredQuotaState advancedWalletQuotaState(const MeteredQuotaState& previous, const MeteredWalletObservation& wallet) {
      const bool usedCounterAdvanced = wallet.used_quota >= previous.last_observed_used_quota;
      const double knownDebits = usedCounterAdvanced ? wallet.used_quota - previous.last_observed_used_quota : 0;
      const double expectedBalance = previous.current_balance_quota - knownDebits;
      const double inferredCredit = usedCounterAdvanced ? rt::Math::max(0.0, wallet.balance_quota - expectedBalance)
                                                        : rt::Math::max(0.0, wallet.balance_quota - previous.current_balance_quota);
      const bool newRefillObserved = wallet.latest_refill && wallet.latest_refill->id != previous.latest_refill_id;
      const bool creditObserved = inferredCredit > 0 || newRefillObserved;
      const double refillBaselineQuota =
        newRefillObserved && wallet.latest_refill ? rt::Math::round(wallet.latest_refill->amount_credits * wallet.quota_per_credit) : 0;
      const double balancePlusKnownDebits = wallet.balance_quota + knownDebits;
      const double reconstructedRefillCapacityQuota =
        newRefillObserved && knownDebits > 0 && rt::Number::isSafeInteger(balancePlusKnownDebits) ? balancePlusKnownDebits : wallet.balance_quota;
      const double refillBaselineFloor = rt::Number::isSafeInteger(refillBaselineQuota) && refillBaselineQuota > 0 ? refillBaselineQuota : 0;
      const double postRefillBaselineQuota =
        creditObserved ? rt::Math::max(reconstructedRefillCapacityQuota, refillBaselineFloor) : previous.post_refill_baseline_quota;
      const MeteredQuotaConfidence observedConfidence = newRefillObserved ? "refill_observed" : "inferred_adjustment";
      return MeteredQuotaState{
        .current_balance_quota = wallet.balance_quota,
        .post_refill_baseline_quota = postRefillBaselineQuota,
        .last_observed_used_quota = wallet.used_quota,
        .quota_per_credit = wallet.quota_per_credit,
        .observed_at_ms = wallet.observed_at_ms,
        .cycle_started_at_ms =
          creditObserved ? (wallet.latest_refill ? wallet.latest_refill->completed_at_ms : wallet.observed_at_ms) : previous.cycle_started_at_ms,
        .confidence = creditObserved ? observedConfidence : previous.confidence,
        .last_known_debits_quota = knownDebits,
        .last_inferred_credit_quota = creditObserved ? inferredCredit : 0,
        .last_credit_at_ms = creditObserved ? std::optional<double>(wallet.observed_at_ms) : previous.last_credit_at_ms,
        .latest_refill_id = wallet.latest_refill ? std::optional<std::string>(wallet.latest_refill->id) : previous.latest_refill_id,
        .latest_refill_amount_credits =
          wallet.latest_refill ? std::optional<double>(wallet.latest_refill->amount_credits) : previous.latest_refill_amount_credits,
        .latest_refill_completed_at_ms =
          wallet.latest_refill ? std::optional<double>(wallet.latest_refill->completed_at_ms) : previous.latest_refill_completed_at_ms,
      };
    }

    /** Remaining percentage of the post-refill baseline, null when unusable. */
    std::optional<double> remainingPercentOfBaseline(double currentBalanceQuota, double postRefillBaselineQuota) {
      if (postRefillBaselineQuota <= 0) return std::nullopt;
      return rt::Math::min(100.0, rt::Math::max(0.0, (currentBalanceQuota / postRefillBaselineQuota) * 100));
    }

    bool tokenUsageState(const MeteredQuotaState& state) {
      return state.unlimited_quota.has_value() || !state.total_available.isUndefined() || !state.total_granted.isUndefined() ||
             !state.total_used.isUndefined();
    }

    MeteredQuotaSnapshot toSnapshot(const MeteredQuotaState& state, const MeteredQuotaCacheState& cacheState) {
      const bool tokenUsage = tokenUsageState(state);
      const std::optional<double> balanceCredits = tokenUsage ? std::nullopt : std::optional<double>(state.current_balance_quota / state.quota_per_credit);
      const std::optional<double> baselineCredits =
        tokenUsage ? std::nullopt : std::optional<double>(state.post_refill_baseline_quota / state.quota_per_credit);
      const std::optional<double> remainingPercent =
        tokenUsage ? std::nullopt : remainingPercentOfBaseline(state.current_balance_quota, state.post_refill_baseline_quota);
      return MeteredQuotaSnapshot{
        .state = state,
        .cache_state = cacheState,
        .balance_credits = balanceCredits,
        .baseline_credits = baselineCredits,
        .last_inferred_credit_credits = tokenUsage ? std::nullopt : std::optional<double>(state.last_inferred_credit_quota / state.quota_per_credit),
        .remaining_percent = remainingPercent,
        .used_percent = remainingPercent ? std::optional<double>(100 - *remainingPercent) : std::nullopt,
        .unlimited_quota = tokenUsage ? state.unlimited_quota == true : false,
        .total_available = tokenUsage && state.total_available.hasValue() ? std::optional<double>(*state.total_available) : std::nullopt,
        .total_granted = tokenUsage && state.total_granted.hasValue() ? std::optional<double>(*state.total_granted) : std::nullopt,
        .total_used = tokenUsage && state.total_used.hasValue() ? std::optional<double>(*state.total_used) : std::nullopt,
      };
    }

    // `isMeteredQuotaState(value) ? value : null` over a stored value.
    std::optional<MeteredQuotaState> quotaStateOf(const rt::KvValue& value) {
      const json::value view = rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS);
      if (!isMeteredQuotaState(&view)) return std::nullopt;
      return json::value_to<MeteredQuotaState>(view);
    }

    rt::Task<std::optional<MeteredQuotaState>> loadRetainedState(std::shared_ptr<rt::Kv> kv, double nowMs) {
      const rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(METERED_QUOTA_STATE_KEY));
      const rt::KvEntryMaybe<> entry = co_await reading.wait();
      std::optional<MeteredQuotaState> state = quotaStateOf(entry.value);
      if (!state) co_return std::nullopt;
      if (nowMs - state->observed_at_ms >= METERED_QUOTA_RETENTION_MS) co_return std::nullopt;
      co_return state;
    }

    // `Promise.resolve(kv.get(METERED_QUOTA_INVALIDATION_KEY))`
    rt::Promise<rt::KvEntryMaybe<>> loadInvalidation(const std::shared_ptr<rt::Kv>& kv) {
      return rt::Promise<rt::KvEntryMaybe<>>(kv->get(METERED_QUOTA_INVALIDATION_KEY));
    }

    std::optional<QuotaInvalidation> quotaInvalidationValue(const rt::KvValue& value) {
      const json::value view = rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS);
      if (!isQuotaInvalidation(&view)) return std::nullopt;
      return QuotaInvalidation{.invalidated_at_ms = rt::toNumber(*rt::get(view, "invalidated_at_ms"))};
    }

    bool isInvalidated(const MeteredQuotaState& state, const std::optional<QuotaInvalidation>& invalidation) {
      return invalidation && invalidation->invalidated_at_ms >= state.observed_at_ms;
    }

    rt::Task<bool> acquireRefreshLease(std::shared_ptr<rt::Kv> kv, std::string owner, double nowMs) {
      const rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(METERED_QUOTA_REFRESH_LEASE_KEY));
      const rt::KvEntryMaybe<> entry = co_await reading.wait();
      if (entry.value.truthy() && relationalNumber(entry.value["lease_until_ms"]) > nowMs) co_return false;
      const RefreshLease lease{.owner = owner, .lease_until_ms = nowMs + METERED_QUOTA_REFRESH_LEASE_MS};
      json::object stored;
      stored["owner"] = lease.owner;
      stored["lease_until_ms"] = rt::makeNumber(lease.lease_until_ms);
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(entry).set(METERED_QUOTA_REFRESH_LEASE_KEY, rt::KvValue(stored), rt::KvSetOptions{.expireIn = METERED_QUOTA_REFRESH_LEASE_MS * 2});
      const rt::Promise<rt::KvCommitResult> committing(operation->commit());
      const rt::KvCommitResult committed = co_await committing.wait();
      co_return committed.ok;
    }

    rt::Task<void> releaseRefreshLease(std::shared_ptr<rt::Kv> kv, std::string owner) {
      rt::Reason failure;
      try {
        const rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(METERED_QUOTA_REFRESH_LEASE_KEY));
        const rt::KvEntryMaybe<> entry = co_await reading.wait();
        if (ownerEquals(entry.value, owner)) {
          const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
          operation->check(entry).delete_(METERED_QUOTA_REFRESH_LEASE_KEY);
          const rt::Promise<rt::KvCommitResult> committing(operation->commit());
          co_await committing.wait();
        }
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) rt::console::warn("[ai.ubq.fi] Metered quota refresh lease release failed:", consoleValue(failure));
    }

    rt::Task<std::optional<MeteredQuotaState>> waitForColdState(std::shared_ptr<rt::Kv> kv) {
      const double deadline = rt::dateNow() + METERED_QUOTA_COLD_WAIT_MS;
      while (rt::dateNow() < deadline) {
        co_await rt::sleep(50);
        const rt::Promise<std::optional<MeteredQuotaState>> loading(loadRetainedState(kv, rt::dateNow()));
        const std::optional<MeteredQuotaState> entry = co_await loading.wait();
        if (entry) co_return entry;
      }
      co_return std::nullopt;
    }

    /** Stale snapshot from the newest retained state, falling back to the cache. */
    std::optional<MeteredQuotaSnapshot> staleSnapshotOrNull(const std::optional<MeteredQuotaState>& replacement,
                                                            const std::optional<MeteredQuotaState>& cached) {
      if (replacement) return toSnapshot(*replacement, "stale");
      if (cached) return toSnapshot(*cached, "stale");
      return std::nullopt;
    }

    rt::Task<std::optional<MeteredQuotaSnapshot>> replacementStaleSnapshot(std::shared_ptr<rt::Kv> kv, std::optional<MeteredQuotaState> cached) {
      const rt::Promise<std::optional<MeteredQuotaState>> loading =
        rt::Promise<std::optional<MeteredQuotaState>>(loadRetainedState(kv, rt::dateNow())).catch_([](const rt::Reason&) -> std::optional<MeteredQuotaState> {
          return std::nullopt;
        });
      const std::optional<MeteredQuotaState> replacement = co_await loading.wait();
      co_return staleSnapshotOrNull(replacement, cached);
    }

    // `Readonly<{ cached: MeteredQuotaState | null; cachedInvalidated: boolean }>`
    struct CachedQuotaState {
      std::optional<MeteredQuotaState> cached;
      bool cachedInvalidated;
    };

    rt::Task<CachedQuotaState> readCachedQuotaState(std::shared_ptr<rt::Kv> kv, double nowMs) {
      const rt::Promise<std::optional<MeteredQuotaState>> cachedEntry =
        rt::Promise<std::optional<MeteredQuotaState>>(loadRetainedState(kv, nowMs)).catch_([](const rt::Reason& error) -> std::optional<MeteredQuotaState> {
          rt::console::warn("[ai.ubq.fi] Metered quota cache read failed:", consoleValue(error));
          return std::nullopt;
        });
      // `.catch(() => null)`: a null entry reads as an absent invalidation, as a null value does.
      const rt::Promise<rt::KvEntryMaybe<>> cachedInvalidationEntry = loadInvalidation(kv).catch_([](const rt::Reason& error) -> rt::KvEntryMaybe<> {
        rt::console::warn("[ai.ubq.fi] Metered quota invalidation read failed:", consoleValue(error));
        return rt::KvEntryMaybe<>{};
      });
      const auto both = rt::all(cachedEntry, cachedInvalidationEntry);
      const std::tuple<std::optional<MeteredQuotaState>, rt::KvEntryMaybe<>> settled = co_await both.wait();
      const std::optional<MeteredQuotaState>& cached = std::get<0>(settled);
      CachedQuotaState result{
        .cached = cached,
        .cachedInvalidated = cached ? isInvalidated(*cached, quotaInvalidationValue(std::get<1>(settled).value)) : false,
      };
      co_return result;
    }

    /**
     * Refresh under a held lease. Returns null when the lease was lost, which the
     * caller reports through the same stale snapshot used for a failed commit.
     */
    rt::Task<std::optional<MeteredQuotaSnapshot>> refreshMeteredQuotaSnapshot(std::shared_ptr<rt::Kv> kv, MeteredAccountCredentials credentials,
                                                                             GetMeteredQuotaSnapshotOptions options, std::optional<MeteredQuotaState> cached,
                                                                             std::string owner, std::function<double()> now) {
      const rt::Promise<rt::KvEntryMaybe<>> stateReading(kv->get(METERED_QUOTA_STATE_KEY));
      const rt::Promise<rt::KvEntryMaybe<>> invalidationReading = loadInvalidation(kv);
      const auto both = rt::all(stateReading, invalidationReading);
      const std::tuple<rt::KvEntryMaybe<>, rt::KvEntryMaybe<>> entries = co_await both.wait();
      const rt::KvEntryMaybe<>& stateEntry = std::get<0>(entries);
      const rt::KvEntryMaybe<>& refreshInvalidationEntry = std::get<1>(entries);
      FetchMeteredQuotaObservationOptions observationOptions;
      observationOptions.fetcher = options.fetcher;
      observationOptions.now = now;
      observationOptions.signal = options.signal;
      const rt::Promise<MeteredQuotaObservation> observing(fetchMeteredQuotaObservation(credentials, observationOptions));
      const MeteredQuotaObservation observation = co_await observing.wait();
      const rt::Promise<rt::KvEntryMaybe<>> leaseReading(kv->get(METERED_QUOTA_REFRESH_LEASE_KEY));
      const rt::KvEntryMaybe<> leaseEntry = co_await leaseReading.wait();
      // `leaseEntry.value?.owner !== owner || leaseEntry.value.lease_until_ms <= observation.observed_at_ms`
      const double leaseUntilMs = relationalNumber(leaseEntry.value["lease_until_ms"]);
      if (!ownerEquals(leaseEntry.value, owner) || leaseUntilMs <= observation.observed_at_ms) {
        const rt::Promise<std::optional<MeteredQuotaSnapshot>> replacing(replacementStaleSnapshot(kv, cached));
        const std::optional<MeteredQuotaSnapshot> replaced = co_await replacing.wait();
        co_return replaced;
      }
      const std::optional<MeteredQuotaState> previous = quotaStateOf(stateEntry.value);
      const MeteredQuotaState state = updateMeteredQuotaState(previous, observation);
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(stateEntry)
        .check(refreshInvalidationEntry)
        .check(leaseEntry)
        .set(METERED_QUOTA_STATE_KEY, rt::KvValue(json::value_from(state)), rt::KvSetOptions{.expireIn = METERED_QUOTA_RETENTION_MS})
        .delete_(METERED_QUOTA_INVALIDATION_KEY)
        .delete_(METERED_QUOTA_REFRESH_LEASE_KEY);
      const rt::Promise<rt::KvCommitResult> committing(operation->commit());
      const rt::KvCommitResult committed = co_await committing.wait();
      if (committed.ok) {
        const rt::Promise<std::optional<std::string>> fingerprinting =
          rt::Promise<std::optional<std::string>>(meterQuotaAccountFingerprint(credentials)).catch_([](const rt::Reason&) -> std::optional<std::string> {
            return std::nullopt;
          });
        const std::optional<std::string> accountFingerprint = co_await fingerprinting.wait();
        const rt::Promise<void> writing =
          rt::Promise<void>(writeMeteredQuotaBalanceSample(kv, state, observation.observed_at_ms, accountFingerprint)).catch_([](const rt::Reason& error) {
            rt::console::warn("[ai.ubq.fi] Metered quota balance history write failed:", consoleValue(error));
          });
        co_await writing.wait();
        co_return toSnapshot(state, "refreshed");
      }
      const rt::Promise<std::optional<MeteredQuotaSnapshot>> replacing(replacementStaleSnapshot(kv, cached));
      const std::optional<MeteredQuotaSnapshot> replaced = co_await replacing.wait();
      co_return replaced;
    }

    /** Without the lease we serve whatever retained state appears, or nothing. */
    rt::Task<std::optional<MeteredQuotaSnapshot>> coldWaitSnapshot(std::shared_ptr<rt::Kv> kv, std::optional<MeteredQuotaState> cached) {
      if (cached) co_return toSnapshot(*cached, "stale");
      const rt::Promise<std::optional<MeteredQuotaState>> waiting =
        rt::Promise<std::optional<MeteredQuotaState>>(waitForColdState(kv)).catch_([](const rt::Reason&) -> std::optional<MeteredQuotaState> {
          return std::nullopt;
        });
      const std::optional<MeteredQuotaState> waited = co_await waiting.wait();
      if (!waited) co_return std::nullopt;
      co_return toSnapshot(*waited, "wait");
    }

    bool isMeteredQuotaBalanceSample(const json::value* value) {
      if (!utils::isRecord(value)) return false;
      const json::value* v = rt::get(*value, "v");
      const json::value* remainingPercent = rt::get(*value, "remaining_percent");
      const json::value* unlimitedQuota = rt::get(*value, "unlimited_quota");
      const json::value* totalAvailable = rt::get(*value, "total_available");
      const json::value* totalGranted = rt::get(*value, "total_granted");
      const json::value* totalUsed = rt::get(*value, "total_used");
      return v && rt::isNumber(*v) && rt::toNumber(*v) == 1 && isNonNegativeSafeInteger(rt::get(*value, "bucket_start_at_ms")) &&
             isNonNegativeSafeInteger(rt::get(*value, "observed_at_ms")) && isSafeInteger(rt::get(*value, "balance_quota")) &&
             isSafeInteger(rt::get(*value, "baseline_quota")) && isPositiveSafeInteger(rt::get(*value, "quota_per_credit")) &&
             (isNull(remainingPercent) || isNonNegativeFiniteNumber(remainingPercent)) && (!unlimitedQuota || isBoolean(unlimitedQuota)) &&
             (rt::isNullish(totalAvailable) || isSafeInteger(totalAvailable)) && (rt::isNullish(totalGranted) || isSafeInteger(totalGranted)) &&
             (rt::isNullish(totalUsed) || isSafeInteger(totalUsed));
    }

    // `isMeteredQuotaBalanceSample(value) ? value : null` over a stored value.
    std::optional<MeteredQuotaBalanceSample> balanceSampleOf(const rt::KvValue& value) {
      const json::value view = rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS);
      if (!isMeteredQuotaBalanceSample(&view)) return std::nullopt;
      return json::value_to<MeteredQuotaBalanceSample>(view);
    }

    // `value ?? null` for an optional total.
    rt::Nullable<double> orNull(const rt::Nullable<double>& value) {
      if (value.hasValue()) return *value;
      return nullptr;
    }

    std::optional<MeteredQuotaBalanceSample> balanceSampleFromState(const MeteredQuotaState& state, double observedAtMs) {
      if (!isNonNegativeSafeInteger(observedAtMs)) return std::nullopt;
      const bool tokenUsage = tokenUsageState(state);
      const std::optional<double> remainingPercent =
        tokenUsage ? std::nullopt : remainingPercentOfBaseline(state.current_balance_quota, state.post_refill_baseline_quota);
      MeteredQuotaBalanceSample sample{
        .v = 1,
        .bucket_start_at_ms = rt::Math::floor(observedAtMs / METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS) * METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS,
        .observed_at_ms = observedAtMs,
        .balance_quota = state.current_balance_quota,
        .baseline_quota = state.post_refill_baseline_quota,
        .quota_per_credit = state.quota_per_credit,
        .remaining_percent = remainingPercent,
      };
      if (tokenUsage) {
        sample.unlimited_quota = state.unlimited_quota == true;
        sample.total_available = orNull(state.total_available);
        sample.total_granted = orNull(state.total_granted);
        sample.total_used = orNull(state.total_used);
      }
      return sample;
    }

    MeteredQuotaDiagnostics unavailableDiagnostics(bool configured) {
      return MeteredQuotaDiagnostics{
        .configured = configured,
        .available = false,
        .cache_state = std::nullopt,
        .confidence = std::nullopt,
        .balance_credits = std::nullopt,
        .baseline_credits = std::nullopt,
        .remaining_percent = std::nullopt,
        .used_percent = std::nullopt,
        .observed_at_ms = std::nullopt,
        .cycle_started_at_ms = std::nullopt,
        .last_known_debits_credits = std::nullopt,
        .last_inferred_credit_credits = std::nullopt,
        .last_credit_at_ms = std::nullopt,
        .latest_refill_id = std::nullopt,
        .latest_refill_amount_credits = std::nullopt,
        .latest_refill_completed_at_ms = std::nullopt,
        .unlimited_quota = nullptr,
        .total_available = nullptr,
        .total_granted = nullptr,
        .total_used = nullptr,
      };
    }

    rt::Nullable<double> nullableOf(const std::optional<double>& value) {
      if (value) return *value;
      return nullptr;
    }
  }

  MeteredQuotaBalanceWindowDays normalizeMeteredQuotaBalanceWindowDays(const std::optional<std::string>& value) {
    // `Number(value)`: null is 0.
    const double parsed = value ? rt::stringToNumber(*value) : 0;
    bool listed = false;
    for (const double days : METERED_QUOTA_BALANCE_WINDOW_DAYS) listed = listed || days == parsed;
    return rt::Number::isInteger(parsed) && listed ? parsed : 7;
  }

  std::optional<MeteredAccountCredentials> readMeteredAccountCredentials() {
    return parseCredentials(MeteredAccountCredentials{.apiKey = getEnv(METERED_API_KEY_ENV).value_or("")});
  }

  rt::Task<std::optional<std::string>> meterQuotaAccountFingerprint(std::optional<MeteredAccountCredentials> credentials) {
    const MeteredAccountCredentials input = credentials ? *credentials : MeteredAccountCredentials{.apiKey = ""};
    const std::optional<MeteredAccountCredentials> parsed = parseCredentials(input);
    if (!parsed) co_return std::nullopt;
    std::optional<std::string> fingerprint;
    try {
      const rt::Promise<std::string> hashing(utils::sha256Hex(parsed->apiKey));
      const std::string hex = co_await hashing.wait();
      fingerprint = rt::slice(hex, 0, 16);
    } catch (...) {
      fingerprint = std::nullopt;
    }
    co_return fingerprint;
  }

  rt::Task<MeteredQuotaObservation> fetchMeteredQuotaObservation(MeteredAccountCredentials credentialsInput) {
    return fetchMeteredQuotaObservation(std::move(credentialsInput), FetchMeteredQuotaObservationOptions{});
  }

  rt::Task<MeteredQuotaObservation> fetchMeteredQuotaObservation(MeteredAccountCredentials credentialsInput, FetchMeteredQuotaObservationOptions options) {
    const std::optional<MeteredAccountCredentials> credentials = parseCredentials(credentialsInput);
    if (!credentials) throw rt::JsError("Metered account credentials are invalid");
    const provider::metered::MeteredFetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
    const rt::AbortSignal signal = combinedSignal(options.signal);
    const rt::Promise<json::value> fetching(fetchJson(std::string(METERED_TOKEN_USAGE_URL), fetcher, authenticatedHeaders(*credentials), signal));
    const json::value usageEnvelope = co_await fetching.wait();
    const json::value* usage = successfulEnvelopeData(usageEnvelope);
    if (!usage) throw rt::JsError("Metered account API returned an invalid envelope");
    const json::value* unlimitedQuota = rt::get(*usage, "unlimited_quota");
    const json::value* totalAvailable = rt::get(*usage, "total_available");
    const json::value* totalGranted = rt::get(*usage, "total_granted");
    const json::value* totalUsed = rt::get(*usage, "total_used");
    if (!isBoolean(unlimitedQuota) || !isSafeInteger(totalAvailable) || !isSafeInteger(totalGranted) || !isSafeInteger(totalUsed))
      throw rt::JsError("Metered account API returned invalid token usage data");
    const double observedAtMs = rt::Math::trunc(options.now ? options.now() : rt::dateNow());
    if (!isNonNegativeSafeInteger(observedAtMs)) throw rt::JsError("Metered quota observation clock is invalid");
    MeteredQuotaObservation observation{
      .balance_quota = std::nullopt,
      .used_quota = std::nullopt,
      .quota_per_credit = std::nullopt,
      .observed_at_ms = observedAtMs,
      .latest_refill = std::nullopt,
      .unlimited_quota = unlimitedQuota->get_bool(),
      .total_available = rt::toNumber(*totalAvailable),
      .total_granted = rt::toNumber(*totalGranted),
      .total_used = rt::toNumber(*totalUsed),
    };
    co_return observation;
  }

  MeteredQuotaState updateMeteredQuotaState(const std::optional<MeteredQuotaState>& previous, const MeteredQuotaObservation& observation) {
    if (hasTokenUsageObservation(observation)) return tokenUsageQuotaState(observation);
    const MeteredWalletObservation wallet = meteredWalletObservation(observation);
    if (!previous) return initialWalletQuotaState(wallet);
    return advancedWalletQuotaState(*previous, wallet);
  }

  bool isMeteredQuotaState(const json::value* value) {
    if (!utils::isRecord(value)) return false;
    const json::value* lastCreditAtMs = rt::get(*value, "last_credit_at_ms");
    const json::value* latestRefillId = rt::get(*value, "latest_refill_id");
    const json::value* latestRefillAmountCredits = rt::get(*value, "latest_refill_amount_credits");
    const json::value* latestRefillCompletedAtMs = rt::get(*value, "latest_refill_completed_at_ms");
    const json::value* unlimitedQuota = rt::get(*value, "unlimited_quota");
    const json::value* totalAvailable = rt::get(*value, "total_available");
    const json::value* totalGranted = rt::get(*value, "total_granted");
    const json::value* totalUsed = rt::get(*value, "total_used");
    return isSafeInteger(rt::get(*value, "current_balance_quota")) && isSafeInteger(rt::get(*value, "post_refill_baseline_quota")) &&
           isNonNegativeSafeInteger(rt::get(*value, "last_observed_used_quota")) && isPositiveSafeInteger(rt::get(*value, "quota_per_credit")) &&
           isNonNegativeSafeInteger(rt::get(*value, "observed_at_ms")) && isNonNegativeSafeInteger(rt::get(*value, "cycle_started_at_ms")) &&
           isConfidence(rt::get(*value, "confidence")) && isNonNegativeSafeInteger(rt::get(*value, "last_known_debits_quota")) &&
           isNonNegativeSafeInteger(rt::get(*value, "last_inferred_credit_quota")) && (isNull(lastCreditAtMs) || isNonNegativeSafeInteger(lastCreditAtMs)) &&
           (isNull(latestRefillId) || (latestRefillId && latestRefillId->is_string())) &&
           (isNull(latestRefillAmountCredits) || isNonNegativeFiniteNumber(latestRefillAmountCredits)) &&
           (isNull(latestRefillCompletedAtMs) || isNonNegativeSafeInteger(latestRefillCompletedAtMs)) && (!unlimitedQuota || isBoolean(unlimitedQuota)) &&
           (rt::isNullish(totalAvailable) || isSafeInteger(totalAvailable)) && (rt::isNullish(totalGranted) || isSafeInteger(totalGranted)) &&
           (rt::isNullish(totalUsed) || isSafeInteger(totalUsed));
  }

  bool isMeteredQuotaState(const json::value& value) { return isMeteredQuotaState(&value); }

  bool isMeteredQuotaState(const rt::KvValue& value) {
    const json::value view = rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS);
    return isMeteredQuotaState(&view);
  }

  rt::Task<std::optional<MeteredQuotaSnapshot>> getMeteredQuotaSnapshot(MeteredAccountCredentials credentials) {
    return getMeteredQuotaSnapshot(std::move(credentials), GetMeteredQuotaSnapshotOptions{});
  }

  rt::Task<std::optional<MeteredQuotaSnapshot>> getMeteredQuotaSnapshot(MeteredAccountCredentials credentials, GetMeteredQuotaSnapshotOptions options) {
    // `options.kv === undefined ? await getKv() : options.kv`
    std::shared_ptr<rt::Kv> kv;
    if (options.kv) {
      kv = *options.kv;
    } else {
      const rt::Promise<std::shared_ptr<rt::Kv>> opening = kv::getKv();
      kv = co_await opening.wait();
    }
    if (!kv || !parseCredentials(credentials)) co_return std::nullopt;
    const std::function<double()> now = options.now ? options.now : rt::globalDateNow();
    const double nowMs = rt::Math::trunc(now());
    const rt::Promise<CachedQuotaState> reading(readCachedQuotaState(kv, nowMs));
    const CachedQuotaState cachedState = co_await reading.wait();
    const std::optional<MeteredQuotaState>& cached = cachedState.cached;
    if (cached && !options.forceRefresh.value_or(false) && !cachedState.cachedInvalidated && nowMs - cached->observed_at_ms < METERED_QUOTA_FRESH_MS) {
      co_return toSnapshot(*cached, "fresh");
    }

    const std::string owner = options.createLeaseOwner ? options.createLeaseOwner() : rt::crypto::randomUUID();
    const rt::Promise<bool> acquiring = rt::Promise<bool>(acquireRefreshLease(kv, owner, nowMs)).catch_([](const rt::Reason& error) {
      rt::console::warn("[ai.ubq.fi] Metered quota refresh lease acquisition failed:", consoleValue(error));
      return false;
    });
    const bool acquired = co_await acquiring.wait();
    if (!acquired) {
      const rt::Promise<std::optional<MeteredQuotaSnapshot>> waiting(coldWaitSnapshot(kv, cached));
      const std::optional<MeteredQuotaSnapshot> waited = co_await waiting.wait();
      co_return waited;
    }

    std::optional<MeteredQuotaSnapshot> result;
    rt::Reason failure;
    try {
      const rt::Promise<std::optional<MeteredQuotaSnapshot>> refreshing(refreshMeteredQuotaSnapshot(kv, credentials, options, cached, owner, now));
      result = co_await refreshing.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    rt::Reason rethrown;
    if (failure) {
      if (options.signal && options.signal->aborted()) {
        rethrown = failure;
      } else {
        const std::string message = rt::isError(failure) ? rt::errorMessage(failure).value_or("") : rt::reasonToString(failure);
        rt::console::warn("[ai.ubq.fi] Metered quota refresh failed:", message);
        result = cached ? std::optional<MeteredQuotaSnapshot>(toSnapshot(*cached, "stale")) : std::nullopt;
      }
    }
    const rt::Promise<void> releasing(releaseRefreshLease(kv, owner));
    co_await releasing.wait();
    if (rethrown) std::rethrow_exception(rethrown);
    co_return result;
  }

  rt::Task<std::optional<MeteredQuotaSnapshot>> getCachedMeteredQuotaSnapshot() { return getCachedMeteredQuotaSnapshot(GetCachedMeteredQuotaSnapshotOptions{}); }

  rt::Task<std::optional<MeteredQuotaSnapshot>> getCachedMeteredQuotaSnapshot(GetCachedMeteredQuotaSnapshotOptions options) {
    // `options.kv === undefined ? await getKv() : options.kv`
    std::shared_ptr<rt::Kv> kv;
    if (options.kv) {
      kv = *options.kv;
    } else {
      const rt::Promise<std::shared_ptr<rt::Kv>> opening = kv::getKv();
      kv = co_await opening.wait();
    }
    if (!kv) co_return std::nullopt;
    const double nowMs = rt::Math::trunc(options.now ? options.now() : rt::dateNow());
    std::optional<MeteredQuotaSnapshot> result;
    rt::Reason failure;
    try {
      const rt::Promise<std::optional<MeteredQuotaState>> stateReading(loadRetainedState(kv, nowMs));
      const rt::Promise<rt::KvEntryMaybe<>> invalidationReading = loadInvalidation(kv);
      const auto both = rt::all(stateReading, invalidationReading);
      const std::tuple<std::optional<MeteredQuotaState>, rt::KvEntryMaybe<>> entries = co_await both.wait();
      const std::optional<MeteredQuotaState>& state = std::get<0>(entries);
      if (state) {
        const bool fresh = !isInvalidated(*state, quotaInvalidationValue(std::get<1>(entries).value)) && nowMs - state->observed_at_ms < METERED_QUOTA_FRESH_MS;
        result = toSnapshot(*state, fresh ? "fresh" : "stale");
      }
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      rt::console::warn("[ai.ubq.fi] Metered quota cache peek failed:", consoleValue(failure));
      co_return std::nullopt;
    }
    co_return result;
  }

  rt::Task<void> invalidateMeteredQuotaSnapshot() { return invalidateMeteredQuotaSnapshot(GetCachedMeteredQuotaSnapshotOptions{}); }

  rt::Task<void> invalidateMeteredQuotaSnapshot(GetCachedMeteredQuotaSnapshotOptions options) {
    // `options.kv === undefined ? await getKv() : options.kv`
    std::shared_ptr<rt::Kv> kv;
    if (options.kv) {
      kv = *options.kv;
    } else {
      const rt::Promise<std::shared_ptr<rt::Kv>> opening = kv::getKv();
      kv = co_await opening.wait();
    }
    if (!kv) co_return;
    const double invalidatedAtMs = rt::Math::trunc(options.now ? options.now() : rt::dateNow());
    if (!isNonNegativeSafeInteger(invalidatedAtMs)) throw rt::JsError("Metered quota invalidation clock is invalid");
    json::object invalidation;
    invalidation["invalidated_at_ms"] = rt::makeNumber(invalidatedAtMs);
    const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
    operation->set(METERED_QUOTA_INVALIDATION_KEY, rt::KvValue(invalidation), rt::KvSetOptions{.expireIn = METERED_QUOTA_RETENTION_MS});
    const rt::Promise<rt::KvCommitResult> committing(operation->commit());
    const rt::KvCommitResult committed = co_await committing.wait();
    if (!committed.ok) throw rt::JsError("Deno KV could not invalidate the Metered quota snapshot");
  }

  rt::Task<void> writeMeteredQuotaBalanceSample(std::shared_ptr<rt::Kv> kv, MeteredQuotaState state, double observedAtMs,
                                                std::optional<std::string> accountFingerprint) {
    const std::optional<MeteredQuotaBalanceSample> sample = balanceSampleFromState(state, observedAtMs);
    if (!sample || !accountFingerprint || accountFingerprint->empty()) co_return;
    const rt::KvKey key{METERED_QUOTA_BALANCE_HISTORY_PREFIX, *accountFingerprint, sample->bucket_start_at_ms};
    const rt::KvValue stored(json::value_from(*sample));
    for (int attempt = 0; attempt < 3; attempt += 1) {
      const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
      const rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(key, strong));
      const rt::KvEntryMaybe<> entry = co_await reading.wait();
      const std::optional<MeteredQuotaBalanceSample> existing = balanceSampleOf(entry.value);
      if (existing && existing->observed_at_ms >= sample->observed_at_ms) co_return;
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(entry).set(key, stored);
      const rt::Promise<rt::KvCommitResult> committing(operation->commit());
      const rt::KvCommitResult committed = co_await committing.wait();
      if (committed.ok) co_return;
    }
    throw rt::JsError("Metered quota balance history changed concurrently.");
  }

  rt::Task<std::vector<MeteredQuotaBalanceSample>> readMeteredQuotaBalanceHistory(std::shared_ptr<rt::Kv> kv, ReadMeteredQuotaBalanceHistoryOptions options) {
    if (!kv || !options.accountFingerprint || options.accountFingerprint->empty()) co_return std::vector<MeteredQuotaBalanceSample>{};
    const double limit = rt::Math::max(1.0, rt::Math::min(10'000.0, rt::Math::trunc(options.limit.value_or(10'000))));
    std::vector<MeteredQuotaBalanceSample> samples;
    const double sinceMs = rt::Math::max(0.0, rt::Math::trunc(options.sinceMs));
    const double nowMs = rt::Math::max(sinceMs, rt::Math::trunc(options.nowMs));
    const rt::KvKey prefix{METERED_QUOTA_BALANCE_HISTORY_PREFIX, *options.accountFingerprint};
    const rt::KvKey start{prefix, rt::Math::floor(sinceMs / METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS) * METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS};
    const rt::KvKey end{
      prefix,
      rt::Math::floor(nowMs / METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS) * METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS + METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS,
    };
    // Deno KV rejects selectors that combine a prefix with both range bounds.
    // These bounds already include the complete account-specific namespace.
    rt::KvListSelector selector;
    selector.start = start;
    selector.end = end;
    rt::KvListOptions listOptions;
    listOptions.limit = limit;
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, listOptions);
    while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      const std::optional<MeteredQuotaBalanceSample> sample = balanceSampleOf(entry->value);
      if (sample && sample->observed_at_ms >= sinceMs && sample->observed_at_ms <= nowMs) samples.push_back(*sample);
    }
    rt::sort(samples, [](const MeteredQuotaBalanceSample& left, const MeteredQuotaBalanceSample& right) {
      return left.bucket_start_at_ms - right.bucket_start_at_ms;
    });
    co_return samples;
  }

  std::vector<MeteredQuotaBalanceSample> resampleMeteredQuotaBalanceHistory(const std::vector<MeteredQuotaBalanceSample>& samples, double bucketMs, double limit) {
    if (!rt::Number::isSafeInteger(bucketMs) || bucketMs <= 0 || !rt::Number::isSafeInteger(limit) || limit <= 0) return {};
    rt::OrderedMap<double, MeteredQuotaBalanceSample> buckets;
    for (const MeteredQuotaBalanceSample& sample : samples) {
      const double bucketStartAtMs = rt::Math::floor(sample.observed_at_ms / bucketMs) * bucketMs;
      const MeteredQuotaBalanceSample* existing = buckets.get(bucketStartAtMs);
      if (!existing || sample.observed_at_ms > existing->observed_at_ms ||
          (sample.observed_at_ms == existing->observed_at_ms && sample.bucket_start_at_ms > existing->bucket_start_at_ms)) {
        MeteredQuotaBalanceSample resampled = sample;
        resampled.bucket_start_at_ms = bucketStartAtMs;
        buckets.set(bucketStartAtMs, std::move(resampled));
      }
    }
    std::vector<MeteredQuotaBalanceSample> ordered;
    for (const MeteredQuotaBalanceSample& sample : buckets.values()) ordered.push_back(sample);
    rt::sort(ordered, [](const MeteredQuotaBalanceSample& left, const MeteredQuotaBalanceSample& right) {
      return left.bucket_start_at_ms - right.bucket_start_at_ms;
    });
    // `.slice(-limit)`
    const std::size_t keep = static_cast<std::size_t>(std::min(limit, static_cast<double>(ordered.size())));
    return std::vector<MeteredQuotaBalanceSample>(ordered.end() - static_cast<std::ptrdiff_t>(keep), ordered.end());
  }

  rt::Task<std::optional<MeteredQuotaSnapshot>> getConfiguredMeteredQuotaSnapshot() {
    return getConfiguredMeteredQuotaSnapshot(GetMeteredQuotaSnapshotOptions{});
  }

  rt::Task<std::optional<MeteredQuotaSnapshot>> getConfiguredMeteredQuotaSnapshot(GetMeteredQuotaSnapshotOptions options) {
    const std::optional<MeteredAccountCredentials> credentials = readMeteredAccountCredentials();
    if (!credentials) co_return std::nullopt;
    const rt::Promise<std::optional<MeteredQuotaSnapshot>> loading(getMeteredQuotaSnapshot(*credentials, options));
    const std::optional<MeteredQuotaSnapshot> snapshot = co_await loading.wait();
    co_return snapshot;
  }

  rt::Task<std::optional<MeteredQuotaSnapshot>> getCachedConfiguredMeteredQuotaSnapshot() {
    return getCachedConfiguredMeteredQuotaSnapshot(GetCachedMeteredQuotaSnapshotOptions{});
  }

  rt::Task<std::optional<MeteredQuotaSnapshot>> getCachedConfiguredMeteredQuotaSnapshot(GetCachedMeteredQuotaSnapshotOptions options) {
    if (!readMeteredAccountCredentials()) co_return std::nullopt;
    const rt::Promise<std::optional<MeteredQuotaSnapshot>> loading(getCachedMeteredQuotaSnapshot(options));
    const std::optional<MeteredQuotaSnapshot> snapshot = co_await loading.wait();
    co_return snapshot;
  }

  rt::Task<MeteredQuotaDiagnostics> getMeteredQuotaDiagnostics() { return getMeteredQuotaDiagnostics(GetMeteredQuotaSnapshotOptions{}); }

  rt::Task<MeteredQuotaDiagnostics> getMeteredQuotaDiagnostics(GetMeteredQuotaSnapshotOptions options) {
    const std::optional<MeteredAccountCredentials> credentials = readMeteredAccountCredentials();
    if (!credentials) co_return unavailableDiagnostics(false);
    const rt::Promise<std::optional<MeteredQuotaSnapshot>> loading(getMeteredQuotaSnapshot(*credentials, options));
    const std::optional<MeteredQuotaSnapshot> snapshot = co_await loading.wait();
    if (!snapshot) co_return unavailableDiagnostics(true);
    const bool tokenUsage = snapshot->unlimited_quota || snapshot->total_available || snapshot->total_used;
    const MeteredQuotaState& state = snapshot->state;
    MeteredQuotaDiagnostics diagnostics{
      .configured = true,
      .available = true,
      .cache_state = snapshot->cache_state,
      .confidence = tokenUsage ? std::nullopt : std::optional<MeteredQuotaConfidence>(state.confidence),
      .balance_credits = snapshot->balance_credits,
      .baseline_credits = snapshot->baseline_credits,
      .remaining_percent = snapshot->remaining_percent,
      .used_percent = snapshot->used_percent,
      .observed_at_ms = state.observed_at_ms,
      .cycle_started_at_ms = tokenUsage ? std::nullopt : std::optional<double>(state.cycle_started_at_ms),
      .last_known_debits_credits = tokenUsage ? std::nullopt : std::optional<double>(state.last_known_debits_quota / state.quota_per_credit),
      .last_inferred_credit_credits = snapshot->last_inferred_credit_credits,
      .last_credit_at_ms = tokenUsage ? std::nullopt : state.last_credit_at_ms,
      .latest_refill_id = state.latest_refill_id,
      .latest_refill_amount_credits = state.latest_refill_amount_credits,
      .latest_refill_completed_at_ms = state.latest_refill_completed_at_ms,
      .unlimited_quota = snapshot->unlimited_quota,
      .total_available = nullableOf(snapshot->total_available),
      .total_granted = nullableOf(snapshot->total_granted),
      .total_used = nullableOf(snapshot->total_used),
    };
    co_return diagnostics;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredRefillObservation& value) {
    out = Writer().required("id", value.id).required("amount_credits", value.amount_credits).required("completed_at_ms", value.completed_at_ms).done();
  }

  MeteredRefillObservation tag_invoke(const json::value_to_tag<MeteredRefillObservation>&, const json::value& value) {
    const Reader in(value, "MeteredRefillObservation");
    return MeteredRefillObservation{
      .id = in.required<std::string>("id"),
      .amount_credits = in.required<double>("amount_credits"),
      .completed_at_ms = in.required<double>("completed_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaObservation& value) {
    out = Writer()
            .orNull("balance_quota", value.balance_quota)
            .orNull("used_quota", value.used_quota)
            .orNull("quota_per_credit", value.quota_per_credit)
            .required("observed_at_ms", value.observed_at_ms)
            .orNull("latest_refill", value.latest_refill)
            .orUndefined("unlimited_quota", value.unlimited_quota)
            .orUndefined("total_available", value.total_available)
            .orUndefined("total_granted", value.total_granted)
            .orUndefined("total_used", value.total_used)
            .done();
  }

  MeteredQuotaObservation tag_invoke(const json::value_to_tag<MeteredQuotaObservation>&, const json::value& value) {
    const Reader in(value, "MeteredQuotaObservation");
    return MeteredQuotaObservation{
      .balance_quota = in.orNull<double>("balance_quota"),
      .used_quota = in.orNull<double>("used_quota"),
      .quota_per_credit = in.orNull<double>("quota_per_credit"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
      .latest_refill = in.orNull<MeteredRefillObservation>("latest_refill"),
      .unlimited_quota = in.orUndefined<bool>("unlimited_quota"),
      .total_available = in.orUndefined<double>("total_available"),
      .total_granted = in.orUndefined<double>("total_granted"),
      .total_used = in.orUndefined<double>("total_used"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaState& value) {
    out = Writer()
            .required("current_balance_quota", value.current_balance_quota)
            .required("post_refill_baseline_quota", value.post_refill_baseline_quota)
            .required("last_observed_used_quota", value.last_observed_used_quota)
            .required("quota_per_credit", value.quota_per_credit)
            .required("observed_at_ms", value.observed_at_ms)
            .required("cycle_started_at_ms", value.cycle_started_at_ms)
            .required("confidence", value.confidence)
            .required("last_known_debits_quota", value.last_known_debits_quota)
            .required("last_inferred_credit_quota", value.last_inferred_credit_quota)
            .orNull("last_credit_at_ms", value.last_credit_at_ms)
            .orNull("latest_refill_id", value.latest_refill_id)
            .orNull("latest_refill_amount_credits", value.latest_refill_amount_credits)
            .orNull("latest_refill_completed_at_ms", value.latest_refill_completed_at_ms)
            .orUndefined("unlimited_quota", value.unlimited_quota)
            .nullable("total_available", value.total_available)
            .nullable("total_granted", value.total_granted)
            .nullable("total_used", value.total_used)
            .done();
  }

  MeteredQuotaState tag_invoke(const json::value_to_tag<MeteredQuotaState>&, const json::value& value) {
    const Reader in(value, "MeteredQuotaState");
    return MeteredQuotaState{
      .current_balance_quota = in.required<double>("current_balance_quota"),
      .post_refill_baseline_quota = in.required<double>("post_refill_baseline_quota"),
      .last_observed_used_quota = in.required<double>("last_observed_used_quota"),
      .quota_per_credit = in.required<double>("quota_per_credit"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
      .cycle_started_at_ms = in.required<double>("cycle_started_at_ms"),
      .confidence = in.required<std::string>("confidence"),
      .last_known_debits_quota = in.required<double>("last_known_debits_quota"),
      .last_inferred_credit_quota = in.required<double>("last_inferred_credit_quota"),
      .last_credit_at_ms = in.orNull<double>("last_credit_at_ms"),
      .latest_refill_id = in.orNull<std::string>("latest_refill_id"),
      .latest_refill_amount_credits = in.orNull<double>("latest_refill_amount_credits"),
      .latest_refill_completed_at_ms = in.orNull<double>("latest_refill_completed_at_ms"),
      .unlimited_quota = in.orUndefined<bool>("unlimited_quota"),
      .total_available = in.nullable<double>("total_available"),
      .total_granted = in.nullable<double>("total_granted"),
      .total_used = in.nullable<double>("total_used"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaBalanceSample& value) {
    out = Writer()
            .required("v", value.v)
            .required("bucket_start_at_ms", value.bucket_start_at_ms)
            .required("observed_at_ms", value.observed_at_ms)
            .required("balance_quota", value.balance_quota)
            .required("baseline_quota", value.baseline_quota)
            .required("quota_per_credit", value.quota_per_credit)
            .orNull("remaining_percent", value.remaining_percent)
            .orUndefined("unlimited_quota", value.unlimited_quota)
            .nullable("total_available", value.total_available)
            .nullable("total_granted", value.total_granted)
            .nullable("total_used", value.total_used)
            .done();
  }

  MeteredQuotaBalanceSample tag_invoke(const json::value_to_tag<MeteredQuotaBalanceSample>&, const json::value& value) {
    const Reader in(value, "MeteredQuotaBalanceSample");
    return MeteredQuotaBalanceSample{
      .v = in.required<double>("v"),
      .bucket_start_at_ms = in.required<double>("bucket_start_at_ms"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
      .balance_quota = in.required<double>("balance_quota"),
      .baseline_quota = in.required<double>("baseline_quota"),
      .quota_per_credit = in.required<double>("quota_per_credit"),
      .remaining_percent = in.orNull<double>("remaining_percent"),
      .unlimited_quota = in.orUndefined<bool>("unlimited_quota"),
      .total_available = in.nullable<double>("total_available"),
      .total_granted = in.nullable<double>("total_granted"),
      .total_used = in.nullable<double>("total_used"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaSnapshot& value) {
    out = Writer()
            .required("state", value.state)
            .required("cache_state", value.cache_state)
            .orNull("balance_credits", value.balance_credits)
            .orNull("baseline_credits", value.baseline_credits)
            .orNull("last_inferred_credit_credits", value.last_inferred_credit_credits)
            .orNull("remaining_percent", value.remaining_percent)
            .orNull("used_percent", value.used_percent)
            .required("unlimited_quota", value.unlimited_quota)
            .orNull("total_available", value.total_available)
            .orNull("total_granted", value.total_granted)
            .orNull("total_used", value.total_used)
            .done();
  }

  MeteredQuotaSnapshot tag_invoke(const json::value_to_tag<MeteredQuotaSnapshot>&, const json::value& value) {
    const Reader in(value, "MeteredQuotaSnapshot");
    return MeteredQuotaSnapshot{
      .state = in.required<MeteredQuotaState>("state"),
      .cache_state = in.required<std::string>("cache_state"),
      .balance_credits = in.orNull<double>("balance_credits"),
      .baseline_credits = in.orNull<double>("baseline_credits"),
      .last_inferred_credit_credits = in.orNull<double>("last_inferred_credit_credits"),
      .remaining_percent = in.orNull<double>("remaining_percent"),
      .used_percent = in.orNull<double>("used_percent"),
      .unlimited_quota = in.required<bool>("unlimited_quota"),
      .total_available = in.orNull<double>("total_available"),
      .total_granted = in.orNull<double>("total_granted"),
      .total_used = in.orNull<double>("total_used"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaDiagnostics& value) {
    out = Writer()
            .required("configured", value.configured)
            .required("available", value.available)
            .orNull("cache_state", value.cache_state)
            .orNull("confidence", value.confidence)
            .orNull("balance_credits", value.balance_credits)
            .orNull("baseline_credits", value.baseline_credits)
            .orNull("remaining_percent", value.remaining_percent)
            .orNull("used_percent", value.used_percent)
            .orNull("observed_at_ms", value.observed_at_ms)
            .orNull("cycle_started_at_ms", value.cycle_started_at_ms)
            .orNull("last_known_debits_credits", value.last_known_debits_credits)
            .orNull("last_inferred_credit_credits", value.last_inferred_credit_credits)
            .orNull("last_credit_at_ms", value.last_credit_at_ms)
            .orNull("latest_refill_id", value.latest_refill_id)
            .orNull("latest_refill_amount_credits", value.latest_refill_amount_credits)
            .orNull("latest_refill_completed_at_ms", value.latest_refill_completed_at_ms)
            .nullable("unlimited_quota", value.unlimited_quota)
            .nullable("total_available", value.total_available)
            .nullable("total_granted", value.total_granted)
            .nullable("total_used", value.total_used)
            .done();
  }

  MeteredQuotaDiagnostics tag_invoke(const json::value_to_tag<MeteredQuotaDiagnostics>&, const json::value& value) {
    const Reader in(value, "MeteredQuotaDiagnostics");
    return MeteredQuotaDiagnostics{
      .configured = in.required<bool>("configured"),
      .available = in.required<bool>("available"),
      .cache_state = in.orNull<std::string>("cache_state"),
      .confidence = in.orNull<std::string>("confidence"),
      .balance_credits = in.orNull<double>("balance_credits"),
      .baseline_credits = in.orNull<double>("baseline_credits"),
      .remaining_percent = in.orNull<double>("remaining_percent"),
      .used_percent = in.orNull<double>("used_percent"),
      .observed_at_ms = in.orNull<double>("observed_at_ms"),
      .cycle_started_at_ms = in.orNull<double>("cycle_started_at_ms"),
      .last_known_debits_credits = in.orNull<double>("last_known_debits_credits"),
      .last_inferred_credit_credits = in.orNull<double>("last_inferred_credit_credits"),
      .last_credit_at_ms = in.orNull<double>("last_credit_at_ms"),
      .latest_refill_id = in.orNull<std::string>("latest_refill_id"),
      .latest_refill_amount_credits = in.orNull<double>("latest_refill_amount_credits"),
      .latest_refill_completed_at_ms = in.orNull<double>("latest_refill_completed_at_ms"),
      .unlimited_quota = in.nullable<bool>("unlimited_quota"),
      .total_available = in.nullable<double>("total_available"),
      .total_granted = in.nullable<double>("total_granted"),
      .total_used = in.nullable<double>("total_used"),
    };
  }
}
