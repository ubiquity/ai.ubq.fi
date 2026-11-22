#include "uos/models/codex_models_availability.hpp"
#include "uos/rt/memory.hpp"

#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace uos::models::codex_models_availability {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    template <class T> using Record = authority::contracts::Record<T>;

    // ---- JavaScript semantics over a stored (`unknown`) value ----------------------------------

    // `typeof value === "object" && value !== null` (utils.isRecord): arrays and every object kind.
    bool isRecord(const rt::KvValue& value) {
      return !(value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt() || value.is<rt::KvHole>());
    }

    // utils.getString over a stored value.
    std::optional<std::string> getString(const rt::KvValue& value) {
      if (value.isString()) return value.asString();
      return std::nullopt;
    }

    template <class T> T element(const rt::KvBytes& buffer, std::uint64_t offset) {
      T out;
      std::memcpy(&out, buffer.data() + offset, sizeof(T));
      return out;
    }

    template <class T> void pushElements(std::vector<std::pair<std::string, rt::KvValue>>& entries, const rt::KvArrayBufferView& view) {
      const std::uint64_t count = view.byteLength / sizeof(T);
      for (std::uint64_t index = 0; index < count; ++index) {
        const T value = element<T>(view.buffer, view.byteOffset + index * sizeof(T));
        if constexpr (std::is_same_v<T, std::int64_t>) entries.emplace_back(std::to_string(index), rt::KvValue(rt::BigInt(value)));
        else if constexpr (std::is_same_v<T, std::uint64_t>) entries.emplace_back(std::to_string(index), rt::KvValue(rt::BigInt::fromUint64(value)));
        else entries.emplace_back(std::to_string(index), rt::KvValue(static_cast<double>(value)));
      }
    }

    // `Object.entries(value)` for a stored object of any kind: its own enumerable string-keyed
    // properties in JS order. Plain objects give their members, arrays and typed arrays their
    // elements (array holes are skipped), a KvU64 its `value`, every other object kind none.
    std::vector<std::pair<std::string, rt::KvValue>> objectEntries(const rt::KvValue& value) {
      std::vector<std::pair<std::string, rt::KvValue>> entries;
      if (value.isObject()) {
        for (const auto& [key, member] : value.asObject()) entries.emplace_back(key, member);
      } else if (value.isArray()) {
        const rt::KvArray& items = value.asArray();
        for (std::size_t index = 0; index < items.size(); ++index)
          if (!items[index].is<rt::KvHole>()) entries.emplace_back(std::to_string(index), items[index]);
      } else if (value.isBytes()) {
        const rt::KvBytes& bytes = value.asBytes();
        for (std::size_t index = 0; index < bytes.size(); ++index) entries.emplace_back(std::to_string(index), rt::KvValue(static_cast<double>(bytes[index])));
      } else if (value.isU64()) {
        entries.emplace_back("value", rt::KvValue(value.asU64().toBigInt()));
      } else if (value.is<rt::KvArrayBufferView>()) {
        const rt::KvArrayBufferView& view = value.as<rt::KvArrayBufferView>();
        switch (view.kind) {
          case rt::KvViewKind::DataView: break;
          case rt::KvViewKind::Int8Array: pushElements<std::int8_t>(entries, view); break;
          case rt::KvViewKind::Uint8Array:
          case rt::KvViewKind::Uint8ClampedArray: pushElements<std::uint8_t>(entries, view); break;
          case rt::KvViewKind::Int16Array: pushElements<std::int16_t>(entries, view); break;
          case rt::KvViewKind::Uint16Array: pushElements<std::uint16_t>(entries, view); break;
          case rt::KvViewKind::Int32Array: pushElements<std::int32_t>(entries, view); break;
          case rt::KvViewKind::Uint32Array: pushElements<std::uint32_t>(entries, view); break;
          case rt::KvViewKind::Float32Array: pushElements<float>(entries, view); break;
          case rt::KvViewKind::Float64Array: pushElements<double>(entries, view); break;
          case rt::KvViewKind::BigInt64Array: pushElements<std::int64_t>(entries, view); break;
          case rt::KvViewKind::BigUint64Array: pushElements<std::uint64_t>(entries, view); break;
        }
      }
      return entries;
    }

    // `typeof value === "number" && Number.isSafeInteger(value) && value > 0`
    bool isPositiveSafeInteger(const rt::KvValue& value) { return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && value.asNumber() > 0; }

    // `Reflect.deleteProperty(record, key)`: the other own keys keep their JS order.
    template <class T> void deleteProperty(Record<T>& record, const std::string& key) {
      if (!record.has(key)) return;
      Record<T> kept;
      for (const std::string& name : record.keys())
        if (name != key) kept.define(name, *record.get(name));
      record = std::move(kept);
    }

    std::vector<std::string> uniqueSlugs(const std::vector<std::optional<std::string>>& values) {
      std::vector<std::string> slugs;
      std::unordered_set<std::string> seen;
      for (const std::optional<std::string>& value : values) {
        if (!value) continue;
        std::string slug = rt::trim(*value);
        if (slug.empty() || seen.contains(slug)) continue;
        seen.insert(slug);
        slugs.push_back(std::move(slug));
      }
      return slugs;
    }

    std::vector<std::string> uniqueSlugs(const json::array& values) {
      std::vector<std::optional<std::string>> strings;
      for (const json::value& value : values) strings.push_back(utils::getString(value));
      return uniqueSlugs(strings);
    }

    std::vector<std::string> uniqueSlugs(const rt::KvArray& values) {
      std::vector<std::optional<std::string>> strings;
      for (const rt::KvValue& value : values) strings.push_back(getString(value));
      return uniqueSlugs(strings);
    }

    std::optional<CodexAccountModelCatalog> parseCodexAccountModelCatalog(const rt::KvValue& value) {
      if (!isRecord(value)) return std::nullopt;
      const std::optional<std::string> clientVersionText = getString(value["client_version"]);
      const std::string clientVersion = clientVersionText ? rt::trim(*clientVersionText) : std::string();
      const rt::KvValue& updatedAtMs = value["updated_at_ms"];
      if (clientVersion.empty()) return std::nullopt;
      if (!isPositiveSafeInteger(updatedAtMs)) return std::nullopt;
      if (!value["slugs"].isArray()) return std::nullopt;
      return CodexAccountModelCatalog{.client_version = clientVersion, .slugs = uniqueSlugs(value["slugs"].asArray()), .updated_at_ms = updatedAtMs.asNumber()};
    }

    std::optional<Record<double>> parseUnsupportedEntry(const rt::KvValue& value) {
      if (!isRecord(value)) return std::nullopt;
      Record<double> models;
      for (const auto& [model, observedAtMs] : objectEntries(value)) {
        if (rt::trim(model).empty()) continue;
        if (!isPositiveSafeInteger(observedAtMs)) continue;
        models.set(model, observedAtMs.asNumber());
      }
      return models;
    }

    Record<CodexAccountModelCatalog> parseStoredAccounts(const rt::KvValue& value) {
      Record<CodexAccountModelCatalog> accounts;
      for (const auto& [accountId, entry] : objectEntries(value)) {
        if (rt::trim(accountId).empty()) continue;
        std::optional<CodexAccountModelCatalog> catalog = parseCodexAccountModelCatalog(entry);
        if (catalog) accounts.set(accountId, std::move(*catalog));
      }
      return accounts;
    }

    Record<Record<double>> parseStoredUnsupported(const rt::KvValue& value) {
      Record<Record<double>> unsupported;
      for (const auto& [accountId, entry] : objectEntries(value)) {
        if (rt::trim(accountId).empty()) continue;
        std::optional<Record<double>> models = parseUnsupportedEntry(entry);
        if (models && models->size()) unsupported.set(accountId, std::move(*models));
      }
      return unsupported;
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAccountModelCatalog& value) {
    out = Writer().required("client_version", value.client_version).required("slugs", value.slugs).required("updated_at_ms", value.updated_at_ms).done();
  }

  CodexAccountModelCatalog tag_invoke(const json::value_to_tag<CodexAccountModelCatalog>&, const json::value& value) {
    const Reader in(value, "CodexAccountModelCatalog");
    return {
      .client_version = in.required<std::string>("client_version"),
      .slugs = in.required<std::vector<std::string>>("slugs"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  json::object toJson(const CodexAccountModelCatalog& catalog) { return json::value_from(catalog).as_object(); }

  json::object toJson(const CodexAccountModelsStore& store) {
    json::object accounts;
    for (const std::string& accountId : store.accounts.keys()) rt::objectDefine(accounts, accountId, json::value(toJson(*store.accounts.get(accountId))));
    json::object unsupported;
    for (const std::string& accountId : store.unsupported.keys()) {
      const Record<double>& models = *store.unsupported.get(accountId);
      json::object observed;
      for (const std::string& model : models.keys()) rt::objectDefine(observed, model, rt::makeNumber(*models.get(model)));
      rt::objectDefine(unsupported, accountId, json::value(std::move(observed)));
    }
    json::object out;
    out["accounts"] = std::move(accounts);
    out["unsupported"] = std::move(unsupported);
    return out;
  }

  CodexAccountModelsStore emptyCodexAccountModelsStore() { return CodexAccountModelsStore{}; }

  std::optional<CodexAccountModelsStore> parseCodexAccountModelsStore(const rt::KvValue& value) {
    if (!isRecord(value)) return std::nullopt;
    const rt::KvValue& rawAccounts = value["accounts"];
    const rt::KvValue& rawUnsupported = value["unsupported"];
    const bool accountsPresent = isRecord(rawAccounts);
    const bool unsupportedPresent = isRecord(rawUnsupported);
    if (!accountsPresent && !unsupportedPresent) return std::nullopt;
    CodexAccountModelsStore store;
    if (accountsPresent) store.accounts = parseStoredAccounts(rawAccounts);
    if (unsupportedPresent) store.unsupported = parseStoredUnsupported(rawUnsupported);
    return store;
  }

  CodexAccountModelsStore mergeCodexAccountCatalogs(const CodexAccountModelsStore& store, const std::vector<CodexAccountCatalogInput>& entries, double nowMs) {
    if (entries.empty()) return store;
    Record<CodexAccountModelCatalog> accounts = store.accounts;
    Record<Record<double>> unsupported;
    for (const std::string& accountId : store.unsupported.keys()) unsupported.set(accountId, *store.unsupported.get(accountId));
    for (const CodexAccountCatalogInput& entry : entries) {
      std::vector<std::string> slugs = uniqueSlugs(entry.slugs);
      accounts.set(entry.accountId, CodexAccountModelCatalog{
                                      .client_version = entry.clientVersion,
                                      .slugs = slugs,
                                      .updated_at_ms = entry.updatedAtMs.value_or(nowMs),
                                    });
      Record<double>* models = unsupported.get(entry.accountId);
      if (models == nullptr) continue;
      const std::unordered_set<std::string> present(slugs.begin(), slugs.end());
      for (const std::string& model : models->keys()) {
        if (present.contains(model)) deleteProperty(*models, model);
      }
      if (!models->size()) deleteProperty(unsupported, entry.accountId);
    }
    return CodexAccountModelsStore{.accounts = std::move(accounts), .unsupported = std::move(unsupported)};
  }

  CodexAccountModelsStore withCodexModelUnsupported(const CodexAccountModelsStore& store, const std::string& accountId, const std::string& model, double observedAtMs) {
    return withCodexModelUnsupported(store, accountId, model, observedAtMs, observedAtMs);
  }

  CodexAccountModelsStore withCodexModelUnsupported(const CodexAccountModelsStore& store, const std::string& accountId, const std::string& model, double observedAtMs,
                                                    double nowMs) {
    const Record<double>* existing = store.unsupported.get(accountId);
    Record<double> models = existing != nullptr ? *existing : Record<double>();
    models.define(model, observedAtMs);
    std::vector<std::pair<std::string, double>> kept;
    for (const std::string& key : models.keys()) {
      const double observed = *models.get(key);
      if (nowMs - observed <= CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_AGE_MS) kept.emplace_back(key, observed);
    }
    rt::sort(kept, [](const std::pair<std::string, double>& left, const std::pair<std::string, double>& right) -> double {
      const double newer = right.second - left.second;
      if (rt::truthy(newer)) return newer;
      return static_cast<double>(rt::localeCompare(left.first, right.first));
    });
    if (static_cast<double>(kept.size()) > CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_COUNT) kept.resize(static_cast<std::size_t>(CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_COUNT));
    Record<double> bounded;
    for (const auto& [key, observed] : kept) bounded.define(key, observed);
    CodexAccountModelsStore next{.accounts = store.accounts, .unsupported = store.unsupported};
    next.unsupported.define(accountId, std::move(bounded));
    return next;
  }

  // ── In-memory cache ──────────────────────────────────────────────────────────
  //
  // Ordinary routing evaluates accounts synchronously, so it reads this snapshot
  // only. A stale snapshot schedules one background KV read and keeps serving
  // until it lands: dropping to "unknown" here would forget a learned rejection
  // after the TTL instead of refreshing it.

  namespace {
    struct CodexAccountModelsCache {
      std::optional<CodexAccountModelsStore> store;
      double loadedAtMs;
    };

    CodexAccountModelsCache cache{.store = std::nullopt, .loadedAtMs = 0};
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> learnedDetails;
    std::optional<rt::Promise<void>> refreshInFlight;
    std::size_t learnedCount = 0;
    std::size_t learnedBytes = 0;

    void cacheAccountModels(std::optional<CodexAccountModelsStore> store) {
      const auto bytes = store ? sizeof(CodexAccountModelsStore) + rt::memory::bytes(toJson(*store)) : 0;
      cache.store = std::move(store);
      rt::memory::setCache(rt::memory::Cache::codex_account_models, cache.store ? 1 : 0, bytes);
    }

    rt::Task<std::shared_ptr<rt::Kv>> resolveKv(std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
      if (kvOverride) co_return *kvOverride;
      rt::Promise<std::shared_ptr<rt::Kv>> pending = ::uos::kv::getKv();
      std::shared_ptr<rt::Kv> kv = co_await pending.wait();
      co_return kv;
    }

    rt::Task<std::optional<CodexAccountModelsStore>> loadCodexAccountModelsFromKv(std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
      rt::Task<std::shared_ptr<rt::Kv>> resolving = resolveKv(std::move(kvOverride));
      const std::shared_ptr<rt::Kv> kv = co_await std::move(resolving);
      if (!kv) co_return std::nullopt;
      const rt::KvKey key = CODEX_ACCOUNT_MODELS_KV_KEY;
      rt::KvGetOptions strong;
      strong.consistency = rt::KvConsistencyLevel::strong;
      rt::Task<rt::KvEntryMaybe<>> reading = kv->get(key, strong);
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      if (entry.value.isNull()) co_return emptyCodexAccountModelsStore();
      co_return parseCodexAccountModelsStore(entry.value);
    }
  }

  rt::Task<std::optional<CodexAccountModelsStore>> loadCodexAccountModels() { return loadCodexAccountModels(std::nullopt); }

  rt::Task<std::optional<CodexAccountModelsStore>> loadCodexAccountModels(std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    std::optional<CodexAccountModelsStore> store;
    bool failed = false;
    try {
      rt::Task<std::optional<CodexAccountModelsStore>> loading = loadCodexAccountModelsFromKv(std::move(kvOverride));
      store = co_await std::move(loading);
    } catch (...) {
      failed = true;
    }
    if (failed) co_return cache.store;
    if (store) {
      cacheAccountModels(store);
      cache.loadedAtMs = rt::dateNow();
    }
    co_return store;
  }

  namespace {
    void scheduleCodexAccountModelsRefresh() {
      if (refreshInFlight) return;
      const rt::Promise<std::optional<CodexAccountModelsStore>> loading(loadCodexAccountModels());
      refreshInFlight = loading.then([](const std::optional<CodexAccountModelsStore>&) {}).catch_([](const rt::Reason&) {}).finally_([] { refreshInFlight.reset(); });
    }

    const CodexAccountModelsStore* currentCodexAccountModelsStore(double nowMs) {
      if (!cache.store || nowMs - cache.loadedAtMs > CODEX_ACCOUNT_MODELS_CACHE_TTL_MS) scheduleCodexAccountModelsRefresh();
      return cache.store ? &*cache.store : nullptr;
    }

    /** A bounded retry budget, mirroring the catalog snapshot writer's commit loop. */
    constexpr double CODEX_ACCOUNT_MODELS_COMMIT_ATTEMPTS = 3;

    using Merge = std::function<CodexAccountModelsStore(const CodexAccountModelsStore& current)>;

    /**
     * Persist one merged update with a compare-and-set commit, never letting an
     * availability failure affect serving. Catalog refreshes and learned rejections
     * run concurrently in independent isolates, so an unconditional `set` would let
     * the last writer discard the other's rows -- or resurrect a rejection the
     * fresh catalog had already cleared. Every attempt re-reads the durable store
     * strongly, merges on the value it just read, and commits only while that exact
     * version is still current; the in-memory cache follows the committed value.
     */
    rt::Task<void> mergeAndStoreCodexAccountModels(Merge merge, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
      try {
        rt::Task<std::shared_ptr<rt::Kv>> resolving = resolveKv(std::move(kvOverride));
        const std::shared_ptr<rt::Kv> kv = co_await std::move(resolving);
        if (!kv) {
          // Without a durable store the hint stays isolate-local, as before.
          CodexAccountModelsStore local = merge(cache.store ? *cache.store : emptyCodexAccountModelsStore());
          cacheAccountModels(std::move(local));
          cache.loadedAtMs = rt::dateNow();
          co_return;
        }
        const rt::KvKey key = CODEX_ACCOUNT_MODELS_KV_KEY;
        rt::KvGetOptions strong;
        strong.consistency = rt::KvConsistencyLevel::strong;
        for (double attempt = 0; attempt < CODEX_ACCOUNT_MODELS_COMMIT_ATTEMPTS; attempt += 1) {
          rt::Task<rt::KvEntryMaybe<>> reading = kv->get(key, strong);
          const rt::KvEntryMaybe<> entry = co_await std::move(reading);
          // An absent or unreadable durable value merges from an empty store: the
          // base is always the value just read, never a possibly-stale snapshot.
          std::optional<CodexAccountModelsStore> parsed = parseCodexAccountModelsStore(entry.value);
          const CodexAccountModelsStore current = parsed ? std::move(*parsed) : emptyCodexAccountModelsStore();
          CodexAccountModelsStore next = merge(current);
          std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
          operation->check(entry).set(key, rt::KvValue(toJson(next)));
          rt::Task<rt::KvCommitResult> committing = operation->commit();
          const rt::KvCommitResult committed = co_await std::move(committing);
          if (!committed.ok) continue;
          cacheAccountModels(std::move(next));
          cache.loadedAtMs = rt::dateNow();
          co_return;
        }
      } catch (...) {
        // Availability evidence is a hint; a KV failure only loses the hint.
      }
    }
  }

  rt::Task<void> recordCodexAccountCatalogs(std::vector<CodexAccountCatalogInput> entries) { return recordCodexAccountCatalogs(std::move(entries), std::nullopt); }

  rt::Task<void> recordCodexAccountCatalogs(std::vector<CodexAccountCatalogInput> entries, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    if (entries.empty()) co_return;
    const double nowMs = rt::dateNow();
    Merge merge = [entries, nowMs](const CodexAccountModelsStore& current) { return mergeCodexAccountCatalogs(current, entries, nowMs); };
    rt::Task<void> storing = mergeAndStoreCodexAccountModels(std::move(merge), std::move(kvOverride));
    co_await std::move(storing);
  }

  rt::Task<void> recordCodexModelUnsupported(std::string accountId, std::string model) {
    return recordCodexModelUnsupported(std::move(accountId), std::move(model), CodexModelUnsupportedOptions{});
  }

  rt::Task<void> recordCodexModelUnsupported(std::string accountId, std::string model, CodexModelUnsupportedOptions options) {
    const std::string target = rt::trim(model);
    if (accountId.empty() || target.empty()) co_return;
    const double nowMs = options.nowMs ? *options.nowMs : rt::dateNow();
    if (options.detail && !options.detail->empty()) {
      std::unordered_map<std::string, std::string>& details = learnedDetails[accountId];
      auto entry = details.find(target);
      if (entry == details.end()) {
        entry = details.emplace(target, *options.detail).first;
        learnedCount += 1;
        learnedBytes += sizeof(*entry) + rt::memory::bytes(entry->first) + rt::memory::bytes(entry->second);
      } else {
        const auto prior = rt::memory::bytes(entry->second);
        entry->second = *options.detail;
        learnedBytes += rt::memory::bytes(entry->second) - prior;
      }
      rt::memory::setCache(rt::memory::Cache::codex_learned_model_details, learnedCount, learnedBytes);
    }
    Merge merge = [accountId, target, nowMs](const CodexAccountModelsStore& current) { return withCodexModelUnsupported(current, accountId, target, nowMs, nowMs); };
    rt::Task<void> storing = mergeAndStoreCodexAccountModels(std::move(merge), std::move(options.kv));
    co_await std::move(storing);
  }

  namespace {
    /** Whether a sibling's same-version catalog proves the model is servable somewhere. */
    bool siblingListsModel(const CodexAccountModelsStore& store, const std::string& other, const CodexAccountModelCatalog& catalog, const std::string& target) {
      const CodexAccountModelCatalog* sibling = store.accounts.get(other);
      if (sibling == nullptr || sibling->client_version != catalog.client_version) return false;
      return std::find(sibling->slugs.begin(), sibling->slugs.end(), target) != sibling->slugs.end();
    }
  }

  rt::OrderedMap<std::string, std::optional<std::string>> codexModelUnavailableAccounts(const std::optional<std::string>& model,
                                                                                        const std::vector<std::string>& currentAccountIds) {
    rt::OrderedMap<std::string, std::optional<std::string>> unavailable;
    const std::string target = model ? rt::trim(*model) : std::string();
    if (target.empty()) return unavailable;
    const double nowMs = rt::dateNow();
    const CodexAccountModelsStore* store = currentCodexAccountModelsStore(nowMs);
    if (store == nullptr) return unavailable;
    std::vector<std::string> accountIds;
    std::unordered_set<std::string> seen;
    for (const std::string& accountId : currentAccountIds)
      if (seen.insert(accountId).second) accountIds.push_back(accountId);
    for (const std::string& accountId : accountIds) {
      const Record<double>* unsupported = store->unsupported.get(accountId);
      const double* observedAtMs = unsupported == nullptr ? nullptr : unsupported->get(target);
      if (observedAtMs != nullptr && nowMs - *observedAtMs <= CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_AGE_MS) {
        std::optional<std::string> detail;
        if (const auto details = learnedDetails.find(accountId); details != learnedDetails.end()) {
          if (const auto learned = details->second.find(target); learned != details->second.end()) detail = learned->second;
        }
        unavailable.set(accountId, std::move(detail));
        continue;
      }
      const CodexAccountModelCatalog* catalog = store->accounts.get(accountId);
      if (catalog == nullptr || std::find(catalog->slugs.begin(), catalog->slugs.end(), target) != catalog->slugs.end()) continue;
      if (std::any_of(accountIds.begin(), accountIds.end(),
                      [&](const std::string& other) { return other != accountId && siblingListsModel(*store, other, *catalog, target); }))
        unavailable.set(accountId, std::nullopt);
    }
    return unavailable;
  }

  // ── Test hooks ───────────────────────────────────────────────────────────────

  void setCodexAccountModelsStoreForTest(std::optional<CodexAccountModelsStore> store) { setCodexAccountModelsStoreForTest(std::move(store), rt::dateNow()); }

  void setCodexAccountModelsStoreForTest(std::optional<CodexAccountModelsStore> store, double loadedAtMs) {
    const bool absent = !store;
    cacheAccountModels(std::move(store));
    cache.loadedAtMs = absent ? 0 : loadedAtMs;
  }

  CodexAccountModelsCacheForTest getCodexAccountModelsCacheForTest() {
    return CodexAccountModelsCacheForTest{.store = cache.store, .loadedAtMs = cache.loadedAtMs, .refreshing = refreshInFlight.has_value()};
  }

  void resetCodexAccountModelsCacheForTest() {
    cacheAccountModels(std::nullopt);
    cache.loadedAtMs = 0;
    learnedDetails.clear();
    learnedCount = learnedBytes = 0;
    rt::memory::setCache(rt::memory::Cache::codex_learned_model_details, 0, 0);
    refreshInFlight.reset();
  }

  rt::Task<void> awaitCodexAccountModelsRefreshForTest() {
    if (!refreshInFlight) {
      co_await rt::microtask();
      co_return;
    }
    const rt::Promise<void> pending = *refreshInFlight;
    co_await pending.wait();
  }
}
