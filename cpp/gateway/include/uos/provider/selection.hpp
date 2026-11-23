#pragma once
// Port of src/provider/selection.ts.
//
// `kv: Kv | null` is std::shared_ptr<rt::Kv> (a null pointer is null); `kv: Kv` is non-null.
// `selection: ProviderSelection | null` is const std::optional<ProviderSelection>&. The
// `value: unknown` guards take json::value plus a pointer overload (nullptr is undefined) and a
// std::string_view overload for a known string (a string literal argument is ambiguous with the
// json::value one: pass a std::string_view). normalizeProviderSelection validates stored values,
// so it takes rt::KvValue (a json::value converts implicitly).
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::provider::selection {
  // ── KV key ───────────────────────────────────────────────────────────────────

  inline const rt::KvKey PROVIDER_SELECTION_KV_KEY{"uos_ai", "provider_selection", "v1"};

  /** Routing reads this control on the hot path, so a short cache bounds KV reads. */
  inline constexpr double PROVIDER_SELECTION_CACHE_TTL_MS = 5'000;

  // ── Provider roster ──────────────────────────────────────────────────────────

  /**
   * The provider vocabulary the model catalog already publishes, in the order the
   * inference waterfall tries it: the Codex subscription tier first, then the two
   * paid fallback tiers (`surplus` before `openlux`), then the credential-gated
   * direct routes (`deepseek`, `cerebras`, `lithos`), and the gateway-owned
   * `ubiquity` identity last. Checking `ubiquity` allows the synthetic
   * `ubiquity/deepseek-flash` route; in an active selection its absence
   * switches that route off like any other unchecked provider.
   */
  inline constexpr std::array<std::string_view, 10> SELECTABLE_PROVIDER_IDS = {"codex",  "claude_code", "gemini", "surplus", "openlux", "deepseek",
                                                                               "cerebras", "lithos",    "openrouter", "ubiquity"};

  // `(typeof SELECTABLE_PROVIDER_IDS)[number]`
  using SelectableProviderId = std::string;

  /**
   * One configured Codex subscription, addressed by the opaque hash of its
   * account id. A selection may therefore narrow the Codex tier to a subset of
   * the configured accounts the same way it narrows the whole waterfall:
   * `codex` alone means every configured subscription, `codex:<hash>` entries
   * mean exactly those, and neither means the Codex tier is switched off.
   */
  // `` `codex:${string}` ``
  using CodexSubscriptionId = std::string;

  // `SelectableProviderId | CodexSubscriptionId`
  using ProviderSelectionId = std::string;

  struct ProviderSelection {
    std::vector<ProviderSelectionId> provider_ids;
    double updated_at_ms;
    friend bool operator==(const ProviderSelection&, const ProviderSelection&) = default;
  };

  /**
   * Stable opaque id for one configured subscription, for the operator picker.
   * The selection never stores a raw account id: this is deliberately the same
   * unsalted account hash the admin console already uses to name one subscription
   * in the banked-reset settings, so both admin surfaces identify the same
   * account with the same opaque id.
   */
  rt::Task<std::string> codexSubscriptionHash(std::string accountId);

  CodexSubscriptionId codexSubscriptionSelectionId(std::string_view accountIdHash);

  // `(value: unknown): value is CodexSubscriptionId`
  bool isCodexSubscriptionSelectionId(const json::value& value);
  bool isCodexSubscriptionSelectionId(const json::value* value);
  bool isCodexSubscriptionSelectionId(std::string_view value);

  /** The account hash inside a subscription id, or `null` for any other id. */
  // std::nullopt is null.
  std::optional<std::string> codexSubscriptionHashFromSelectionId(const json::value& value);
  std::optional<std::string> codexSubscriptionHashFromSelectionId(const json::value* value);
  std::optional<std::string> codexSubscriptionHashFromSelectionId(std::string_view value);

  /** Every id the operator API may store: a roster provider or one subscription. */
  bool isProviderSelectionId(const json::value& value);
  bool isProviderSelectionId(const json::value* value);
  bool isProviderSelectionId(std::string_view value);

  // `(value: unknown): value is SelectableProviderId` (the trimmed string is on the roster).
  bool isSelectableProviderId(const json::value& value);
  bool isSelectableProviderId(const json::value* value);
  bool isSelectableProviderId(std::string_view value);

  // ── Normalize / validate ─────────────────────────────────────────────────────

  /**
   * Canonical form of a submitted selection: known provider ids in roster order,
   * then subscription ids in hash order, de-duplicated. The `codex` umbrella
   * means every configured subscription, so it absorbs any subscription id
   * submitted alongside it and storage can never hold an ambiguous pair.
   */
  // `rawIds: readonly unknown[]`
  std::vector<ProviderSelectionId> normalizeSelectedProviderIds(const json::array& rawIds);
  // The same over known strings (`readonly ProviderSelectionId[]`, as storeProviderSelection passes).
  std::vector<ProviderSelectionId> normalizeSelectedProviderIds(const std::vector<std::string>& rawIds);

  /**
   * Read a stored selection. Ids that are no longer on the roster are dropped
   * rather than voiding the whole selection, so retiring a provider cannot
   * silently switch every other provider back on.
   */
  // `(value: unknown): ProviderSelection | null`; std::nullopt is null.
  std::optional<ProviderSelection> normalizeProviderSelection(const rt::KvValue& value);

  // ── KV helpers ───────────────────────────────────────────────────────────────

  rt::Task<std::optional<ProviderSelection>> loadProviderSelection(std::shared_ptr<rt::Kv> kv);

  /**
   * Persist a selection and prime the routing cache with exactly what was
   * written, so an operator sees the effect of a save on the next request
   * instead of waiting out the cache TTL. Returns `null` when KV rejected it.
   */
  rt::Task<std::optional<ProviderSelection>> storeProviderSelection(std::shared_ptr<rt::Kv> kv, std::vector<ProviderSelectionId> providerIds);

  // ── Enforcement ──────────────────────────────────────────────────────────────

  /**
   * An empty or absent selection is no filter at all: every provider stays
   * eligible. That matches the model whitelist contract and keeps an accidental
   * (or stale) empty selection from disabling inference.
   *
   * `codex` is enabled by its own id or by any single selected subscription, so
   * narrowing the tier to one account never reads as switching the tier off.
   */
  // `provider: SelectableProviderId`
  bool isProviderEnabled(std::string_view provider, const std::optional<ProviderSelection>& selection);

  bool providerSelectionIsActive(const std::optional<ProviderSelection>& selection);

  /**
   * Which configured Codex subscriptions may serve inference: every account (no
   * restriction), none of them (the tier is switched off), or exactly the hashes
   * the operator selected.
   */
  // `Readonly<{ kind: "all" }> | Readonly<{ kind: "none" }> | Readonly<{ kind: "only"; hashes: readonly string[] }>`
  struct CodexAccountEligibilityAll {
    static constexpr std::string_view kind = "all";
    friend bool operator==(const CodexAccountEligibilityAll&, const CodexAccountEligibilityAll&) = default;
  };
  struct CodexAccountEligibilityNone {
    static constexpr std::string_view kind = "none";
    friend bool operator==(const CodexAccountEligibilityNone&, const CodexAccountEligibilityNone&) = default;
  };
  struct CodexAccountEligibilityOnly {
    static constexpr std::string_view kind = "only";
    std::vector<std::string> hashes;
    friend bool operator==(const CodexAccountEligibilityOnly&, const CodexAccountEligibilityOnly&) = default;
  };
  using CodexAccountEligibility = std::variant<CodexAccountEligibilityAll, CodexAccountEligibilityNone, CodexAccountEligibilityOnly>;

  CodexAccountEligibility codexAccountEligibility(const std::optional<ProviderSelection>& selection);

  bool isCodexSubscriptionEnabled(const std::string& accountIdHash, const std::optional<ProviderSelection>& selection);

  namespace detail {
    // One entry of filterCatalogEntriesByProviderSelection under an active selection, out of line
    // because it reads presentation::GATEWAY_PROVIDER_ID and uos/provider/presentation.hpp
    // includes this header. std::nullopt drops the entry; otherwise the positions, in order, of
    // the providers the entry keeps (every position when none is filtered out).
    std::optional<std::vector<std::size_t>> selectedCatalogEntryProviders(const std::vector<std::string>& providerIds, const ProviderSelection& selection);
  }

  /**
   * Keep only the entries an enabled provider serves, narrowing each row to the
   * providers that are still active. An entry no enabled provider serves is
   * dropped, so a disabled provider can never be advertised or dispatched to.
   * Catalog vocabulary the picker does not control stays listed untouched.
   */
  // `<T extends Readonly<{ providers: readonly Readonly<{ id: string }>[] }>>`: T has a sequence
  // member `providers` whose elements have a string `id`. An unfiltered entry is copied as is.
  template <class T>
  std::vector<T> filterCatalogEntriesByProviderSelection(const std::vector<T>& entries, const std::optional<ProviderSelection>& selection) {
    if (!providerSelectionIsActive(selection)) return entries;
    std::vector<T> filtered;
    for (const T& entry : entries) {
      std::vector<std::string> providerIds;
      for (const auto& provider : entry.providers) providerIds.push_back(std::string(provider.id));
      const std::optional<std::vector<std::size_t>> kept = detail::selectedCatalogEntryProviders(providerIds, *selection);
      if (!kept) continue;
      if (kept->size() == providerIds.size()) {
        filtered.push_back(entry);
        continue;
      }
      T narrowed = entry;
      narrowed.providers.clear();
      for (const std::size_t index : *kept) narrowed.providers.push_back(entry.providers[index]);
      filtered.push_back(std::move(narrowed));
    }
    return filtered;
  }

  // ── Cached routing read ──────────────────────────────────────────────────────

  /**
   * The routing path reads the selection through this cache. A failed read keeps
   * the last known selection and, with nothing cached, fails open: an operator
   * control must never turn a KV hiccup into a routing outage.
   */
  // `nowMs = Date.now()`
  rt::Task<std::optional<ProviderSelection>> loadProviderSelectionCached();
  rt::Task<std::optional<ProviderSelection>> loadProviderSelectionCached(double nowMs);

  void resetProviderSelectionCacheForTest();

  // ---- JSON conversion of the stored selection (members in declaration order) --------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderSelection& value);
  ProviderSelection tag_invoke(const json::value_to_tag<ProviderSelection>&, const json::value& value);
}
