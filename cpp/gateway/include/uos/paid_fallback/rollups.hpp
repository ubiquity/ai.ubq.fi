#pragma once
// Port of src/paid-fallback/rollups.ts.
#include "uos/authority/native_paid.hpp"
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"

#include <memory>
#include <vector>

namespace uos::paid_fallback::rollups {
  // `export { ... } from "../authority/native-paid.ts"`
  using ::uos::authority::native_paid::PAID_FALLBACK_USAGE_ROLLUP_PREFIX;
  using ::uos::authority::native_paid::PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS;
  using ::uos::authority::native_paid::PAID_FALLBACK_SETTLED_PROVIDERS;
  using ::uos::authority::native_paid::paidFallbackUsageRollupShard;
  using ::uos::authority::native_paid::paidFallbackUsageRollupKey;
  using ::uos::authority::native_paid::isPaidFallbackUsageRollup;
  using ::uos::authority::native_paid::mergePaidFallbackUsageRollup;
  // `export type { PaidFallbackUsageRollup, TerminalUsageRollupInput }`
  using ::uos::authority::native_paid_contracts::PaidFallbackUsageRollup;
  using ::uos::authority::native_paid_contracts::TerminalUsageRollupInput;

  /**
   * Folds one terminal inference observation into the hourly rollup.
   *
   * This is the accounting path for traffic that never settles through the
   * paid-fallback ledger, which is the Codex subscription capacity first:
   * without it the projection would count paid spend alone. Providers the
   * settlement already writes are skipped (see
   * `PAID_FALLBACK_SETTLED_PROVIDERS`), and the aggregate labels `gateway`
   * (no upstream dispatched) and `mixed` (more than one route served an image
   * fanout) are not routes, so they are skipped too. A response that never
   * observed its input or output tokens is also skipped: the rollup carries
   * actual known usage, and a missing measurement must not become a zero.
   * Observations carry no quota or spend, so the paid balance runway is never
   * inflated.
   *
   * The write is bounded and best effort: it returns false when the identity or
   * observed usage is missing, KV is unavailable, or every read-merge-write
   * attempt loses its compare-and-set race. A terminal response is already
   * final, so callers on that path swallow failures instead of surfacing them.
   */
  // `recordTerminalUsageRollup(input, kvOverride?: Kv | null)`: the one-argument overload is an
  // undefined kvOverride (the shared KV); a null pointer is null.
  rt::Task<bool> recordTerminalUsageRollup(TerminalUsageRollupInput input);
  rt::Task<bool> recordTerminalUsageRollup(TerminalUsageRollupInput input, std::shared_ptr<rt::Kv> kvOverride);

  // Readonly<{ sinceMs: number; nowMs: number }>
  struct ListPaidFallbackUsageRollupsOptions {
    double sinceMs;
    double nowMs;
  };

  // `kv: Kv | null`: a null pointer lists nothing.
  rt::Task<std::vector<PaidFallbackUsageRollup>> listPaidFallbackUsageRollups(std::shared_ptr<rt::Kv> kv, ListPaidFallbackUsageRollupsOptions options);
}
