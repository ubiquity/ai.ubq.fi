#pragma once
// Port of src/kernel/quota-v2.ts.
//
// `kv: Kv` parameters are std::shared_ptr<rt::Kv> (a reservation context keeps its store for its
// timers); `kv?: Kv | null` options are std::optional<std::shared_ptr<rt::Kv>> (std::nullopt is
// undefined, a null pointer is null). Entries are read untyped (rt::KvEntryMaybe<>): the TS
// `kv.get<T>` type arguments check nothing at run time. A stored value reaches the native-kernel
// normalizers as kernelQuotaValue(entry.value).
//
// `export * from "./quota-write.ts"` (and from quota-reservations.ts and quota-reserve.ts): those
// modules import this one, so each of their headers ends with its own re-export block into this
// namespace, and this header includes them last. Every include order then declares each name
// before its using-declaration.
#include "uos/api_keys.hpp"
#include "uos/authority/native_kernel.hpp"
#include "uos/authority/native_kernel_contracts.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace uos::kernel::quota_v2 {
  using authority::native_kernel::KERNEL_REPO_POLICY_V2_PREFIX;
  using authority::native_kernel::KERNEL_ORG_POLICY_V2_PREFIX;
  using authority::native_kernel::KERNEL_REPO_WINDOW_V2_PREFIX;
  using authority::native_kernel::KERNEL_ORG_WINDOW_V2_PREFIX;
  using authority::native_kernel::KERNEL_REPO_RESERVATION_V2_PREFIX;
  using authority::native_kernel::KERNEL_ORG_RESERVATION_V2_PREFIX;
  using authority::native_kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY;
  using authority::native_kernel::KERNEL_QUOTA_RESERVATION_LEASE_MS;
  using authority::native_kernel::KERNEL_QUOTA_RESERVATION_RENEWAL_MS;
  using authority::native_kernel::kernelRepoPolicyKey;
  using authority::native_kernel::kernelOrgPolicyKey;
  using authority::native_kernel::kernelRepoWindowKey;
  using authority::native_kernel::kernelOrgWindowKey;
  using authority::native_kernel::kernelRepoReservationKey;
  using authority::native_kernel::kernelOrgReservationKey;
  // `kernelRepoPolicyKey as kernelLimitKey`
  inline rt::KvKey kernelLimitKey(const std::string& owner, const std::string& repo) { return authority::native_kernel::kernelRepoPolicyKey(owner, repo); }
  // `kernelOrgPolicyKey as kernelOrgLimitKey`
  inline rt::KvKey kernelOrgLimitKey(const std::string& owner) { return authority::native_kernel::kernelOrgPolicyKey(owner); }
  using authority::native_kernel::normalizeKernelQuotaWindowV2;
  using authority::native_kernel_contracts::KernelQuotaPolicyV2;
  using authority::native_kernel_contracts::KernelQuotaWindowV2;
  using authority::native_kernel_contracts::KernelQuotaReservationRowV2;
  // `export type { KernelDefaultWindowCutoverV2, KernelDefaults, KernelQuotaEffectivePolicy,
  // KernelQuotaEffectiveWindow, KernelQuotaScope }` from the end of the module, declared before
  // their first use (KernelDefaults is defined below).
  using authority::native_kernel_contracts::KernelDefaultWindowCutoverV2;
  using authority::native_kernel_contracts::KernelQuotaEffectivePolicy;
  using authority::native_kernel_contracts::KernelQuotaEffectiveWindow;
  using authority::native_kernel_contracts::KernelQuotaScope;

  // `Readonly<{ key: typeof KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY; entry: KvEntry<KernelDefaultWindowCutoverV2> }>`:
  // the entry as it was read back after the marker was written (its key and versionstamp are the
  // compare-and-set check that releases the guard).
  struct KernelDefaultWindowCutoverGuard {
    rt::KvKey key;
    rt::KvEntry<> entry;
  };

  // `Readonly<{ ok: true; guard }> | Readonly<{ ok: false; reason }>`
  struct KernelDefaultWindowCutoverDecisionOk {
    static constexpr bool ok = true;
    KernelDefaultWindowCutoverGuard guard;
  };
  struct KernelDefaultWindowCutoverDecisionNotOk {
    static constexpr bool ok = false;
    std::string reason;  // "active_reservations" | "concurrent_change" | "unavailable"
  };
  using KernelDefaultWindowCutoverDecision = std::variant<KernelDefaultWindowCutoverDecisionOk, KernelDefaultWindowCutoverDecisionNotOk>;

  // `Readonly<{ signal: AbortSignal; commit: () => Promise<void>; release: (reason?: string) => Promise<void> }>`.
  // commit and release return the settlement promise, already running; std::nullopt is an
  // omitted reason.
  struct KernelQuotaReservation {
    rt::AbortSignal signal;
    std::function<rt::Promise<void>()> commit;
    std::function<rt::Promise<void>(std::optional<std::string> reason)> release;
  };

  // `Readonly<{ ok: true; reservation }> | Readonly<{ ok: false; response: Response }>`
  struct KernelQuotaReservationDecisionOk {
    static constexpr bool ok = true;
    KernelQuotaReservation reservation;
  };
  struct KernelQuotaReservationDecisionNotOk {
    static constexpr bool ok = false;
    rt::Response response;
  };
  using KernelQuotaReservationDecision = std::variant<KernelQuotaReservationDecisionOk, KernelQuotaReservationDecisionNotOk>;

  // `Readonly<{ ok: true; limit_scope; has_policy }> | Readonly<{ ok: false; response: Response }>`
  struct KernelQuotaPolicyStateDecisionOk {
    static constexpr bool ok = true;
    KernelQuotaScope limit_scope;
    bool has_policy;
  };
  struct KernelQuotaPolicyStateDecisionNotOk {
    static constexpr bool ok = false;
    rt::Response response;
  };
  using KernelQuotaPolicyStateDecision = std::variant<KernelQuotaPolicyStateDecisionOk, KernelQuotaPolicyStateDecisionNotOk>;

  // `Readonly<{ limit; windowMs; limitEntry: KvEntryMaybe<number>; windowEntry: KvEntryMaybe<number> }>`:
  // the KernelQuotaDefaults the native-kernel decisions read, plus the entries it came from.
  struct KernelDefaults : authority::native_kernel_contracts::KernelQuotaDefaults {
    rt::KvEntryMaybe<> limitEntry;
    rt::KvEntryMaybe<> windowEntry;
  };

  inline constexpr authority::native_kernel_contracts::KernelQuotaConstants KERNEL_QUOTA_CONSTANTS{.noUsageLimit = api_keys::API_KEY_NO_USAGE_LIMIT,
                                                                                                  .noExpirationMs = api_keys::API_KEY_NO_EXPIRATION_MS};

  // C++ addition (no TS twin): a stored value as the `unknown` the native-kernel normalizers
  // read (`entry.value` in the TS, which hands them the stored object itself). JSON-shaped parts
  // convert as KvValue::toJson does (undefined members and holes dropped, an undefined element
  // null); every part JSON cannot hold (a top-level undefined, bigint, KvU64, Uint8Array,
  // ArrayBuffer, typed array, Date, RegExp, Error, Map, Set) becomes `{}`, an object that, like
  // those values, is not a number, string, null or undefined and has none of the members a
  // normalizer reads. RangeError "Maximum call stack size exceeded." past rt::kJsonMaxDepth.
  json::value kernelQuotaValue(const rt::KvValue& value);

  // `kernelQuotaRead<T>(entry)`: `{ value: entry.value, revision: entry.versionstamp }`.
  authority::native_kernel_contracts::KernelQuotaRead<> kernelQuotaRead(const rt::KvEntryMaybe<>& entry);

  // Exported at the end of the TS module. `value: unknown`: nullptr is undefined.
  double normalizeUsageLimit(const json::value* value, double fallback);
  double normalizeUsageLimit(const json::value& value, double fallback);
  double normalizeUsageLimit(double value, double fallback);
  double normalizeExpiration(const json::value* value);
  double normalizeExpiration(const json::value& value);
  double normalizeExpiration(double value);

  // std::nullopt is null.
  std::optional<KernelQuotaPolicyV2> normalizeKernelQuotaPolicyV2(const json::value& value, const KernelQuotaScope& scope, const std::string& owner,
                                                                  const std::optional<std::string>& repo);

  // Exported at the end of the TS module. Throws Error "kernel quota policy is malformed" for a
  // present entry that does not normalize.
  KernelQuotaEffectivePolicy policyFor(const rt::KvEntryMaybe<>& entry, const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo,
                                       const KernelDefaults& defaults);

  // Exported at the end of the TS module.
  rt::Task<KernelDefaults> loadDefaults(std::shared_ptr<rt::Kv> kv);

  // The `policy: { limit: number; windowMs: number; expiresAtMs: number }` parameter of
  // repoRecord/orgRecord (an effective policy, or the summary of a written one).
  struct KernelPolicySummary {
    double limit;
    double windowMs;
    double expiresAtMs;
  };

  // Exported at the end of the TS module.
  ::uos::types::KernelAuthLimitRecord repoRecord(const std::string& owner, const std::string& repo, const KernelPolicySummary& policy, const KernelQuotaWindowV2& window,
                                                 double nowMs);
  ::uos::types::KernelOrgLimitRecord orgRecord(const std::string& owner, const KernelPolicySummary& policy, const KernelQuotaWindowV2& window, double nowMs);

  // `Readonly<{ record: KernelAuthLimitRecord; source: "default" | "kv" }>`
  struct KernelAuthLimitSnapshot {
    ::uos::types::KernelAuthLimitRecord record;
    std::string source;  // "default" | "kv"
  };
  // `Readonly<{ record: KernelOrgLimitRecord; source: "default" | "kv" }>`
  struct KernelOrgLimitSnapshot {
    ::uos::types::KernelOrgLimitRecord record;
    std::string source;  // "default" | "kv"
  };

  // std::nullopt is null.
  rt::Task<std::optional<KernelAuthLimitSnapshot>> getKernelUsageLimitSnapshot(std::string owner, std::string repo);
  rt::Task<std::optional<KernelOrgLimitSnapshot>> getKernelOrgUsageLimitSnapshot(std::string owner);

  rt::Task<std::optional<std::vector<::uos::types::KernelAuthLimitRecord>>> listKernelUsageLimits();
  rt::Task<std::optional<std::vector<::uos::types::KernelOrgLimitRecord>>> listKernelOrgUsageLimits();

  using authority::native_kernel::KERNEL_QUOTA_RESERVATION_RETENTION_MS;
  using authority::native_kernel::KERNEL_QUOTA_SETTLEMENT_RETRY_MS;
  using authority::native_kernel::MAX_KV_RETRIES;
  using authority::native_kernel::hasLiveKernelReservations;
  using authority::native_kernel::kernelPolicyKeys;
  using authority::native_kernel::kernelRepoPart;
  using authority::native_kernel::kernelScopedPolicyKey;
  using authority::native_kernel::kernelScopedWindowKey;
  using authority::native_kernel::normalizeWindow;
  using authority::native_kernel::windowForEffectivePolicy;
}

// The write path, reservations and reserve entry points moved into their own
// modules; they are re-exported here so existing callers keep one import site.
#include "uos/kernel/quota_write.hpp"
#include "uos/kernel/quota_reservations.hpp"
#include "uos/kernel/quota_reserve.hpp"
