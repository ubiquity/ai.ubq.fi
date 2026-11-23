// Port of tests/oss-accounting-lifecycle.test.ts (all ten cases).
// Links: tests/oss_accounting_lifecycle_test.cpp tests/helpers/counting_kv.cpp src/api_key_policy.cpp src/api_keys.cpp src/auth/relay.cpp
//        src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/authority/native_contracts.cpp
//        src/authority/native_paid_policy.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/bounded_response_body.cpp
//        src/config.cpp src/http.cpp src/inference_deadline.cpp src/kv.cpp src/types.cpp src/paid_fallback/ledger_admission.cpp
//        src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp
//        src/paid_fallback/ledger_stats.cpp src/paid_fallback/rollups.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp
//        src/provider/anthropic_responses.cpp src/provider/cerebras_rate_limits.cpp src/provider/metered.cpp src/provider/surplus_claude_request.cpp
//        src/release.cpp src/responses_stream.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
//        src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/utils.cpp
// The paid-fallback cases start the module's one-time background backfill (admitPaidFallbackV3
// schedules it on first use, as in the TS); each lets it settle before its loop ends
// (scheduledPaidFallbackBackfillForTest).
#include "uos/api_key_policy.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/api_keys.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/ledger_admission.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/paid_fallback/ledger_settlement.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/types.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::api_key_policy;
using uos::tests::helpers::counting_kv::CountingKv;
using uos::tests::helpers::counting_kv::encodeKey;
namespace ledger_admission = uos::paid_fallback::ledger_admission;
namespace ledger_backfill = uos::paid_fallback::ledger_backfill;
namespace ledger_settlement = uos::paid_fallback::ledger_settlement;
namespace ledger_state = uos::paid_fallback::ledger_state;
using uos::authority::native_paid_contracts::PaidFallbackAdmissionV3;
using uos::authority::native_paid_contracts::PaidFallbackRequestLifecyclePatchV3;

namespace {
  /**
   * Adversarial reservation-lifecycle fixtures for the V3 API-key ledger.
   *
   * These exercise the identity and compare-and-set boundary of one reservation:
   * duplicate commit/release callbacks, cancellation racing usage settlement, and
   * request ids re-presented by the caller. The assertions are economic
   * invariants (a provider transport may only proceed against its own charge, and
   * the window may never under- or over-count), not a mirror of the code.
   */

  const std::string ROUTE = "responses";

  struct Setup {
    std::shared_ptr<CountingKv> kv;
    ApiKeyPolicy policy;
  };

  Setup setupPolicy(const std::string& id, double usageLimitRequests, double nowMs) {
    const std::string tokenHash = "oss-accounting-" + id;
    const uos::types::ApiKeyHashRecord record{
      .id = id,
      .expires_at_ms = -1,
      .revoked_at_ms = std::nullopt,
      .usage_limit_requests = usageLimitRequests,
      .usage_requests = 0,
      .usage_reset_at_ms = nowMs + 60 * 60'000,
      .window_ms = 60 * 60'000,
      .usage_quota_version = 3,
      .paid_fallback_enabled = false,
      .paid_fallback_limit_microcredits = 0,
      .paid_fallback_spent_microcredits = 0,
      .paid_fallback_reserved_microcredits = 0,
      .paid_fallback_reservation_request_id = std::nullopt,
    };
    const std::optional<ApiKeyPolicy> policy = apiKeyPolicyFromHashRecord(tokenHash, record, nowMs);
    if (!policy) throw std::runtime_error("test API key policy must be valid");
    auto kv = std::make_shared<CountingKv>();
    kv->seed(uos::api_keys::apiKeyHashKey(tokenHash), rt::KvValue(json::value_from(record)));
    return Setup{.kv = kv, .policy = *policy};
  }
  Setup setupPolicy(const std::string& id, double usageLimitRequests) { return setupPolicy(id, usageLimitRequests, rt::dateNow()); }

  // `kv.entries.get(JSON.stringify(key))?.value ?? null`
  json::value storedValue(const CountingKv& kv, const rt::KvKey& key) {
    const auto* entry = kv.entries.get(encodeKey(key));
    return entry ? entry->value.toJson() : json::value(nullptr);
  }

  json::value windowFor(const CountingKv& kv, const ApiKeyPolicy& policy) {
    json::value window = storedValue(kv, apiKeyUsageV3WindowKey(policy));
    if (window.is_null()) throw std::runtime_error("expected a V3 usage window");
    return window;
  }

  json::value requestFor(const CountingKv& kv, const ApiKeyPolicy& policy, const std::string& requestId) {
    return storedValue(kv, apiKeyUsageV3RequestKey(policy, requestId));
  }

  double numberAt(const json::value& value, std::string_view member) { return rt::getNumber(rt::get(value, member)).value_or(rt::Number::NaN); }

  /** The row and aggregate a client could observe for one request id. */
  json::object accountingFor(const CountingKv& kv, const ApiKeyPolicy& policy, const std::string& requestId) {
    const json::value window = windowFor(kv, policy);
    const json::value request = requestFor(kv, policy, requestId);
    json::object accounting;
    accounting["committed"] = *rt::get(window, "committed_requests");
    accounting["reserved"] = *rt::get(window, "reserved_requests");
    const json::value* state = rt::get(request, "state");
    accounting["state"] = state != nullptr && !state->is_null() ? *state : json::value(nullptr);
    const json::value* reason = rt::get(request, "release_reason");
    accounting["reason"] = reason != nullptr && !reason->is_null() ? *reason : json::value(nullptr);
    return accounting;
  }

  bool sameJson(const json::value& actual, std::string_view expected) { return rt::deepEqual(actual, rt::jsonParse(expected)); }

  /** Every reservation mutation must keep the aggregate consistent with the row. */
  void assertLedgerConsistent(const CountingKv& kv, const ApiKeyPolicy& policy, const std::string& requestId, double limit) {
    const json::value accounting = accountingFor(kv, policy, requestId);
    const double committed = numberAt(accounting, "committed");
    const double reserved = numberAt(accounting, "reserved");
    const json::value* state = rt::get(accounting, "state");
    // reserved_requests must never underflow
    EXPECT(reserved >= 0);
    // committed + reserved must never exceed the window limit
    EXPECT(committed + reserved <= limit);
    // a reserved row owns no committed request
    if (sameJson(*state, R"("reserved")")) EXPECT_EQ(committed, 0.0);
    // a dispatched row owns exactly one committed request
    if (sameJson(*state, R"("dispatched")")) EXPECT_EQ(committed, 1.0);
    // a released row owns no committed request
    if (sameJson(*state, R"("released")")) EXPECT_EQ(committed, 0.0);
  }

  rt::Task<ApiKeyUsageReservationDecision> reserve(std::shared_ptr<CountingKv> kv, ApiKeyPolicy policy, std::string requestId, std::optional<double> nowMs,
                                                   std::string route) {
    ReserveApiKeyUsageV3Options options;
    options.kv = kv;
    if (nowMs) options.nowMs = *nowMs;
    rt::Task<ApiKeyUsageReservationDecision> reserving = reserveApiKeyUsageV3(std::move(policy), std::move(requestId), std::move(route), std::move(options));
    ApiKeyUsageReservationDecision decision = co_await std::move(reserving);
    co_return decision;
  }
  rt::Task<ApiKeyUsageReservationDecision> reserve(std::shared_ptr<CountingKv> kv, ApiKeyPolicy policy, std::string requestId) {
    rt::Task<ApiKeyUsageReservationDecision> reserving = reserve(std::move(kv), std::move(policy), std::move(requestId), std::nullopt, ROUTE);
    ApiKeyUsageReservationDecision decision = co_await std::move(reserving);
    co_return decision;
  }

  ApiKeyUsageReservation admitted(const ApiKeyUsageReservationDecision& decision) {
    if (const auto* refused = std::get_if<ApiKeyUsageReservationDecisionNotOk>(&decision)) {
      throw std::runtime_error("unexpected admission failure: " + std::to_string(refused->response.status()));
    }
    return std::get<ApiKeyUsageReservationDecisionOk>(decision).reservation;
  }

  rt::Task<std::optional<ApiKeyProviderDispatch>> dispatchFor(ApiKeyUsageReservation reservation, std::string provider) {
    rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = reservation.beforeProviderDispatch(std::move(provider));
    std::optional<ApiKeyProviderDispatch> dispatch = co_await std::move(dispatching);
    co_return dispatch;
  }

  rt::Task<void> releaseWith(ApiKeyUsageReservation reservation, std::optional<std::string> reason) {
    rt::Task<void> releasing = reservation.release(std::move(reason));
    co_await std::move(releasing);
  }

  bool isQuotaDispatchError(const rt::Reason& reason) {
    if (!reason) return false;
    try {
      std::rethrow_exception(reason);
    } catch (const ApiKeyQuotaDispatchError&) {
      return true;
    } catch (...) {
      return false;
    }
  }

  /**
   * Paid-fallback request rows are keyed by (key id, request id) with no window
   * component, and `admitPaidFallbackV3` projects any existing row back into a
   * reservation. The ledger's own billing contract is `isBillableRequestV3`, which
   * treats `settled` and `not_billed` rows as unbillable. These fixtures drive the
   * real exported lifecycle to check the identity boundary: a live pending row
   * must stay reusable, while a consumed identity must not be re-admitted for new
   * paid work that settlement would then refuse to record.
   */

  const std::string PAID_KEY_ID = "oss-accounting-paid-key";
  const std::string PAID_MODEL = "gpt-5.6-luna";

  ledger_state::AdmissionInput paidAdmissionInput(const std::string& requestId, double nowMs) {
    ledger_state::AdmissionInput input;
    input.keyId = PAID_KEY_ID;
    input.requestId = requestId;
    input.createdAtMs = nowMs;
    input.policyVersion = "3600000:1";
    input.limitMicrocredits = 1'000;
    input.maximumExposureMicrocredits = 100;
    input.initialSettledMicrocredits = 0;
    input.quotaPerCredit = 1'000'000;
    input.windowResetAtMs = nowMs + 60 * 60'000;
    input.model = PAID_MODEL;
    input.route = "responses";
    input.path = "/v1/responses";
    input.stream = false;
    input.reasoning = std::nullopt;
    return input;
  }

  ledger_settlement::PaidFallbackUsageSettlementV3 surplusSettlement(const std::string& requestId, double providerQuota) {
    return ledger_settlement::PaidFallbackUsageSettlementV3{
      .settlement_request_id = requestId,
      .provider_quota = providerQuota,
      .input_tokens = 10,
      .cached_input_tokens = {},
      .output_tokens = 5,
      .model = PAID_MODEL,
      .created_at_ms = rt::dateNow(),
      .buyer_cost_micro = std::nullopt,
    };
  }

  json::value paidLedgerRow(const CountingKv& kv, const std::string& requestId) {
    return storedValue(kv, ledger_state::paidFallbackRequestV3Key(PAID_KEY_ID, requestId));
  }

  PaidFallbackRequestLifecyclePatchV3 dispatchedSurplusPatch(const std::string& providerRequestId) {
    PaidFallbackRequestLifecyclePatchV3 patch;
    patch.provider = "surplus";
    patch.provider_request_id = providerRequestId;
    patch.dispatch_state = "dispatched";
    return patch;
  }

  // `Readonly<{ kind: "reserved" | "blocked"; recorded: boolean | null }>`
  struct Readmission {
    std::string kind;
    std::optional<bool> recorded;
  };

  /** Re-admits a consumed identity and reports whether settlement recorded the new paid work. */
  rt::Task<Readmission> readmitAndSettle(std::string requestId, double nowMs) {
    rt::Task<ledger_admission::AdmitPaidFallbackV3Result> admitting = ledger_admission::admitPaidFallbackV3(paidAdmissionInput(requestId, nowMs));
    const ledger_admission::AdmitPaidFallbackV3Result readmission = co_await std::move(admitting);
    const auto* reserved = std::get_if<ledger_admission::AdmitPaidFallbackV3ResultReserved>(&readmission);
    if (reserved == nullptr) co_return Readmission{.kind = "blocked", .recorded = std::nullopt};
    rt::Task<void> updating = ledger_admission::updatePaidFallbackRequestV3(reserved->reservation, dispatchedSurplusPatch(requestId + "-second-upstream"));
    co_await std::move(updating);
    rt::Task<bool> settling = ledger_settlement::settlePaidFallbackUsageV3(reserved->reservation, surplusSettlement(requestId + "-second-settlement", 5));
    const bool recorded = co_await std::move(settling);
    co_return Readmission{.kind = "reserved", .recorded = recorded};
  }

  // Lets the background backfill admitPaidFallbackV3 scheduled finish on this test's loop.
  rt::Task<void> settleScheduledBackfill() {
    while (const std::optional<rt::Promise<void>> scheduled = ledger_backfill::scheduledPaidFallbackBackfillForTest()) {
      co_await scheduled->wait();
    }
  }
}

ASYNC_TEST("a released request identity is refused at admission instead of admitted then failed") {
  const Setup setup = setupPolicy("released-identity", 2);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservationDecision> reservingFirst = reserve(kv, policy, "reused-request");
  const ApiKeyUsageReservationDecision firstDecision = co_await std::move(reservingFirst);
  const ApiKeyUsageReservation first = admitted(firstDecision);
  rt::Task<void> releasing = releaseWith(first, std::string("validation_failed"));
  co_await std::move(releasing);
  EXPECT(sameJson(accountingFor(*kv, policy, "reused-request"), R"({"committed":0,"reserved":0,"state":"released","reason":"validation_failed"})"));

  rt::Task<ApiKeyUsageReservationDecision> replaying = reserve(kv, policy, "reused-request");
  const ApiKeyUsageReservationDecision replayed = co_await std::move(replaying);
  if (const auto* ok = std::get_if<ApiKeyUsageReservationDecisionOk>(&replayed)) {
    // Today this admission reports success and the request is only refused later,
    // inside provider dispatch, after the request was already accepted.
    rt::Reason dispatchFailure;
    try {
      rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = dispatchFor(ok->reservation, "cerebras");
      co_await std::move(dispatching);
    } catch (...) {
      dispatchFailure = std::current_exception();
    }
    throw std::runtime_error("a spent request identity must not be re-admitted (dispatch failed with " + rt::reasonToString(dispatchFailure) + ")");
  }
  // a spent identity is refused as an unavailable reservation
  EXPECT_EQ(std::get<ApiKeyUsageReservationDecisionNotOk>(replayed).response.status(), 503);
  // The refusal is scoped to the spent identity: fresh work is still admitted.
  rt::Task<ApiKeyUsageReservationDecision> reservingFresh = reserve(kv, policy, "fresh-request");
  const ApiKeyUsageReservationDecision freshDecision = co_await std::move(reservingFresh);
  const ApiKeyUsageReservation fresh = admitted(freshDecision);
  rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = dispatchFor(fresh, "cerebras");
  const std::optional<ApiKeyProviderDispatch> dispatch = co_await std::move(dispatching);
  if (!dispatch) throw std::runtime_error("fresh work must still reserve and dispatch");
  dispatch->markTransportStarted();
  assertLedgerConsistent(*kv, policy, "fresh-request", 2);
  EXPECT_EQ(numberAt(windowFor(*kv, policy), "committed_requests"), 1.0);
}

ASYNC_TEST("a dispatched request identity is never re-admitted without its own charge") {
  const Setup setup = setupPolicy("dispatched-identity", 2);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservationDecision> reservingFirst = reserve(kv, policy, "shared-request");
  const ApiKeyUsageReservationDecision firstDecision = co_await std::move(reservingFirst);
  const ApiKeyUsageReservation first = admitted(firstDecision);
  rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = dispatchFor(first, "surplus");
  const std::optional<ApiKeyProviderDispatch> firstDispatch = co_await std::move(dispatching);
  if (!firstDispatch) throw std::runtime_error("the first reservation must commit its dispatch");
  firstDispatch->markTransportStarted();
  EXPECT(sameJson(accountingFor(*kv, policy, "shared-request"), R"({"committed":1,"reserved":0,"state":"dispatched","reason":null})"));

  rt::Task<ApiKeyUsageReservationDecision> readmitting = reserve(kv, policy, "shared-request");
  const ApiKeyUsageReservationDecision readmission = co_await std::move(readmitting);
  std::optional<ApiKeyProviderDispatch> secondContext;
  rt::Reason refusal;
  const auto* readmitted = std::get_if<ApiKeyUsageReservationDecisionOk>(&readmission);
  if (readmitted != nullptr) {
    try {
      rt::Task<std::optional<ApiKeyProviderDispatch>> secondDispatching = dispatchFor(readmitted->reservation, "surplus");
      secondContext = co_await std::move(secondDispatching);
    } catch (...) {
      refusal = std::current_exception();
    }
  }
  const bool transportsWithoutOwnCharge = readmitted != nullptr && !secondContext && !refusal;
  // a re-presented consumed identity must be refused or must commit its own charge before provider transport
  EXPECT_EQ(transportsWithoutOwnCharge, false);
  assertLedgerConsistent(*kv, policy, "shared-request", 2);
}

ASYNC_TEST("concurrent cancellation and dispatch settlement agree on one outcome") {
  const Setup setup = setupPolicy("cancel-settlement-race", 1);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservationDecision> reserving = reserve(kv, policy, "race-request");
  const ApiKeyUsageReservationDecision reservationDecision = co_await std::move(reserving);
  const ApiKeyUsageReservation reservation = admitted(reservationDecision);
  // `Promise.allSettled([reservation.beforeProviderDispatch("deepseek"), reservation.release("client_cancelled")])`
  const rt::Promise<std::optional<ApiKeyProviderDispatch>> dispatchPromise(reservation.beforeProviderDispatch("deepseek"));
  const rt::Promise<void> releasePromise(reservation.release(std::string("client_cancelled")));
  std::vector<rt::Promise<std::optional<ApiKeyProviderDispatch>>> dispatches{dispatchPromise};
  std::vector<rt::Promise<void>> releases{releasePromise};
  auto settledDispatch = rt::allSettled(std::move(dispatches));
  auto settledRelease = rt::allSettled(std::move(releases));
  const std::vector<rt::SettledResult<std::optional<ApiKeyProviderDispatch>>> dispatchOutcomes = co_await settledDispatch.wait();
  const std::vector<rt::SettledResult<void>> releaseOutcomes = co_await settledRelease.wait();
  const rt::SettledResult<std::optional<ApiKeyProviderDispatch>>& dispatchOutcome = dispatchOutcomes.at(0);
  // completion must settle without error
  EXPECT_EQ(releaseOutcomes.at(0).status, "fulfilled");
  const json::object accounting = accountingFor(*kv, policy, "race-request");
  assertLedgerConsistent(*kv, policy, "race-request", 1);
  if (sameJson(*rt::get(accounting, "state"), R"("dispatched")")) {
    // the winning dispatch must return a transport context
    EXPECT(dispatchOutcome.status == "fulfilled");
    // a committed dispatch must be compensatable by its caller
    EXPECT(dispatchOutcome.value.has_value() && dispatchOutcome.value->has_value());
  } else {
    // the race must end in a settled row
    EXPECT(sameJson(*rt::get(accounting, "state"), R"("released")"));
    // a release that won the race must leave no dispatch context
    EXPECT_EQ(dispatchOutcome.status, "rejected");
  }
}

ASYNC_TEST("duplicate pre-transport cancellation callbacks refund exactly once") {
  const Setup setup = setupPolicy("duplicate-cancellation", 1);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservationDecision> reserving = reserve(kv, policy, "duplicate-cancel-request");
  const ApiKeyUsageReservationDecision reservationDecision = co_await std::move(reserving);
  const ApiKeyUsageReservation reservation = admitted(reservationDecision);
  rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = dispatchFor(reservation, "cerebras");
  const std::optional<ApiKeyProviderDispatch> dispatch = co_await std::move(dispatching);
  if (!dispatch) throw std::runtime_error("the reservation must commit its dispatch");

  std::vector<rt::Promise<void>> cancellations;
  cancellations.push_back(rt::Promise<void>(dispatch->cancelBeforeTransport()));
  cancellations.push_back(rt::Promise<void>(dispatch->cancelBeforeTransport()));
  auto all = rt::all(std::move(cancellations));
  co_await all.wait();

  EXPECT(sameJson(accountingFor(*kv, policy, "duplicate-cancel-request"),
                  R"({"committed":0,"reserved":0,"state":"released","reason":"transport_cancelled_before_fetch"})"));
  assertLedgerConsistent(*kv, policy, "duplicate-cancel-request", 1);
}

ASYNC_TEST("accepted upstream work is not refunded by a later completion callback") {
  const Setup setup = setupPolicy("accepted-not-refunded", 1);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservationDecision> reserving = reserve(kv, policy, "accepted-request");
  const ApiKeyUsageReservationDecision reservationDecision = co_await std::move(reserving);
  const ApiKeyUsageReservation reservation = admitted(reservationDecision);
  rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = dispatchFor(reservation, "chatgpt_codex");
  const std::optional<ApiKeyProviderDispatch> dispatch = co_await std::move(dispatching);
  if (!dispatch) throw std::runtime_error("the reservation must commit its dispatch");
  dispatch->markTransportStarted();

  rt::Task<void> cancelling = dispatch->cancelBeforeTransport();
  co_await std::move(cancelling);
  rt::Task<void> releasingOnce = releaseWith(reservation, std::string("provider_http_failure"));
  co_await std::move(releasingOnce);
  rt::Task<void> releasingTwice = releaseWith(reservation, std::string("provider_http_failure"));
  co_await std::move(releasingTwice);

  EXPECT(sameJson(accountingFor(*kv, policy, "accepted-request"), R"({"committed":1,"reserved":0,"state":"dispatched","reason":null})"));
  assertLedgerConsistent(*kv, policy, "accepted-request", 1);
}

ASYNC_TEST("a reclaimed expired lease returns its slot once and never rides free later") {
  const double baseMs = rt::dateNow();
  const Setup setup = setupPolicy("lease-expiry-late-dispatch", 1, baseMs);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservationDecision> reservingAbandoned = reserve(kv, policy, "abandoned-request", baseMs, ROUTE);
  const ApiKeyUsageReservationDecision abandonedDecision = co_await std::move(reservingAbandoned);
  const ApiKeyUsageReservation abandoned = admitted(abandonedDecision);
  rt::Task<ApiKeyUsageReservationDecision> reservingReplacement = reserve(kv, policy, "replacement-request", baseMs + API_KEY_USAGE_V3_RESERVATION_LEASE_MS + 1, ROUTE);
  const ApiKeyUsageReservationDecision replacementDecision = co_await std::move(reservingReplacement);
  const ApiKeyUsageReservation replacement = admitted(replacementDecision);
  // The window aggregate and the reclaimed row are separate facts: the one live
  // slot belongs to the replacement, while the reclaimed identity's own row must
  // be terminal and uncharged.
  const json::value abandonedRow = requestFor(*kv, policy, "abandoned-request");
  json::object observed;
  observed["state"] = rt::get(abandonedRow, "state") ? *rt::get(abandonedRow, "state") : json::value(nullptr);
  observed["reason"] = rt::get(abandonedRow, "release_reason") ? *rt::get(abandonedRow, "release_reason") : json::value(nullptr);
  observed["committed"] = *rt::get(windowFor(*kv, policy), "committed_requests");
  // the reclaimed identity's own row must be released without charging it
  EXPECT(sameJson(observed, R"({"state":"released","reason":"lease_expired","committed":0})"));
  // exactly one live reservation remains, and it is the replacement's, not the reclaimed identity's
  EXPECT_EQ(numberAt(windowFor(*kv, policy), "reserved_requests"), 1.0);

  std::optional<ApiKeyProviderDispatch> lateDispatch;
  rt::Reason lateRefusal;
  try {
    rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = dispatchFor(abandoned, "chatgpt_codex");
    lateDispatch = co_await std::move(dispatching);
  } catch (...) {
    lateRefusal = std::current_exception();
  }
  // a reclaimed identity must either re-reserve its own charge or fail closed, never dispatch uncharged
  EXPECT(isQuotaDispatchError(lateRefusal) || lateDispatch.has_value());
  assertLedgerConsistent(*kv, policy, "abandoned-request", 1);
  assertLedgerConsistent(*kv, policy, "replacement-request", 1);

  rt::Task<void> releasing = releaseWith(replacement, std::string("validation_failed"));
  co_await std::move(releasing);
  EXPECT(sameJson(accountingFor(*kv, policy, "replacement-request"), R"({"committed":0,"reserved":0,"state":"released","reason":"validation_failed"})"));
}

ASYNC_TEST("a request id reused on another route cannot share the first route's charge") {
  const Setup setup = setupPolicy("route-scoped-identity", 2);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservationDecision> reservingFirst = reserve(kv, policy, "cross-route-id", std::nullopt, "chat.completions");
  const ApiKeyUsageReservationDecision firstDecision = co_await std::move(reservingFirst);
  const ApiKeyUsageReservation first = admitted(firstDecision);

  rt::Task<ApiKeyUsageReservationDecision> reservingOther = reserve(kv, policy, "cross-route-id", std::nullopt, "embeddings");
  const ApiKeyUsageReservationDecision otherRoute = co_await std::move(reservingOther);
  const auto* refused = std::get_if<ApiKeyUsageReservationDecisionNotOk>(&otherRoute);
  // one request id must not span two routes
  EXPECT(refused != nullptr);
  if (refused != nullptr) EXPECT_EQ(refused->response.status(), 503);
  EXPECT(sameJson(accountingFor(*kv, policy, "cross-route-id"), R"({"committed":0,"reserved":1,"state":"reserved","reason":null})"));

  rt::Task<void> releasing = releaseWith(first, std::string("validation_failed"));
  co_await std::move(releasing);
  assertLedgerConsistent(*kv, policy, "cross-route-id", 2);
}

ASYNC_TEST("a live duplicate admission still shares one reservation and one committed request") {
  const Setup setup = setupPolicy("live-idempotency", 1);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservationDecision> reservingFirst = reserve(kv, policy, "live-request");
  const ApiKeyUsageReservationDecision firstDecision = co_await std::move(reservingFirst);
  const ApiKeyUsageReservation first = admitted(firstDecision);
  rt::Task<ApiKeyUsageReservationDecision> reservingDuplicate = reserve(kv, policy, "live-request");
  const ApiKeyUsageReservationDecision duplicateDecision = co_await std::move(reservingDuplicate);
  const ApiKeyUsageReservation duplicate = admitted(duplicateDecision);
  // a duplicate admission must not reserve twice
  EXPECT_EQ(numberAt(windowFor(*kv, policy), "reserved_requests"), 1.0);

  rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = dispatchFor(duplicate, "metered");
  const std::optional<ApiKeyProviderDispatch> dispatch = co_await std::move(dispatching);
  if (!dispatch) throw std::runtime_error("a live duplicate admission must dispatch the one durable row");
  dispatch->markTransportStarted();
  rt::Task<std::optional<ApiKeyProviderDispatch>> retrying = dispatchFor(duplicate, "surplus");
  const std::optional<ApiKeyProviderDispatch> retry = co_await std::move(retrying);
  // a retry of one reservation must not commit a second dispatch
  EXPECT(!retry.has_value());

  rt::Task<void> releasing = releaseWith(first, std::nullopt);
  co_await std::move(releasing);
  EXPECT(sameJson(accountingFor(*kv, policy, "live-request"), R"({"committed":1,"reserved":0,"state":"dispatched","reason":null})"));
  assertLedgerConsistent(*kv, policy, "live-request", 1);
}

ASYNC_TEST("a settled paid-fallback identity is not re-admitted as unrecorded new paid work") {
  const auto kv = std::make_shared<CountingKv>();
  uos::kv::setKvForTest(kv);
  rt::Reason failure;
  try {
    const double nowMs = rt::dateNow();
    rt::Task<ledger_admission::AdmitPaidFallbackV3Result> admittingFirst = ledger_admission::admitPaidFallbackV3(paidAdmissionInput("paid-settled-request", nowMs));
    const ledger_admission::AdmitPaidFallbackV3Result first = co_await std::move(admittingFirst);
    const auto* firstReserved = std::get_if<ledger_admission::AdmitPaidFallbackV3ResultReserved>(&first);
    if (firstReserved == nullptr) throw std::runtime_error("the first paid admission must reserve");

    rt::Task<ledger_admission::AdmitPaidFallbackV3Result> admittingDuplicate = ledger_admission::admitPaidFallbackV3(paidAdmissionInput("paid-settled-request", nowMs));
    const ledger_admission::AdmitPaidFallbackV3Result duplicate = co_await std::move(admittingDuplicate);
    // a live pending identity must remain reusable
    EXPECT(std::holds_alternative<ledger_admission::AdmitPaidFallbackV3ResultReserved>(duplicate));
    const json::value window = storedValue(*kv, ledger_state::paidFallbackWindowV3Key(PAID_KEY_ID, nowMs + 60 * 60'000));
    // a duplicate paid admission must not reserve exposure twice
    EXPECT_EQ(numberAt(window, "reserved_microcredits"), 100.0);

    rt::Task<void> updating = ledger_admission::updatePaidFallbackRequestV3(firstReserved->reservation, dispatchedSurplusPatch("paid-settled-upstream"));
    co_await std::move(updating);
    rt::Task<bool> settling = ledger_settlement::settlePaidFallbackUsageV3(firstReserved->reservation, surplusSettlement("paid-settled-settlement", 5));
    const bool recorded = co_await std::move(settling);
    // the first settlement must record its usage
    EXPECT_EQ(recorded, true);
    // the ledger must record the settled billing state
    EXPECT(sameJson(*rt::get(paidLedgerRow(*kv, "paid-settled-request"), "billing_state"), R"("settled")"));

    rt::Task<Readmission> replaying = readmitAndSettle("paid-settled-request", nowMs + 60 * 60'000);
    const Readmission replayed = co_await std::move(replaying);
    // a settled identity must not be re-admitted for new paid work
    EXPECT(!replayed.recorded.has_value());
  } catch (...) {
    failure = std::current_exception();
  }
  rt::Task<void> settlingBackfill = settleScheduledBackfill();
  co_await std::move(settlingBackfill);
  uos::kv::setKvForTest(nullptr);
  if (failure) std::rethrow_exception(failure);
}

ASYNC_TEST("a not-billed paid-fallback identity is not re-admitted as unrecorded new paid work") {
  const auto kv = std::make_shared<CountingKv>();
  uos::kv::setKvForTest(kv);
  rt::Reason failure;
  try {
    const double nowMs = rt::dateNow();
    rt::Task<ledger_admission::AdmitPaidFallbackV3Result> admittingFirst = ledger_admission::admitPaidFallbackV3(paidAdmissionInput("paid-not-billed-request", nowMs));
    const ledger_admission::AdmitPaidFallbackV3Result first = co_await std::move(admittingFirst);
    const auto* firstReserved = std::get_if<ledger_admission::AdmitPaidFallbackV3ResultReserved>(&first);
    if (firstReserved == nullptr) throw std::runtime_error("the first paid admission must reserve");

    rt::Task<void> releasing = ledger_admission::releaseUndispatchedPaidFallbackV3(firstReserved->reservation);
    co_await std::move(releasing);
    // an undispatched release must record the identity as unbilled
    EXPECT(sameJson(*rt::get(paidLedgerRow(*kv, "paid-not-billed-request"), "billing_state"), R"("not_billed")"));

    rt::Task<Readmission> replaying = readmitAndSettle("paid-not-billed-request", nowMs + 60 * 60'000);
    const Readmission replayed = co_await std::move(replaying);
    // a not-billed identity must not be re-admitted for new paid work
    EXPECT(!replayed.recorded.has_value());
  } catch (...) {
    failure = std::current_exception();
  }
  rt::Task<void> settlingBackfill = settleScheduledBackfill();
  co_await std::move(settlingBackfill);
  uos::kv::setKvForTest(nullptr);
  if (failure) std::rethrow_exception(failure);
}
