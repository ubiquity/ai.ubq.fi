#pragma once
// Port of src/authority/native-codex-capacity.ts.
// Source-owned capacity/normalization decisions. All clocks and identity metadata are injected.
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace uos::authority::native_codex_capacity {

  // `Readonly<{ accountIdHash: string; credentialVersion: string }>`: a CodexRoutingIdentity also fits.
  struct CodexRoutingAccountIdentity {
    std::string accountIdHash = {};
    std::string credentialVersion = {};
  };

  // `accountIdHash: string | null`
  native_codex_contracts::CodexRoutingSlot neutralSlot(std::string credentialVersion, std::optional<std::string> accountIdHash);

  native_codex_contracts::CodexAccountRoutingState withoutLegacyTimeoutCircuits(const native_codex_contracts::CodexAccountRoutingState& state);

  /**
   * A token refresh stays in the same provider quota scope. Release ordinary
   * routing for the refreshed credential, but keep any pre-refresh stable reset
   * observation lookup-only until a successful recovery probe proves the
   * account recovered.
   */
  native_codex_contracts::CodexRoutingSlot rotateCredentialForSameAccount(const native_codex_contracts::CodexRoutingSlot& slot, const CodexRoutingAccountIdentity& identity);
  native_codex_contracts::CodexRoutingSlot rotateCredentialForSameAccount(const native_codex_contracts::CodexRoutingSlot& slot, const native_codex_contracts::CodexRoutingIdentity& identity);

  // `raw: native_codex_contracts::CodexAccountRoutingState | null`: nullptr is null.
  native_codex_contracts::CodexAccountRoutingState normalizeRoutingState(const native_codex_contracts::CodexAccountRoutingState* raw, const std::vector<native_codex_contracts::CodexRoutingIdentity>& identities, double now,
                                                 bool discardLegacyTimeoutCircuit = false);

  native_codex_contracts::CodexRoutingSlot slotFor(const native_codex_contracts::CodexAccountRoutingState& state, const native_codex_contracts::CodexRoutingAccount& account);

  bool slotMatchesCodexRoutingAccount(const native_codex_contracts::CodexRoutingSlot& slot, const native_codex_contracts::CodexRoutingAccount& account);

  native_codex_contracts::CodexProbeCircuit routingProbeCircuit(const native_codex_contracts::CodexRoutingAccount& account);

  /**
   * A lease matches only when its token, generation, circuit and (for the quota
   * circuit) model class all belong to this account's claim.
   */
  bool probeLeaseMatchesCodexRoutingAccount(const native_codex_contracts::CodexRoutingSlot& slot, const native_codex_contracts::CodexRoutingAccount& account);

  native_codex_contracts::CodexAccountRoutingState withSlot(const native_codex_contracts::CodexAccountRoutingState& state, double index, const native_codex_contracts::CodexRoutingSlot& slot, double updatedAtMs);

  // `value: string | null`
  std::optional<double> parseFinitePercent(const std::optional<std::string>& value);

  std::optional<double> quotaHeadroomFor(const native_codex_contracts::CodexRoutingSlot& slot);

  // `value: unknown`: nullptr is undefined.
  std::optional<std::string> capacityState(const json::value* value);

  std::optional<native_codex_contracts::CodexCapacityRoutingWindow> normalizeCapacityWindow(const json::value* value);

  std::vector<native_codex_contracts::CodexCapacityRoutingAdditionalRateLimit> normalizeCapacityAdditionalRateLimits(const json::value* value);

  // `model: string | null | undefined`
  native_codex_contracts::CodexQuotaClass quotaClass(std::optional<std::string_view> model);
  native_codex_contracts::CodexQuotaClass quotaClass(const rt::Nullable<std::string>& model);

  std::optional<native_codex_contracts::CodexQuotaClassBlock> quotaBlockForClass(const native_codex_contracts::CodexRoutingSlot& slot, std::string_view quotaClassKey);

  native_codex_contracts::CodexQuotaBlocksByClass quotaBlocksIncludingLegacy(const native_codex_contracts::CodexRoutingSlot& slot);

  native_codex_contracts::CodexRoutingSlot withLegacyQuotaClassMap(const native_codex_contracts::CodexRoutingSlot& slot);

  std::optional<double> quotaSignalObservedAtForClass(const native_codex_contracts::CodexRoutingSlot& slot, std::string_view quotaClassKey);

  std::optional<native_codex_contracts::CodexQuotaClass> quotaBlockKeyForClass(const native_codex_contracts::CodexRoutingSlot& slot, std::string_view quotaClassKey);

  native_codex_contracts::CodexRoutingSlot withoutQuotaClass(const native_codex_contracts::CodexRoutingSlot& slot, std::string_view quotaClassKey);

  native_codex_contracts::CodexRoutingSlot recheckQuotaClasses(const native_codex_contracts::CodexRoutingSlot& slot, double recheckAtMs);

  native_codex_contracts::CodexRoutingSlot releaseQuotaClassProbe(const native_codex_contracts::CodexRoutingSlot& slot, std::string_view quotaClassKey);

  // `model: string | null`
  std::optional<double> capacityHeadroomForObservation(const native_codex_contracts::CodexCapacityRoutingObservation& observation, std::optional<std::string_view> model);

  bool capacityObservationIsFresh(const native_codex_contracts::CodexCapacityRoutingObservation& observation, double now);

  // The provider deadline that identifies a fresh, exhausted model-applicable window.
  std::optional<double> capacityExhaustedResetAtMs(const native_codex_contracts::CodexCapacityRoutingObservation& observation,
                                                  std::optional<std::string_view> model, double now);

  bool capacityHasCurrentExhaustedWindow(const native_codex_contracts::CodexCapacityRoutingObservation& observation,
                                         std::optional<std::string_view> model, double now);
  bool capacityLimitMatchesModel(std::string_view limitName, std::string_view model);

  std::vector<native_codex_contracts::CodexCapacityRoutingObservation> parseStoredCapacityObservationStore(const json::value* value);
  std::vector<native_codex_contracts::CodexCapacityRoutingObservation> parseStoredCapacityObservationStore(const json::value& value);

  // `...sets`
  std::vector<native_codex_contracts::CodexCapacityRoutingObservation> mergeCapacityObservations(std::span<const std::vector<native_codex_contracts::CodexCapacityRoutingObservation>> sets);
  template <class... Sets>
    requires(std::is_convertible_v<const Sets&, const std::vector<native_codex_contracts::CodexCapacityRoutingObservation>&> && ...)
  std::vector<native_codex_contracts::CodexCapacityRoutingObservation> mergeCapacityObservations(const Sets&... sets) {
    const std::vector<std::vector<native_codex_contracts::CodexCapacityRoutingObservation>> list{sets...};
    return mergeCapacityObservations(std::span<const std::vector<native_codex_contracts::CodexCapacityRoutingObservation>>(list));
  }

  std::vector<native_codex_contracts::CodexCapacityRoutingObservation> retainRecentCapacityObservations(const std::vector<native_codex_contracts::CodexCapacityRoutingObservation>& observations, double now);

  /**
   * Materialize or reconcile the requested class block from a fresh, complete
   * capacity observation. Unlike a live 429, the usage read describes the
   * current provider window directly, so a newer exhausted sample may replace a
   * retained deadline whose identity became ambiguous. A live quota signal that
   * is newer than the sample remains authoritative.
   */
  std::optional<native_codex_contracts::CodexAccountRoutingState> applyCapacityQuotaBlock(const native_codex_contracts::CodexAccountRoutingState& state, const native_codex_contracts::CodexRoutingAccount& account,
                                                                  const native_codex_contracts::CodexCapacityRoutingObservation& observation, double now, double updatedAtMs);

  // `model: string | null`
  std::optional<native_codex_contracts::CodexAccountRoutingState> applyCapacityObservation(const native_codex_contracts::CodexAccountRoutingState& state, const native_codex_contracts::CodexCapacityRoutingObservation& observation,
                                                                   double now, std::optional<std::string_view> model, double updatedAtMs);

  // The value planCapacityObservationStore returns:
  // `{ kind: "capacity_observations", checks: { capacity }, writes: { capacity: { v: 1, updated_at_ms, observations } } }`.
  struct CodexCapacityObservationStore {
    double v = 1;
    double updated_at_ms = 0;
    std::vector<native_codex_contracts::CodexCapacityRoutingObservation> observations = {};
  };
  struct CodexCapacityObservationStoreChecks {
    std::optional<std::string> capacity = std::nullopt;
  };
  struct CodexCapacityObservationStoreWrites {
    CodexCapacityObservationStore capacity = {};
  };
  struct CodexCapacityObservationStorePlan {
    static constexpr std::string_view kind = "capacity_observations";
    CodexCapacityObservationStoreChecks checks = {};
    CodexCapacityObservationStoreWrites writes = {};
  };

  /** Merge/retention and the exact unexpired observation-store row under its native C-only revision fence. */
  CodexCapacityObservationStorePlan planCapacityObservationStore(std::optional<std::string> capacityRevision, const std::vector<native_codex_contracts::CodexCapacityRoutingObservation>& durable,
                                                                 const std::vector<native_codex_contracts::CodexCapacityRoutingObservation>& observations, double now);

  // The store row as JSON: `{ v: 1, updated_at_ms, observations }`.
  json::object toJson(const CodexCapacityObservationStore& store);
}
