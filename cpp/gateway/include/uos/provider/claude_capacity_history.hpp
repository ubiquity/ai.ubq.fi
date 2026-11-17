#pragma once
// Port of src/provider/claude-capacity-history.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>; `kv: Kv | null` one whose null pointer is
// null. readClaudeCapacityHistoryPoint validates stored values, so it takes rt::KvValue (a
// json::value converts implicitly) and returns the declared members of a value it accepts.
#include "uos/provider/capacity_contract.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>

namespace uos::provider::claude_capacity_history {
  inline const rt::KvKey CLAUDE_CAPACITY_HISTORY_PREFIX{"uos_ai", "provider_capacity", "claude", "v1", "history"};

  // `(value: number | null): number | null`
  std::optional<double> claudeCapacityUsedPercent(std::optional<double> value);

  // `(value: unknown): ClaudeCapacityHistoryPoint | null`; std::nullopt is null.
  std::optional<capacity_contract::ClaudeCapacityHistoryPoint> readClaudeCapacityHistoryPoint(const rt::KvValue& value);

  /** Post-acceptance telemetry only. Never writes quota, credentials or routing state. */
  // Matches claude_code_quota::ObservationRecorder (capacity.cpp registers it at load). Throws the
  // TS's Errors ("Claude history bucket is invalid", ...).
  rt::Task<void> recordClaudeCapacityObservation(std::shared_ptr<rt::Kv> kv, claude_code_quota::ClaudeCodeAcceptedObservation event);

  rt::Task<capacity_contract::ClaudeCapacityHistoryView> readClaudeCapacityHistory(std::shared_ptr<rt::Kv> kv, double nowMs);
}
