#pragma once
// Port of src/models/codex-models-availability.ts.
// Per-account Codex model availability: the catalog each subscription account
// advertised and the account+model pairs upstream has rejected as unsupported.
// The store is a non-secret routing hint; raw tokens never enter it.
#include "uos/authority/contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::models::codex_models_availability {
  inline const rt::KvKey CODEX_ACCOUNT_MODELS_KV_KEY{"uos_ai", "codex_account_models", "v1"};
  /** Per-request routing reads this isolate-local snapshot, never KV, while it is fresh. */
  inline constexpr double CODEX_ACCOUNT_MODELS_CACHE_TTL_MS = 30'000;
  /** Bound the learned map per account: newest entries win, old observations age out. */
  inline constexpr double CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_COUNT = 32;
  inline constexpr double CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_AGE_MS = 7 * 24 * 60 * 60'000;

  struct CodexAccountModelCatalog {
    std::string client_version;
    std::vector<std::string> slugs;
    double updated_at_ms;
    friend bool operator==(const CodexAccountModelCatalog&, const CodexAccountModelCatalog&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAccountModelCatalog& value);
  CodexAccountModelCatalog tag_invoke(const json::value_to_tag<CodexAccountModelCatalog>&, const json::value& value);

  // `Readonly<Record<string, T>>`: a plain JS object (JS property order, [[Set]] of "__proto__").
  struct CodexAccountModelsStore {
    authority::contracts::Record<CodexAccountModelCatalog> accounts;
    authority::contracts::Record<authority::contracts::Record<double>> unsupported;
    friend bool operator==(const CodexAccountModelsStore&, const CodexAccountModelsStore&) = default;
  };

  // The objects as the TS holds them (what kv.set stores and JSON.stringify writes): members
  // in declaration order, record keys in JS property order.
  json::object toJson(const CodexAccountModelCatalog& catalog);
  json::object toJson(const CodexAccountModelsStore& store);

  CodexAccountModelsStore emptyCodexAccountModelsStore();

  /**
   * Parse the durable store defensively: a value that cannot be a store at all is
   * rejected, while individually malformed entries are dropped so one bad row
   * cannot erase every other account's evidence.
   */
  // `value: unknown` is a stored value; std::nullopt is null.
  std::optional<CodexAccountModelsStore> parseCodexAccountModelsStore(const rt::KvValue& value);

  struct CodexAccountCatalogInput {
    std::string accountId;
    std::string clientVersion;
    // `readonly unknown[]`
    json::array slugs;
    std::optional<double> updatedAtMs{};
  };

  /**
   * Merge fresh per-account catalogs. A slug now present in an account's own
   * catalog clears that account's learned rejection for the same model, which
   * keeps the learned signal from outliving the upstream capability it described.
   */
  CodexAccountModelsStore mergeCodexAccountCatalogs(const CodexAccountModelsStore& store, const std::vector<CodexAccountCatalogInput>& entries, double nowMs);

  /** Record one learned upstream rejection, pruning the account's map by age and count. */
  // `nowMs = observedAtMs`: the four-argument overload.
  CodexAccountModelsStore withCodexModelUnsupported(const CodexAccountModelsStore& store, const std::string& accountId, const std::string& model, double observedAtMs);
  CodexAccountModelsStore withCodexModelUnsupported(const CodexAccountModelsStore& store, const std::string& accountId, const std::string& model, double observedAtMs,
                                                    double nowMs);

  // `kvOverride?: Kv | null`: the overload without it is undefined (the process KV from getKv());
  // std::nullopt is undefined too, and a null pointer is null (no durable store). The result
  // std::nullopt is null.
  [[nodiscard]] rt::Task<std::optional<CodexAccountModelsStore>> loadCodexAccountModels();
  [[nodiscard]] rt::Task<std::optional<CodexAccountModelsStore>> loadCodexAccountModels(std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  /** Record the catalogs every pool account advertised for one client version. */
  [[nodiscard]] rt::Task<void> recordCodexAccountCatalogs(std::vector<CodexAccountCatalogInput> entries);
  [[nodiscard]] rt::Task<void> recordCodexAccountCatalogs(std::vector<CodexAccountCatalogInput> entries, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  // `Readonly<{ detail?: string | null; nowMs?: number; kv?: Kv | null }>`. Only the truthiness of
  // `detail` is read, so null and undefined are both std::nullopt; `kv`: std::nullopt is undefined
  // (the process KV), a null pointer is null.
  struct CodexModelUnsupportedOptions {
    std::optional<std::string> detail{};
    std::optional<double> nowMs{};
    std::optional<std::shared_ptr<rt::Kv>> kv{};
  };

  /** Record one account+model pair upstream rejected, with the detail text for diagnostics. */
  // `options = {}`: the two-argument overload.
  [[nodiscard]] rt::Task<void> recordCodexModelUnsupported(std::string accountId, std::string model);
  [[nodiscard]] rt::Task<void> recordCodexModelUnsupported(std::string accountId, std::string model, CodexModelUnsupportedOptions options);

  /**
   * Accounts in the current pool that cannot serve a named model, mapped to the
   * upstream detail when one was learned. Only current pool accounts are
   * considered: a removed or replaced account's stored catalog or rejection says
   * nothing about the accounts serving now. A stored catalog proves absence only
   * against a current sibling's catalog for the same client version, and a
   * learned rejection ages back to unknown availability after
   * CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_AGE_MS instead of excluding the account
   * forever.
   */
  // `model: string | null` (std::nullopt is null); the detail std::nullopt is null.
  rt::OrderedMap<std::string, std::optional<std::string>> codexModelUnavailableAccounts(const std::optional<std::string>& model,
                                                                                        const std::vector<std::string>& currentAccountIds);

  // ── Test hooks ───────────────────────────────────────────────────────────────

  // `store: CodexAccountModelsStore | null` (std::nullopt is null); `loadedAtMs = Date.now()`:
  // the one-argument overload.
  void setCodexAccountModelsStoreForTest(std::optional<CodexAccountModelsStore> store);
  void setCodexAccountModelsStoreForTest(std::optional<CodexAccountModelsStore> store, double loadedAtMs);

  struct CodexAccountModelsCacheForTest {
    std::optional<CodexAccountModelsStore> store;
    double loadedAtMs;
    bool refreshing;
  };
  CodexAccountModelsCacheForTest getCodexAccountModelsCacheForTest();

  void resetCodexAccountModelsCacheForTest();

  [[nodiscard]] rt::Task<void> awaitCodexAccountModelsRefreshForTest();
}
