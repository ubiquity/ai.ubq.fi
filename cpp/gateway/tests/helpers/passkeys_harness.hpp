#pragma once
// Port of tests/helpers/passkeys-harness.ts, the part its ported modules allow: the shared KV stub
// (`kvStore`, `kvVersions`, `kvStub`, the commit and read seams), its key helpers, `seedPasskeySession`,
// `withEnv`, the base64url and URL helpers and the install/reset/cleanup hooks. The passkeys exports
// it re-exports come from uos/auth/passkeys.hpp and `config` from uos/config.hpp.
// Auth/index and Metered quota re-exports are available through their defining modules.
//
// Timing: like the TS stub (async `get`, `Promise.resolve(...)` elsewhere), every call does its work
// when it runs and its result arrives one microtask later (`get` first sleeps kvGetDelayMs).
#include "uos/auth/passkeys.hpp"
#include "uos/auth/index.hpp"
#include "uos/metered_quota.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uos::tests::helpers::passkeys_harness {
  using auth::index::authenticateAdmin;
  using auth::index::authenticateClient;
  using auth::index::handleV1Auth;
  using auth::index::requireAdminAuth;
  using metered_quota::METERED_QUOTA_FRESH_MS;
  using metered_quota::METERED_QUOTA_STATE_KEY;
  inline rt::Reason abortError(rt::Reason reason) {
    if (rt::isError(reason)) return reason;
    return rt::makeReason(rt::withCause(rt::JsError("Aborted", "AbortError"), reason));
  }

  // Removes every trailing `=`.
  std::string stripBase64Padding(std::string_view value);

  // Base64url encoding of `value` (a binary string, as btoa takes).
  std::string encodeBase64Url(std::string_view value);

  // `JSON.stringify(key)`. A bigint or byte-array part throws TypeError (JSON.stringify cannot write it).
  std::string keyToString(const rt::KvKey& key);

  // `requestUrl(input: RequestInfo | URL)`
  std::string requestUrl(const rt::RequestInfo& input);

  // Bumped by every KvTestStore::set, cleared with the store.
  extern rt::OrderedMap<std::string, double> kvVersions;

  // `setBeforeAtomicCommit(value)`: runs once, at the next commit, before its checks.
  void setBeforeAtomicCommit(std::function<void()> value);
  void setKvGetDelayMs(double value);

  // `class KvTestStore extends Map<string, unknown>`: `set` bumps the key's version; `clear()`
  // also drops the commit hook, the read delay and every version.
  class KvTestStore : public rt::OrderedMap<std::string, rt::KvValue> {
  public:
    KvTestStore& set(const std::string& key, rt::KvValue value);
    void clear();
  };

  // The shared store, keyed by keyToString(key). Touched only after main starts.
  extern KvTestStore kvStore;

  // `String(kvVersions.get(rawKey) ?? 0).padStart(20, "0")` while the key is stored; null (std::nullopt) otherwise.
  std::optional<std::string> versionstampFor(const std::string& rawKey);

  // `kvStub`: get / set / delete / list (prefix and limit; every entry carries versionstamp
  // "00000000000000010000") / atomic (check / set / delete / commit) / close. The same instance
  // every call.
  std::shared_ptr<rt::Kv> kvStub();

  // `withEnv(updates, fn)`: null (std::nullopt) deletes; the previous values come back afterwards.
  rt::Task<void> withEnv(std::vector<std::pair<std::string, std::optional<std::string>>> updates, std::function<rt::Task<void>()> fn);

  // `seedPasskeySession(token, { isAdmin, audienceOrigin })`
  struct SeedPasskeySessionOptions {
    bool isAdmin = true;
    std::string audienceOrigin{};
  };
  struct SeededPasskeySession {
    std::string token;
    auth::passkeys::PasskeyUserRecord user;
  };
  SeededPasskeySession seedPasskeySession(std::string token = "uos_ai_session_test", SeedPasskeySessionOptions options = {});

  /** Reinstall this fixture explicitly when a Bun test file uses the cached harness. */
  void installHarness();
  void resetHarness();
  void cleanupHarness();
}
