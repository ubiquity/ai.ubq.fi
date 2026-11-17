#include "uos/paid_fallback/rollups.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <optional>
#include <string>
#include <utility>

namespace uos::paid_fallback::rollups {
  namespace {
    constexpr rt::StoredJsonViewOptions STORED_JSON_VIEW_OPTIONS{
      .undefinedMember = rt::StoredJsonViewOptions::UndefinedView::emptyObject,
      .undefinedElement = rt::StoredJsonViewOptions::UndefinedView::emptyObject,
      .hole = rt::StoredJsonViewOptions::UndefinedView::emptyObject,
      .topLevelUndefined = rt::StoredJsonViewOptions::UndefinedView::emptyObject,
      .bigint = rt::StoredJsonViewOptions::BigIntView::emptyObject,
      .kvU64Members = false,
      .typedArrayMembers = false,
    };
    using ::uos::authority::native_paid_contracts::NativePaidTerminalUsage;
    using ::uos::authority::native_paid_contracts::PaidFallbackUsageRollupInput;

    constexpr double MAX_TERMINAL_USAGE_ROLLUP_WRITE_ATTEMPTS = 3;

    // A stored value as isPaidFallbackUsageRollup observes it, in JSON: what JSON can hold is kept;
    // every other part (an undefined member or element, a hole, a bigint, KvU64, Date, Map, buffer,
    // ...) becomes `{}`, which fails each of the guard's checks exactly as the original does.

    // `isPaidFallbackUsageRollup(entry.value) ? entry.value : null`
    std::optional<PaidFallbackUsageRollup> storedRollup(const rt::KvValue& value) {
      const json::value view = rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS);
      if (!isPaidFallbackUsageRollup(view)) return std::nullopt;
      return json::value_to<PaidFallbackUsageRollup>(view);
    }

    rt::Task<bool> recordTerminalUsageRollupWith(TerminalUsageRollupInput input, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
      const std::optional<NativePaidTerminalUsage> usage = ::uos::authority::native_paid::paidFallbackTerminalUsageV3(input);
      if (!usage) co_return false;
      const std::string model = usage->model;
      const std::string provider = usage->provider;
      std::shared_ptr<rt::Kv> kv;
      if (!kvOverride) {
        const rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
        kv = co_await opening.wait();
      } else {
        kv = *kvOverride;
      }
      if (!kv) co_return false;
      const std::string trimmed = rt::trim(input.request_id);
      const std::string requestId = trimmed.empty() ? rt::crypto::randomUUID() : trimmed;
      const double nowMs = rt::dateNow();
      const PaidFallbackUsageRollupInput rollupInput = ::uos::authority::native_paid::paidFallbackTerminalRollupV3(input, *usage, requestId, nowMs);
      const rt::KvKey rollupKey = paidFallbackUsageRollupKey(rollupInput.bucket_start_at_ms, model, provider, paidFallbackUsageRollupShard(requestId));
      for (double attempt = 0; attempt < MAX_TERMINAL_USAGE_ROLLUP_WRITE_ATTEMPTS; attempt += 1) {
        const rt::KvGetOptions options{.consistency = rt::KvConsistencyLevel::strong};
        auto reading = kv->get(rollupKey, options);
        const rt::KvEntryMaybe<> entry = co_await std::move(reading);
        const std::optional<PaidFallbackUsageRollup> existing = storedRollup(entry.value);
        const PaidFallbackUsageRollup next = mergePaidFallbackUsageRollup(existing, rollupInput);
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).set(rollupKey, rt::KvValue(json::value_from(next)));
        auto committing = operation->commit();
        const rt::KvCommitResult commit = co_await std::move(committing);
        if (commit.ok) co_return true;
      }
      co_return false;
    }
  }

  rt::Task<bool> recordTerminalUsageRollup(TerminalUsageRollupInput input) { return recordTerminalUsageRollupWith(std::move(input), std::nullopt); }

  rt::Task<bool> recordTerminalUsageRollup(TerminalUsageRollupInput input, std::shared_ptr<rt::Kv> kvOverride) {
    return recordTerminalUsageRollupWith(std::move(input), std::optional<std::shared_ptr<rt::Kv>>(std::move(kvOverride)));
  }

  rt::Task<std::vector<PaidFallbackUsageRollup>> listPaidFallbackUsageRollups(std::shared_ptr<rt::Kv> kv, ListPaidFallbackUsageRollupsOptions options) {
    std::vector<PaidFallbackUsageRollup> entries;
    if (!kv) co_return entries;
    // Floor the scan start to the hour boundary so the bucket that contains the
    // window start is never excluded (bucket-level precision is documented).
    const double startBucket =
      rt::Math::floor(rt::Math::max(0.0, rt::Math::trunc(options.sinceMs)) / PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS) * PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS;
    const double endBucket = rt::Math::trunc(options.nowMs) + PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS;
    // Deno KV rejects selectors that combine a prefix with both range bounds.
    // These bounds already include the complete rollup namespace.
    rt::KvListSelector selector;
    selector.start = rt::KvKey{PAID_FALLBACK_USAGE_ROLLUP_PREFIX, startBucket};
    selector.end = rt::KvKey{PAID_FALLBACK_USAGE_ROLLUP_PREFIX, endBucket};
    std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector);
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      if (std::optional<PaidFallbackUsageRollup> rollup = storedRollup(entry->value)) entries.push_back(std::move(*rollup));
    }
    co_return entries;
  }
}
