#pragma once
// Port of src/codex/manual-reset-state.ts.
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::codex::manual_reset_state {
  /** Retained without TTL: an uncertain consume and a completed promotion must never become spendable by expiry. */
  inline const rt::KvKey CODEX_MANUAL_RESET_PREFIX{"uos_ai", "codex_manual_reset", "v1"};

  rt::KvKey intentKey(const std::string& accountIdHash);

  struct ManualIntent {
    double v = 1;
    std::string account_id_hash;
    std::string credential_version;
    double quota_reset_at_ms;
    double routing_generation;
    double created_at_ms;
    std::optional<double> promoted_at_ms;
    std::optional<double> quota_signal_at_ms;
    friend bool operator==(const ManualIntent&, const ManualIntent&) = default;
  };

  // The stored row: value_from writes the members in declaration order; value_to is strict.
  void tag_invoke(const json::value_from_tag&, json::value& out, const ManualIntent& value);
  ManualIntent tag_invoke(const json::value_to_tag<ManualIntent>&, const json::value& value);

  // `value: unknown`; std::nullopt is null. The TS returns the validated object itself, whose eight
  // members are exactly these (in its own order; the struct writes declaration order).
  std::optional<ManualIntent> parseManualResetIntent(const json::value& value);
}
