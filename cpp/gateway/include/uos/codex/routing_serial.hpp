#pragma once
// Port of src/codex/routing-serial.ts.
// Codex serial admission selection, split out of src/codex_account_routing.ts.
//
// withCodexAdmission is generic in the TS. The template is defined in src/codex/routing_serial.cpp
// and explicitly instantiated there for every T the port uses (the `extern template` lines below);
// a new T needs a new instantiation there.
#include "uos/codex/routing_evaluation.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::codex::routing_serial {
  // `transitionReason: CodexActiveAccountTransitionReason | null`; the result is null as std::nullopt.
  std::optional<routing_state::CodexActiveAccountSelection> activeSelectionForAccount(
    const routing_state::RoutingAccount& account, const std::string& poolVersionstamp, double generation, double now,
    const std::optional<routing_state::CodexActiveAccountTransitionReason>& transitionReason);

  // `type SerialRoutingEvaluations` (module-private in the TS): ReadonlyMaps keyed by raw account id.
  struct SerialRoutingEvaluations {
    routing_evaluation::CodexRoutingAccumulation accumulated;
    rt::OrderedMap<std::string, routing_evaluation::CodexRoutingAccountEvaluation> byAccountId;
    rt::OrderedMap<std::string, routing_evaluation::CodexRoutingSlotIdentity> identityByAccountId;
  };

  /** Evaluate every configured account, preserving pool order without headroom ranking. */
  // `model: string | null`; `slotPool = pool` is the overload without it.
  [[nodiscard]] rt::Task<SerialRoutingEvaluations> evaluateSerialRoutingAccounts(routing_state::CodexAccountRoutingState state, ::uos::types::CodexAuthPoolState pool,
                                                                               std::optional<std::string> model,
                                                                               std::vector<routing_state::CodexCapacityRoutingObservation> capacityObservations,
                                                                               double now);
  [[nodiscard]] rt::Task<SerialRoutingEvaluations> evaluateSerialRoutingAccounts(routing_state::CodexAccountRoutingState state, ::uos::types::CodexAuthPoolState pool,
                                                                               std::optional<std::string> model,
                                                                               std::vector<routing_state::CodexCapacityRoutingObservation> capacityObservations,
                                                                               double now, ::uos::types::CodexAuthPoolState slotPool);

  // The value readStrongCodexRoutingRows returns (inferred in the TS): the four entries of one strong
  // getMany. Every reader parses the rows as `unknown` (the TS getMany types the routing row but
  // nothing trusts that), so each entry holds the stored value as JSON, null when absent.
  struct ReadStrongCodexRoutingRowsResult {
    rt::KvEntryMaybe<json::value> activeEntry;
    rt::KvEntryMaybe<json::value> authEntry;
    rt::KvEntryMaybe<json::value> routingEntry;
    rt::KvEntryMaybe<json::value> capacityEntry;
  };

  /** One coherent strong snapshot of the four rows every admission decision linearizes on, in a fixed order. */
  [[nodiscard]] rt::Task<ReadStrongCodexRoutingRowsResult> readStrongCodexRoutingRows(std::shared_ptr<rt::Kv> kv);

  /**
   * The configured subscriptions the operator has left eligible for routing.
   * Durable slots keep tracking every configured account — the routing state is
   * passed the full pool — so re-enabling a subscription resumes its recorded
   * quota history instead of rebuilding it. Only the accounts an admission may
   * select are narrowed here.
   *
   * Narrowing the pool is itself the operator's decision, so a reduced cohort is
   * the cohort: "every account is exhausted" and the banked-reset cohort both
   * describe the subscriptions that are still switched on.
   */
  [[nodiscard]] rt::Task<::uos::types::CodexAuthPoolState> selectedCodexSubscriptionPool(::uos::types::CodexAuthPoolState pool);

  /**
   * Run one short routing-admission action under the per-KV FIFO tail. This
   * caller's release-only promise is installed before it waits, and is always
   * released, so a failure can never poison later callers.
   */
  // `<T>(kv: Kv, action: () => Promise<T>): Promise<T>`
  template <class T>
  [[nodiscard]] rt::Task<T> withCodexAdmission(std::shared_ptr<rt::Kv> kv, std::function<rt::Task<T>()> action);
  extern template rt::Task<routing_state::RouteSelection> withCodexAdmission<routing_state::RouteSelection>(
    std::shared_ptr<rt::Kv> kv, std::function<rt::Task<routing_state::RouteSelection>()> action);
  extern template rt::Task<bool> withCodexAdmission<bool>(std::shared_ptr<rt::Kv> kv, std::function<rt::Task<bool>()> action);

  /**
   * Every ordinary admission strongly reads the active selection, auth pool,
   * and routing state. A missing active row is the one bootstrap case; any
   * malformed or unavailable durable state fails before upstream dispatch.
   */
  // `model: string | null`. The first two parameters are unused, as in the TS.
  [[nodiscard]] rt::Task<routing_state::RouteSelection> selectSerialCodexRoutingAccounts(::uos::types::CodexAuthPoolState _suppliedPool,
                                                                                       std::vector<::uos::types::CodexAuthState> _orderedAccounts, double now,
                                                                                       std::optional<std::string> model);
}
