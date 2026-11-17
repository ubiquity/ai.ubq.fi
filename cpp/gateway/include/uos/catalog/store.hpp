#pragma once
// Port of src/catalog/store.ts.
// Codex catalog KV storage, leases and loading, split out of src/codex_catalog.ts.
// `kv: Kv` parameters are `rt::Kv&`: the store must outlive every task (and a lease
// heartbeat until its stop() completed or it reported the lease lost).
#include "uos/catalog/types.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::catalog::store {
  void resetCodexCatalogMemoForTest();

  std::vector<std::string> getCodexCatalogMemoVersionsForTest();

  // `Record<string, unknown> | null`
  std::optional<json::object> parseCatalogBody(std::string_view body);

  // `LoadedCodexCatalog | null`. The memo hands out copies of the memoized catalog.
  rt::Task<std::optional<types::LoadedCodexCatalog>> loadCatalog(rt::Kv& kv, std::string version, std::string authGeneration, double nowMs);

  // Readonly<{ clientVersion: string; authGeneration: string; body: string; etag?: string | null;
  //            contentType?: string | null; fetchedAtMs?: number }>
  struct StoreCodexCatalogInput {
    std::string clientVersion;
    std::string authGeneration;
    std::string body;
    std::optional<std::string> etag{};         // null and undefined alike
    std::optional<std::string> contentType{};  // null and undefined alike
    std::optional<double> fetchedAtMs{};
  };

  rt::Task<bool> storeCodexCatalog(rt::Kv& kv, StoreCodexCatalogInput input);

  // A stored generation that is truthy but not a string has no C++ value: it throws
  // rt::TypeError("KV value is <kind>, not string") where the TS returns it.
  rt::Task<std::string> getAuthGeneration(rt::Kv& kv);

  rt::Task<bool> acquireRefreshLease(rt::Kv& kv, std::string version, std::string owner, double nowMs);

  // The `{ lost: () => boolean; stop: () => Promise<void> }` handle.
  struct RefreshLeaseHeartbeat {
    std::function<bool()> lost;
    std::function<rt::Task<void>()> stop;
  };

  RefreshLeaseHeartbeat startRefreshLeaseHeartbeat(rt::Kv& kv, std::string version, std::string owner);

  rt::Task<void> releaseRefreshLease(rt::Kv& kv, std::string version, std::string owner);

  rt::Task<bool> authGenerationIsCurrent(rt::Kv& kv, std::string expected);

  rt::Task<std::optional<types::LoadedCodexCatalog>> loadCurrentGenerationCatalog(rt::Kv& kv, std::string version);

  rt::Task<std::optional<types::LoadedCodexCatalog>> waitForColdCatalog(rt::Kv& kv, std::string version);

  // `export { CODEX_CATALOG_AUTH_GENERATION_KEY };`
  using types::CODEX_CATALOG_AUTH_GENERATION_KEY;
}
