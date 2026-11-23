// Local native Codex credential ownership: pool binding, external adoption and owned refresh.
#include "uos/codex/native_auth.hpp"

#include "uos/codex/external_auth.hpp"
#include "uos/codex/native_auth_transport.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/upstream_auth.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace uos::codex::native_auth {
  namespace {
    using auth::CodexError;
    using ::uos::types::CodexAuthPoolState;
    using ::uos::types::CodexAuthState;

    std::optional<NativeAuthHooks> hooks;

    using GetEnv = std::function<std::optional<std::string>(std::string name)>;

    // `hooks?.getEnv ?? ((name: string) => process.env[name])`
    GetEnv environmentReader() {
      if (hooks && hooks->getEnv) return hooks->getEnv;
      return [](std::string name) { return rt::env::get(name); };
    }

    std::optional<std::string> nativeHome() {
      if (hooks && rt::truthy(hooks->codexHome)) return hooks->codexHome;
      const std::string os = hooks && hooks->os ? *hooks->os : std::string(rt::process::platform);
      // Darwin keeps its original single-home behavior. Linux uses the same single home so the
      // VPS gateway can delegate refresh to the local daemon; multiple homes stay out of scope,
      // and a credential-home path is never trusted from a remote upload.
      if ((os != "darwin" && os != "linux") || config::config.isDeploy) return std::nullopt;
      try {
        const GetEnv getEnv = environmentReader();
        const std::optional<std::string> codexHome = getEnv("CODEX_HOME");
        if (rt::truthy(codexHome)) return codexHome;
        const std::optional<std::string> home = getEnv("HOME");
        return rt::truthy(home) ? std::optional<std::string>(*home + "/.codex") : std::nullopt;
      } catch (...) {
        return std::nullopt;
      }
    }

    bool sameCredentials(const CodexAuthState& left, const CodexAuthState& right) {
      return left.access_token == right.access_token && left.refresh_token == right.refresh_token;
    }

    // `{ auth: CodexAuthState; email: string | null; refreshedAt: string | null; externalUserId?: string }`
    struct NativeDocument {
      CodexAuthState auth;
      std::optional<std::string> email;
      std::optional<std::string> refreshedAt;
      std::optional<std::string> externalUserId{};
    };

    bool isExternalOwner(const std::optional<CodexAuthState::NativeOwner>& owner) {
      return owner && owner->auth_mode == std::optional<std::string>("chatgptAuthTokens");
    }

    rt::Task<NativeDocument> readNativeDocument(std::string home) {
      json::value raw;
      if (hooks && hooks->readAuth) {
        const std::function<rt::Task<json::value>()> readAuth = hooks->readAuth;
        auto reading = readAuth();
        raw = co_await std::move(reading);
      } else {
        std::string text;
        rt::Reason failure;
        try {
          text = rt::fs::promises::readFile(home + "/auth.json", "utf8");
        } catch (...) {
          failure = std::current_exception();
        }
        co_await rt::microtask();
        if (failure) std::rethrow_exception(failure);
        raw = rt::jsonParse(text);
      }
      const std::optional<external_auth::ExternalNativeCodexDocument> external = external_auth::parseExternalNativeCodexDocument(raw);
      if (external) {
        NativeDocument document;
        document.auth =
          CodexAuthState{.access_token = external->auth.access_token, .refresh_token = external->auth.refresh_token, .account_id = external->auth.account_id,
                         .updated_at_ms = rt::dateNow()};
        document.email = auth::getCodexAccountEmail(external->auth.access_token);
        document.refreshedAt = external->refreshedAt;
        document.externalUserId = external->userId;
        co_return document;
      }
      const std::optional<auth::ParseCodexAuthFromAuthJsonResult> parsed = auth::parseCodexAuthFromAuthJson(raw);
      const json::value* authMode = rt::get(raw, "auth_mode");
      if (!parsed || !utils::isRecord(raw) || (authMode != nullptr && !(authMode->is_string() && authMode->get_string() == "chatgpt"))) {
        throw CodexError("Native Codex credentials are not managed ChatGPT auth.", "codex_auth_owner_unavailable", 503);
      }
      const json::value* tokens = utils::isRecord(rt::get(raw, "tokens")) ? rt::get(raw, "tokens") : nullptr;
      const std::optional<std::string> idToken = tokens != nullptr ? utils::getString(rt::get(tokens, "id_token")) : std::nullopt;
      const std::optional<std::string> refreshedAt = utils::getString(rt::get(raw, "last_refresh"));
      NativeDocument document;
      document.auth = CodexAuthState{.access_token = parsed->access_token, .refresh_token = parsed->refresh_token, .account_id = parsed->account_id,
                                     .updated_at_ms = rt::dateNow(), .native_owner = parsed->native_owner};
      document.email = rt::truthy(idToken) ? auth::getCodexAccountEmail(*idToken) : std::nullopt;
      document.refreshedAt = rt::truthy(refreshedAt) && rt::Number::isFinite(rt::Date::parse(*refreshedAt)) ? refreshedAt : std::nullopt;
      co_return document;
    }

    /** Preserve native sub-millisecond generation order when two JWTs share expiry. */
    // `value: string | null | undefined`
    std::optional<rt::BigInt> nativeRefreshTime(const std::optional<std::string>& value) {
      if (!rt::truthy(value)) return std::nullopt;
      const double milliseconds = rt::Date::parse(*value);
      if (!rt::Number::isFinite(milliseconds)) return std::nullopt;
      static const rt::Regex FRACTIONAL_SECONDS("\\.(\\d{1,9})(?:Z|[+-]\\d{2}:\\d{2})$", "");
      const std::optional<rt::RegexMatch> match = FRACTIONAL_SECONDS.exec(*value);
      const std::string fractional = match && (*match)[1] ? *(*match)[1] : std::string();
      return rt::BigInt::fromNumber(milliseconds) * rt::BigInt(1'000'000) + rt::BigInt::fromString(rt::slice(rt::padEnd(fractional, 9, "0"), 3, 9));
    }

    void verifyExternalNativeIdentity(const CodexAuthState& previous, const NativeDocument& document) {
      if (!rt::truthy(document.externalUserId) && !isExternalOwner(previous.native_owner)) return;
      const std::optional<external_auth::ExternalCodexIdentity> identity = external_auth::externalCodexIdentity(previous.access_token);
      if (!rt::truthy(document.externalUserId) || !identity || *document.externalUserId != identity->userId || identity->accountId != previous.account_id) {
        throw CodexError("External native Codex user or workspace changed; nothing was adopted.", "codex_auth_owner_conflict", 409);
      }
    }

    void verifyNativeProgress(const CodexAuthState& previous, const NativeDocument& document, bool requireUsableAccessToken = true) {
      const CodexAuthState& next = document.auth;
      verifyExternalNativeIdentity(previous, document);
      const std::optional<double> previousExpiry = auth::getJwtExpMs(previous.access_token);
      const std::optional<double> nextExpiry = auth::getJwtExpMs(next.access_token);
      const std::optional<rt::BigInt> priorNativeTime = nativeRefreshTime(previous.native_owner ? previous.native_owner->native_refreshed_at : std::nullopt);
      const std::optional<rt::BigInt> nextNativeTime = nativeRefreshTime(document.refreshedAt);
      const bool advancedNativeTime = priorNativeTime && nextNativeTime && *nextNativeTime > *priorNativeTime;
      if (next.account_id != previous.account_id || !rt::truthy(nextExpiry) || (!requireUsableAccessToken && !previousExpiry && !advancedNativeTime) ||
          (requireUsableAccessToken && *nextExpiry <= rt::dateNow() + 120'000) ||
          (previousExpiry && (*nextExpiry < *previousExpiry || (*nextExpiry == *previousExpiry && !advancedNativeTime)))) {
        throw CodexError("Native Codex credential generation did not advance; no stale credentials were adopted.", "codex_auth_refresh_failed", 503);
      }
    }

    rt::Task<CodexAuthState::NativeOwner> nativeOwnerForDocument(NativeDocument document, std::string home) {
      rt::Promise<std::string> generating(nativeCodexCredentialGeneration(document.auth));
      const std::string generationHash = co_await generating.wait();
      CodexAuthState::NativeOwner owner;
      owner.codex_home = home;
      owner.generation_hash = generationHash;
      if (rt::truthy(document.refreshedAt)) owner.native_refreshed_at = document.refreshedAt;
      if (rt::truthy(document.externalUserId)) {
        owner.auth_mode = std::string("chatgptAuthTokens");
        owner.chatgpt_user_id = document.externalUserId;
      }
      co_return owner;
    }

    std::optional<std::size_t> accountIndex(const CodexAuthPoolState& pool, const std::string& accountId) {
      for (std::size_t index = 0; index < pool.accounts.size(); ++index)
        if (pool.accounts[index].account_id == accountId) return index;
      return std::nullopt;
    }

    rt::Task<rt::KvEntryMaybe<json::value>> readPoolEntry(std::shared_ptr<rt::Kv> kv) {
      const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
      auto reading = kv->get(auth::CODEX_AUTH_POOL_KV_KEY, strong);
      const rt::KvEntryMaybe<> stored = co_await std::move(reading);
      co_return stored.as<json::value>();
    }

    rt::Task<CodexAuthState> persistNativeGeneration(auth::CodexAuthAccountEntry current, NativeDocument document, std::string home) {
      if (!current.kv || !current.entry) throw CodexError("Native Codex ownership requires the durable auth pool.", "codex_auth_refresh_failed", 503);
      rt::KvEntryMaybe<json::value> entry = *current.entry;
      for (int attempt = 0; attempt < 3; attempt += 1) {
        const std::optional<CodexAuthPoolState> pool = auth::parseCodexAuthPool(entry.value);
        const std::optional<std::size_t> index = pool ? accountIndex(*pool, current.auth.account_id) : std::nullopt;
        if (!pool || !index) throw CodexError("Native Codex account was removed during refresh.", "codex_auth_missing", 503);
        const CodexAuthState previous = pool->accounts[*index];
        bool ownershipChanged = !previous.native_owner || previous.native_owner->codex_home != home;
        if (!ownershipChanged) {
          rt::Promise<std::string> generating(nativeCodexCredentialGeneration(previous));
          const std::string generation = co_await generating.wait();
          ownershipChanged = previous.native_owner->generation_hash != generation;
        }
        if (ownershipChanged) throw CodexError("Native Codex credential ownership changed during refresh.", "codex_auth_owner_unavailable", 503);
        if (sameCredentials(previous, document.auth)) {
          auth::cacheCodexAuthPool(*pool);
          co_return previous;
        }
        verifyNativeProgress(previous, document);
        rt::Promise<CodexAuthState::NativeOwner> owning(nativeOwnerForDocument(document, home));
        const CodexAuthState::NativeOwner owner = co_await owning.wait();
        CodexAuthState next = document.auth;
        next.native_owner = owner;
        std::vector<CodexAuthState> accounts = pool->accounts;
        accounts[*index] = next;
        const CodexAuthPoolState nextPool{.accounts = std::move(accounts), .updated_at_ms = rt::dateNow()};
        const rt::KvValue row(json::value_from(nextPool));
        std::unique_ptr<rt::AtomicOperation> operation = current.kv->atomic();
        operation->check(entry).set(auth::CODEX_AUTH_POOL_KV_KEY, row);
        auto committing = operation->commit();
        const rt::KvCommitResult commit = co_await std::move(committing);
        if (commit.ok) {
          auth::cacheCodexAuthPool(nextPool);
          co_return next;
        }
        auto rereading = readPoolEntry(current.kv);
        entry = co_await std::move(rereading);
      }
      throw CodexError("Native Codex refresh could not persist after concurrent changes.", "codex_auth_refresh_failed", 503);
    }

    rt::Task<CodexAuthState> verifyNativeAccountBinding(CodexAuthPoolState pool, std::string accountId, std::string home, NativeDocument document) {
      const auto found = std::find_if(pool.accounts.begin(), pool.accounts.end(), [&accountId](const CodexAuthState& candidate) { return candidate.account_id == accountId; });
      if (found == pool.accounts.end() || !found->native_owner || found->native_owner->codex_home != home || document.auth.account_id != accountId) {
        throw CodexError("Native Codex account or credential generation is mismatched; direct refresh was refused.", "codex_auth_owner_unavailable", 503);
      }
      const CodexAuthState bound = *found;
      rt::Promise<std::string> generating(nativeCodexCredentialGeneration(bound));
      const std::string generation = co_await generating.wait();
      if (bound.native_owner->generation_hash != generation) {
        throw CodexError("Native Codex ownership no longer matches its durable credentials.", "codex_auth_owner_unavailable", 503);
      }
      verifyExternalNativeIdentity(bound, document);
      co_return bound;
    }

    // `{ home: string; document: NativeDocument; current: CodexAuthAccountEntry }`
    struct NativeAccountContext {
      std::string home;
      NativeDocument document;
      auth::CodexAuthAccountEntry current;
    };

    rt::Task<std::optional<NativeAccountContext>> loadNativeOwnedAccount(CodexAuthState input) {
      const std::vector<std::string> homes = nativeCodexConsumerHomes();
      const std::optional<std::string> home =
        input.native_owner ? std::optional<std::string>(input.native_owner->codex_home) : (homes.empty() ? std::nullopt : std::optional<std::string>(homes[0]));
      if (!rt::truthy(home) && !input.native_owner) co_return std::nullopt;
      const std::optional<std::string> ownerHome = input.native_owner ? std::optional<std::string>(input.native_owner->codex_home) : std::nullopt;
      if (!rt::truthy(home) || std::find(homes.begin(), homes.end(), *home) == homes.end() || (rt::truthy(ownerHome) && *ownerHome != *home)) {
        throw CodexError("Native Codex credential home is unavailable or changed.", "codex_auth_owner_unavailable", 503);
      }
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      std::optional<rt::KvEntryMaybe<json::value>> entry;
      if (kv) {
        auto reading = readPoolEntry(kv);
        entry = co_await std::move(reading);
      }
      const std::optional<CodexAuthPoolState> pool = entry ? auth::parseCodexAuthPool(entry->value) : std::nullopt;
      std::optional<CodexAuthState> stored;
      if (pool) {
        const std::optional<std::size_t> index = accountIndex(*pool, input.account_id);
        if (index) stored = pool->accounts[*index];
      }
      NativeDocument document;
      rt::Reason failure;
      try {
        rt::Promise<NativeDocument> reading(readNativeDocument(*home));
        document = co_await reading.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        if (!input.native_owner && !(stored && stored->native_owner)) co_return std::nullopt;
        throw CodexError("Native Codex credential file is unavailable; direct refresh was refused.", "codex_auth_owner_unavailable", 503, failure);
      }
      if (!input.native_owner && !(stored && stored->native_owner) && document.auth.account_id != input.account_id) co_return std::nullopt;
      if (!kv || !pool || !entry || !stored) {
        throw CodexError("Native Codex ownership requires the current durable account.", "codex_auth_owner_unavailable", 503);
      }
      auth::CodexAuthPoolEntry unbound;
      unbound.kv = kv;
      unbound.entry = entry;
      unbound.pool = *pool;
      rt::Promise<auth::CodexAuthPoolEntry> binding(bindNativeCodexAuthPool(std::move(unbound)));
      const auth::CodexAuthPoolEntry bound = co_await binding.wait();
      rt::Promise<CodexAuthState> verifying(verifyNativeAccountBinding(bound.pool, input.account_id, *home, document));
      const CodexAuthState authState = co_await verifying.wait();
      NativeAccountContext context;
      context.home = *home;
      context.document = document;
      static_cast<auth::CodexAuthPoolEntry&>(context.current) = bound;
      context.current.auth = authState;
      co_return context;
    }
  }

  /** Isolated fixtures never touch the owner's credential home or daemon. */
  void setNativeCodexAuthHooksForTest(std::optional<NativeAuthHooks> value) { hooks = std::move(value); }

  /** The recovery contract permits the primary and the one existing named Ubiquity home. */
  std::vector<std::string> nativeCodexConsumerHomes() {
    const std::optional<std::string> primary = nativeHome();
    if (!rt::truthy(primary)) return {};
    try {
      const GetEnv getEnv = environmentReader();
      const std::optional<std::string> home = getEnv("HOME");
      if (!rt::truthy(home)) return {*primary};
      const std::string ubiquity = *home + "/.codex-ubiquity";
      if (ubiquity == *primary) return {*primary};
      return {*primary, ubiquity};
    } catch (...) {
      return {*primary};
    }
  }

  rt::Task<std::string> nativeCodexCredentialGeneration(CodexAuthState auth) {
    const json::array triple{json::value(auth.account_id), json::value(auth.access_token), json::value(auth.refresh_token)};
    rt::Promise<std::string> hashing(utils::sha256Hex(rt::jsonStringify(json::value(triple))));
    const std::string generation = co_await hashing.wait();
    co_return generation;
  }

  /** Equality is the only bootstrap proof; a file never overlays uploaded siblings. */
  rt::Task<auth::CodexAuthPoolEntry> bindNativeCodexAuthPool(auth::CodexAuthPoolEntry input) {
    if (upstream_auth::followerMode()) co_return input;
    const std::optional<std::string> home = nativeHome();
    if (!rt::truthy(home) || !input.kv || !input.entry) co_return input;
    NativeDocument document;
    bool readDocument = false;
    try {
      rt::Promise<NativeDocument> reading(readNativeDocument(*home));
      document = co_await reading.wait();
      readDocument = true;
    } catch (...) {
    }
    if (!readDocument) co_return input;
    auth::CodexAuthPoolEntry current = input;
    for (int attempt = 0; attempt < 3; attempt += 1) {
      const std::optional<std::size_t> index = accountIndex(current.pool, document.auth.account_id);
      if (!index) co_return current;
      const CodexAuthState authState = current.pool.accounts[*index];
      if (authState.native_owner || !sameCredentials(authState, document.auth)) co_return current;
      rt::Promise<std::string> generating(nativeCodexCredentialGeneration(authState));
      const std::string generationHash = co_await generating.wait();
      CodexAuthState::NativeOwner owner;
      owner.codex_home = *home;
      owner.generation_hash = generationHash;
      if (rt::truthy(document.refreshedAt)) owner.native_refreshed_at = document.refreshedAt;
      CodexAuthState bound = authState;
      bound.native_owner = owner;
      CodexAuthPoolState pool = current.pool;
      pool.accounts[*index] = bound;
      const json::value poolValue = json::value_from(pool);
      const rt::KvValue row(poolValue);
      std::unique_ptr<rt::AtomicOperation> operation = input.kv->atomic();
      operation->check(*current.entry).set(auth::CODEX_AUTH_POOL_KV_KEY, row);
      auto committing = operation->commit();
      const rt::KvCommitResult committed = co_await std::move(committing);
      if (committed.ok) {
        auth::CodexAuthPoolEntry result = current;
        result.pool = pool;
        result.entry = rt::KvEntryMaybe<json::value>{auth::CODEX_AUTH_POOL_KV_KEY, poolValue, committed.versionstamp};
        co_return result;
      }
      auto rereading = readPoolEntry(input.kv);
      const rt::KvEntryMaybe<json::value> entry = co_await std::move(rereading);
      const std::optional<CodexAuthPoolState> latest = auth::parseCodexAuthPool(entry.value);
      if (!latest) throw CodexError("Native Codex ownership could not bind the current pool.", "codex_auth_owner_unavailable", 503);
      current.kv = input.kv;
      current.entry = entry;
      current.pool = *latest;
    }
    throw CodexError("Native Codex ownership changed concurrently; no refresh was attempted.", "codex_auth_refresh_failed", 503);
  }

  /** The CLI proves the current fixed-home file and passive authorization before this plan. */
  rt::Task<CodexAuthPoolState> planExternalNativeCodexAdoption(CodexAuthPoolState pool, json::value raw, std::string home, double nowMs, bool previousRejected) {
    const std::optional<external_auth::ExternalNativeCodexDocument> external = external_auth::parseExternalNativeCodexDocument(raw);
    const std::optional<std::size_t> index = external ? accountIndex(pool, external->auth.account_id) : std::nullopt;
    const std::vector<std::string> homes = nativeCodexConsumerHomes();
    if (!external || !index || std::find(homes.begin(), homes.end(), home) == homes.end()) {
      throw CodexError("External native Codex requires the current fixed-home account.", "codex_auth_owner_unavailable", 503);
    }
    const CodexAuthState previous = pool.accounts[*index];
    const std::optional<CodexAuthState::NativeOwner> owner = previous.native_owner;
    if (owner) {
      bool destinationChanged = owner->codex_home != home;
      if (!destinationChanged) {
        rt::Promise<std::string> generating(nativeCodexCredentialGeneration(previous));
        const std::string generation = co_await generating.wait();
        destinationChanged = owner->generation_hash != generation;
      }
      if (destinationChanged) throw CodexError("External native Codex destination ownership changed.", "codex_auth_owner_conflict", 409);
    }
    NativeDocument document;
    document.auth = CodexAuthState{.access_token = external->auth.access_token, .refresh_token = external->auth.refresh_token, .account_id = external->auth.account_id,
                                   .updated_at_ms = nowMs};
    document.email = std::nullopt;
    document.refreshedAt = external->refreshedAt;
    document.externalUserId = external->userId;
    verifyExternalNativeIdentity(previous, document);
    if (sameCredentials(previous, document.auth) && isExternalOwner(owner)) {
      if (auth::getJwtExpMs(previous.access_token).value_or(0) <= nowMs + 120'000) {
        throw CodexError("External native access is expired or awaiting renewal.", "codex_auth_owner_unavailable", 503);
      }
      co_return pool;
    }
    if (!isExternalOwner(owner) && !previousRejected && auth::getJwtExpMs(previous.access_token).value_or(rt::Number::POSITIVE_INFINITY) > nowMs) {
      throw CodexError("A valid managed Codex account cannot be replaced by an external consumer.", "codex_auth_owner_conflict", 409);
    }
    verifyNativeProgress(previous, document);
    std::vector<CodexAuthState> accounts = pool.accounts;
    rt::Promise<CodexAuthState::NativeOwner> owning(nativeOwnerForDocument(document, home));
    const CodexAuthState::NativeOwner adoptedOwner = co_await owning.wait();
    CodexAuthState adopted = document.auth;
    adopted.native_owner = adoptedOwner;
    accounts[*index] = adopted;
    CodexAuthPoolState adoptedPool{.accounts = std::move(accounts), .updated_at_ms = nowMs};
    co_return adoptedPool;
  }

  /** Returns null only for an unrelated, unbound account. Never falls open for an owner. */
  rt::Task<std::optional<CodexAuthState>> refreshNativeCodexAuthIfOwned(CodexAuthState input, bool rejectStaleReplacement) {
    auth::requireCodexAuthRefreshAllowed();
    rt::Promise<std::optional<NativeAccountContext>> loading(loadNativeOwnedAccount(input));
    const std::optional<NativeAccountContext> context = co_await loading.wait();
    if (!context) co_return std::nullopt;
    const std::string home = context->home;
    const auth::CodexAuthAccountEntry current = context->current;
    const CodexAuthState authState = current.auth;
    NativeDocument document = context->document;
    if (!sameCredentials(input, authState) && !sameCredentials(input, document.auth)) {
      bool adoptStored = false;
      if (!rejectStaleReplacement && input.native_owner && input.native_owner->codex_home == home) {
        rt::Promise<std::string> generating(nativeCodexCredentialGeneration(input));
        const std::string generation = co_await generating.wait();
        adoptStored = input.native_owner->generation_hash == generation && sameCredentials(authState, document.auth);
      }
      if (adoptStored) {
        auth::cacheCodexAuthPool(current.pool);
        co_return authState;
      }
      throw CodexError("A stale native Codex credential replacement was refused.", "codex_auth_owner_conflict", 409);
    }
    if (!sameCredentials(authState, document.auth)) {
      if (auth::getJwtExpMs(document.auth.access_token).value_or(0) > rt::dateNow() + 120'000) {
        rt::Promise<CodexAuthState> persisting(persistNativeGeneration(current, document, home));
        const CodexAuthState persisted = co_await persisting.wait();
        co_return persisted;
      }
      // Prove generation order before asking its owner to repair an expired token.
      // The expired generation is never adopted; persistence still requires usability.
      verifyNativeProgress(authState, document, false);
    }
    if (rt::truthy(document.externalUserId) || isExternalOwner(authState.native_owner)) {
      throw CodexError("External Codex access must be renewed by its credential owner; local refresh was refused.", "codex_auth_owner_unavailable", 503);
    }
    if (!rt::truthy(document.email)) throw CodexError("Native Codex account identity could not be verified.", "codex_auth_owner_unavailable", 503);
    rt::Reason failure;
    try {
      if (hooks && hooks->refresh) {
        const std::function<rt::Task<void>(std::string, std::string, std::optional<std::string>)> refresh = hooks->refresh;
        auto refreshing = refresh(home, *document.email, authState.account_id);
        co_await std::move(refreshing);
      } else {
        auto refreshing = native_auth_transport::requestNativeCodexRefresh(home, *document.email, authState.account_id);
        co_await std::move(refreshing);
      }
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) throw CodexError("Native Codex refresh request failed; direct refresh was refused.", "codex_auth_owner_unavailable", 503, failure);
    rt::Promise<NativeDocument> rereading(readNativeDocument(home));
    document = co_await rereading.wait();
    if (sameCredentials(authState, document.auth)) {
      throw CodexError("Native Codex did not persist a new credential generation.", "codex_auth_refresh_failed", 503);
    }
    rt::Promise<CodexAuthState> persisting(persistNativeGeneration(current, document, home));
    const CodexAuthState persisted = co_await persisting.wait();
    co_return persisted;
  }
}
