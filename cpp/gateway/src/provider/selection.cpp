#include "uos/provider/selection.hpp"
#include "uos/rt/memory.hpp"

#include "uos/kv.hpp"
#include "uos/provider/presentation.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace uos::provider::selection {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    // `new Set<string>(SELECTABLE_PROVIDER_IDS)`
    bool selectableProviderIdSetHas(std::string_view id) {
      return std::find(SELECTABLE_PROVIDER_IDS.begin(), SELECTABLE_PROVIDER_IDS.end(), id) != SELECTABLE_PROVIDER_IDS.end();
    }

    constexpr std::string_view CODEX_SUBSCRIPTION_ID_PREFIX = "codex:";

    // `/^[a-f0-9]{64}$/.test(value)`
    bool codexSubscriptionHashPatternTest(std::string_view value) {
      if (value.size() != 64) return false;
      return std::all_of(value.begin(), value.end(), [](char character) { return (character >= 'a' && character <= 'f') || (character >= '0' && character <= '9'); });
    }

    std::unordered_map<std::string, std::string>& subscriptionHashCache() {
      static std::unordered_map<std::string, std::string> cache;
      return cache;
    }

    /** Last selection served to the routing path, with the write path priming it. */
    struct CachedSelection {
      std::optional<ProviderSelection> value;
      double expires_at_ms;
    };
    std::optional<CachedSelection> cachedSelection;
    std::size_t subscriptionHashBytes = 0;

    void cacheSelection(std::optional<ProviderSelection> value, double expires) {
      const auto bytes = value ? sizeof(CachedSelection) + rt::memory::bytes(value->provider_ids) : 0;
      rt::memory::setCache(rt::memory::Cache::provider_selection, value ? 1 : 0, bytes);
      cachedSelection = CachedSelection{std::move(value), expires};
    }
    std::optional<rt::Promise<std::optional<ProviderSelection>>> selectionLoadInFlight;
    /** Invalidates a read that began before a successful write primed the cache. */
    double selectionCacheGeneration = 0;
    /** Prevents an invalidated read from clearing a replacement load. */
    double selectionLoadToken = 0;

    // The shared body of both normalizeSelectedProviderIds overloads: `ids` holds every raw id
    // that `getString(raw)` reads as a string, in order (non-strings are skipped by `if (!id)`).
    std::vector<ProviderSelectionId> normalizeSubmittedIds(const std::vector<std::string_view>& ids) {
      rt::OrderedSet<std::string> submittedProviders;
      rt::OrderedSet<std::string> submittedSubscriptions;
      for (const std::string_view raw : ids) {
        const std::string id = rt::trim(raw);
        if (id.empty()) continue;
        if (selectableProviderIdSetHas(id)) {
          submittedProviders.add(id);
          continue;
        }
        if (isCodexSubscriptionSelectionId(std::string_view(id))) submittedSubscriptions.add(id);
      }
      std::vector<ProviderSelectionId> providers;
      for (const std::string_view id : SELECTABLE_PROVIDER_IDS) {
        if (submittedProviders.has(std::string(id))) providers.emplace_back(id);
      }
      if (submittedProviders.has(std::string("codex"))) return providers;
      std::vector<std::string> subscriptions = rt::arrayFrom(submittedSubscriptions);
      rt::sort(subscriptions, [](const std::string& left, const std::string& right) { return static_cast<double>(rt::localeCompare(left, right)); });
      for (std::string& subscription : subscriptions) providers.push_back(std::move(subscription));
      return providers;
    }

    // `selection?.provider_ids ?? []`
    std::vector<std::string> selectionIds(const std::optional<ProviderSelection>& selection) {
      return selection ? selection->provider_ids : std::vector<std::string>{};
    }

    rt::Task<std::optional<ProviderSelection>> loadSelectionIntoCache(double nowMs, std::optional<ProviderSelection> stale, double cacheGenerationAtLoad,
                                                                      double loadToken) {
      const auto cleanup = rt::scopeExit([loadToken] {
        if (selectionLoadToken == loadToken) selectionLoadInFlight.reset();
      });
      std::optional<ProviderSelection> result;
      try {
        auto kvPromise = kv::getKv();
        std::shared_ptr<rt::Kv> kv = co_await kvPromise.wait();
        rt::Promise<std::optional<ProviderSelection>> loading(loadProviderSelection(kv));
        std::optional<ProviderSelection> value = co_await loading.wait();
        if (selectionCacheGeneration == cacheGenerationAtLoad) {
          cacheSelection(value, nowMs + PROVIDER_SELECTION_CACHE_TTL_MS);
        }
        result = std::move(value);
      } catch (...) {
        result = std::move(stale);
      }
      co_return result;
    }
  }

  rt::Task<std::string> codexSubscriptionHash(std::string accountId) {
    if (const auto cached = subscriptionHashCache().find(accountId); cached != subscriptionHashCache().end()) co_return cached->second;
    rt::Promise<std::string> hashing(utils::sha256Hex(accountId));
    std::string hash = co_await hashing.wait();
    auto& cache = subscriptionHashCache();
    if (cache.try_emplace(accountId, hash).second)
      subscriptionHashBytes += sizeof(std::pair<const std::string, std::string>) + rt::memory::bytes(accountId) + rt::memory::bytes(hash);
    rt::memory::setCache(rt::memory::Cache::subscription_hashes, cache.size(), subscriptionHashBytes);
    co_return hash;
  }

  CodexSubscriptionId codexSubscriptionSelectionId(std::string_view accountIdHash) { return std::string(CODEX_SUBSCRIPTION_ID_PREFIX) + std::string(accountIdHash); }

  bool isCodexSubscriptionSelectionId(std::string_view value) {
    return rt::startsWith(value, CODEX_SUBSCRIPTION_ID_PREFIX) && codexSubscriptionHashPatternTest(value.substr(CODEX_SUBSCRIPTION_ID_PREFIX.size()));
  }

  bool isCodexSubscriptionSelectionId(const json::value& value) { return value.is_string() && isCodexSubscriptionSelectionId(std::string_view(value.get_string())); }

  bool isCodexSubscriptionSelectionId(const json::value* value) { return value != nullptr && isCodexSubscriptionSelectionId(*value); }

  std::optional<std::string> codexSubscriptionHashFromSelectionId(std::string_view value) {
    if (!isCodexSubscriptionSelectionId(value)) return std::nullopt;
    return std::string(value.substr(CODEX_SUBSCRIPTION_ID_PREFIX.size()));
  }

  std::optional<std::string> codexSubscriptionHashFromSelectionId(const json::value& value) {
    if (!value.is_string()) return std::nullopt;
    return codexSubscriptionHashFromSelectionId(std::string_view(value.get_string()));
  }

  std::optional<std::string> codexSubscriptionHashFromSelectionId(const json::value* value) {
    if (value == nullptr) return std::nullopt;
    return codexSubscriptionHashFromSelectionId(*value);
  }

  bool isProviderSelectionId(std::string_view value) { return isSelectableProviderId(value) || isCodexSubscriptionSelectionId(value); }

  bool isProviderSelectionId(const json::value& value) { return isSelectableProviderId(value) || isCodexSubscriptionSelectionId(value); }

  bool isProviderSelectionId(const json::value* value) { return value != nullptr && isProviderSelectionId(*value); }

  bool isSelectableProviderId(std::string_view value) { return selectableProviderIdSetHas(rt::trim(value)); }

  bool isSelectableProviderId(const json::value& value) { return value.is_string() && isSelectableProviderId(std::string_view(value.get_string())); }

  bool isSelectableProviderId(const json::value* value) { return value != nullptr && isSelectableProviderId(*value); }

  // ── Normalize / validate ─────────────────────────────────────────────────────

  std::vector<ProviderSelectionId> normalizeSelectedProviderIds(const json::array& rawIds) {
    std::vector<std::string_view> ids;
    for (const json::value& raw : rawIds) {
      if (raw.is_string()) ids.emplace_back(raw.get_string());
    }
    return normalizeSubmittedIds(ids);
  }

  std::vector<ProviderSelectionId> normalizeSelectedProviderIds(const std::vector<std::string>& rawIds) {
    std::vector<std::string_view> ids(rawIds.begin(), rawIds.end());
    return normalizeSubmittedIds(ids);
  }

  std::optional<ProviderSelection> normalizeProviderSelection(const rt::KvValue& value) {
    // `isRecord(value)` admits every object; only a plain object can carry `provider_ids`.
    if (!value.isObject()) return std::nullopt;
    const rt::KvValue& rawIds = value["provider_ids"];
    if (!rawIds.isArray()) return std::nullopt;
    std::vector<std::string_view> ids;
    for (const rt::KvValue& raw : rawIds.asArray()) {
      // `some` skips holes; `for...of` reads one as undefined, which `getString` drops.
      if (raw.is<rt::KvHole>()) continue;
      if (!raw.isString()) return std::nullopt;
      ids.emplace_back(raw.asString());
    }
    const rt::KvValue& updatedAtMs = value["updated_at_ms"];
    if (!updatedAtMs.isNumber() || !rt::Number::isSafeInteger(updatedAtMs.asNumber()) || updatedAtMs.asNumber() <= 0) return std::nullopt;
    return ProviderSelection{.provider_ids = normalizeSubmittedIds(ids), .updated_at_ms = updatedAtMs.asNumber()};
  }

  // ── KV helpers ───────────────────────────────────────────────────────────────

  rt::Task<std::optional<ProviderSelection>> loadProviderSelection(std::shared_ptr<rt::Kv> kv) {
    if (!kv) co_return std::nullopt;
    const rt::KvGetOptions options{.consistency = rt::KvConsistencyLevel::strong};
    auto reading = kv->get(PROVIDER_SELECTION_KV_KEY, options);
    const rt::KvEntryMaybe<> entry = co_await std::move(reading);
    co_return normalizeProviderSelection(entry.value);
  }

  rt::Task<std::optional<ProviderSelection>> storeProviderSelection(std::shared_ptr<rt::Kv> kv, std::vector<ProviderSelectionId> providerIds) {
    const ProviderSelection selection{.provider_ids = normalizeSelectedProviderIds(providerIds), .updated_at_ms = rt::dateNow()};
    bool rejected = false;
    try {
      const rt::KvValue stored(json::value_from(selection));
      auto writing = kv->set(PROVIDER_SELECTION_KV_KEY, stored);
      co_await std::move(writing);
    } catch (...) {
      rejected = true;
    }
    if (rejected) co_return std::nullopt;
    selectionCacheGeneration += 1;
    selectionLoadToken += 1;
    selectionLoadInFlight.reset();
    cacheSelection(selection, selection.updated_at_ms + PROVIDER_SELECTION_CACHE_TTL_MS);
    co_return selection;
  }

  // ── Enforcement ──────────────────────────────────────────────────────────────

  bool isProviderEnabled(std::string_view provider, const std::optional<ProviderSelection>& selection) {
    if (!selection || selection->provider_ids.empty()) return true;
    if (std::find(selection->provider_ids.begin(), selection->provider_ids.end(), provider) != selection->provider_ids.end()) return true;
    if (provider != "codex") return false;
    return std::any_of(selection->provider_ids.begin(), selection->provider_ids.end(),
                       [](const std::string& id) { return isCodexSubscriptionSelectionId(std::string_view(id)); });
  }

  bool providerSelectionIsActive(const std::optional<ProviderSelection>& selection) { return selection && !selection->provider_ids.empty(); }

  CodexAccountEligibility codexAccountEligibility(const std::optional<ProviderSelection>& selection) {
    const std::vector<std::string> ids = selectionIds(selection);
    if (ids.empty() || std::find(ids.begin(), ids.end(), "codex") != ids.end()) return CodexAccountEligibilityAll{};
    std::vector<std::string> hashes;
    for (const std::string& id : ids) {
      if (std::optional<std::string> hash = codexSubscriptionHashFromSelectionId(std::string_view(id))) hashes.push_back(std::move(*hash));
    }
    if (!hashes.empty()) return CodexAccountEligibilityOnly{.hashes = std::move(hashes)};
    return CodexAccountEligibilityNone{};
  }

  bool isCodexSubscriptionEnabled(const std::string& accountIdHash, const std::optional<ProviderSelection>& selection) {
    const CodexAccountEligibility eligibility = codexAccountEligibility(selection);
    if (std::holds_alternative<CodexAccountEligibilityAll>(eligibility)) return true;
    if (std::holds_alternative<CodexAccountEligibilityNone>(eligibility)) return false;
    const std::vector<std::string>& hashes = std::get<CodexAccountEligibilityOnly>(eligibility).hashes;
    return std::find(hashes.begin(), hashes.end(), accountIdHash) != hashes.end();
  }

  namespace detail {
    std::optional<std::vector<std::size_t>> selectedCatalogEntryProviders(const std::vector<std::string>& providerIds, const ProviderSelection& selection) {
      const std::optional<ProviderSelection> active = selection;
      const auto enabled = [&active](std::string_view id) { return selectableProviderIdSetHas(id) ? isProviderEnabled(id, active) : true; };
      // The gateway identity owns its route's entry: when it is switched off the
      // whole entry leaves, even if one of its upstream hops stays enabled.
      const bool ownsGatewayRoute =
        std::any_of(providerIds.begin(), providerIds.end(), [](const std::string& id) { return id == presentation::GATEWAY_PROVIDER_ID; });
      if (ownsGatewayRoute && !enabled(presentation::GATEWAY_PROVIDER_ID)) return std::nullopt;
      std::vector<std::size_t> kept;
      for (std::size_t index = 0; index < providerIds.size(); ++index) {
        if (enabled(providerIds[index])) kept.push_back(index);
      }
      if (kept.empty()) return std::nullopt;
      return kept;
    }
  }

  // ── Cached routing read ──────────────────────────────────────────────────────

  rt::Task<std::optional<ProviderSelection>> loadProviderSelectionCached() {
    // The default parameter (`nowMs = Date.now()`): the same call, without a turn of its own.
    std::optional<ProviderSelection> selection = co_await loadProviderSelectionCached(rt::dateNow());
    co_return selection;
  }

  rt::Task<std::optional<ProviderSelection>> loadProviderSelectionCached(double nowMs) {
    if (cachedSelection && cachedSelection->expires_at_ms > nowMs) co_return cachedSelection->value;
    if (selectionLoadInFlight) {
      const rt::Promise<std::optional<ProviderSelection>> inFlight = *selectionLoadInFlight;
      std::optional<ProviderSelection> shared = co_await inFlight.wait();
      co_return shared;
    }
    std::optional<ProviderSelection> stale = cachedSelection ? cachedSelection->value : std::nullopt;
    const double cacheGenerationAtLoad = selectionCacheGeneration;
    const double loadToken = ++selectionLoadToken;
    selectionLoadInFlight = rt::Promise<std::optional<ProviderSelection>>(loadSelectionIntoCache(nowMs, std::move(stale), cacheGenerationAtLoad, loadToken));
    const rt::Promise<std::optional<ProviderSelection>> started = *selectionLoadInFlight;
    std::optional<ProviderSelection> loaded = co_await started.wait();
    co_return loaded;
  }

  void resetProviderSelectionCacheForTest() {
    selectionCacheGeneration += 1;
    selectionLoadToken += 1;
    cachedSelection.reset();
    rt::memory::setCache(rt::memory::Cache::provider_selection, 0, 0);
    selectionLoadInFlight.reset();
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderSelection& value) {
    out = Writer().required("provider_ids", value.provider_ids).required("updated_at_ms", value.updated_at_ms).done();
  }

  ProviderSelection tag_invoke(const json::value_to_tag<ProviderSelection>&, const json::value& value) {
    const Reader in(value, "ProviderSelection");
    return ProviderSelection{
      .provider_ids = in.required<std::vector<std::string>>("provider_ids"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }
}
