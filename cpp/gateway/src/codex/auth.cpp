// Codex auth pool parsing, caching and loading, split out of src/codex.ts.
#include "uos/codex/auth.hpp"

#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/external_auth.hpp"
#include "uos/codex/native_auth.hpp"
#include "uos/codex/routing_mutations.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/upstream_auth.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <utility>

namespace uos::codex::auth {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using ::uos::types::CodexAuthPoolState;
    using ::uos::types::CodexAuthState;
    using routing_state::RoutingAccount;
  }

  // Routing errors need to remain distinguishable to the gateway's fallback
  // adapter without adding gateway-only headers to OpenAI-compatible responses.
  rt::WeakMap<rt::Response, std::string> codexRoutingErrors;
  std::optional<CodexAuthPoolState> cachedAuthPool;
  double authCacheGeneration = 0;
  rt::OrderedMap<std::string, rt::Promise<CodexAuthState>> refreshesInFlight;
  rt::WeakMap<rt::Response, RoutingAccount> codexProbeByResponse;
  rt::OrderedMap<const void*, rt::Promise<void>> codexProbeTransitionsInFlight;

  namespace {
    rt::WeakMap<rt::Response, std::string> codexAuthWarnings;
    double cachedAuthPoolExpiresAtMs = 0;
    std::optional<rt::Promise<CodexAuthPoolEntry>> authPoolEntryInFlight;
    rt::WeakMap<rt::Response, double> codexSlotByResponse;
    rt::WeakMap<rt::Response, std::string> codexAccountIdByResponse;
    rt::WeakMap<rt::Response, CodexResponseActiveTelemetry> codexActiveTelemetryByResponse;
    rt::WeakSet<rt::Response> codexTerminalOutcomeByResponse;

    struct SemverTriplet {
      double major;
      double minor;
      double patch;
    };

    std::optional<SemverTriplet> parseSemverTriplet(std::string_view value) {
      const std::vector<std::string> parts = rt::split(rt::trim(value), ".");
      if (parts.size() < 2 || parts.size() > 3) return std::nullopt;
      std::vector<double> nums;
      for (const std::string& part : parts) nums.push_back(rt::stringToNumber(part));
      for (const double num : nums)
        if (!rt::Number::isFinite(num) || num < 0) return std::nullopt;
      const double patch = nums.size() > 2 ? nums[2] : 0;
      return SemverTriplet{rt::Math::trunc(nums[0]), rt::Math::trunc(nums[1]), rt::Math::trunc(patch)};
    }

    // `a, b: string | null | undefined`; std::nullopt is null and undefined.
    std::optional<std::string> pickHigherSemver(const std::optional<std::string>& a, const std::optional<std::string>& b) {
      const std::string aNorm = a ? rt::trim(*a) : std::string();
      const std::string bNorm = b ? rt::trim(*b) : std::string();
      if (aNorm.empty() && bNorm.empty()) return std::nullopt;
      if (aNorm.empty()) return bNorm.empty() ? std::nullopt : std::optional<std::string>(bNorm);
      if (bNorm.empty()) return aNorm.empty() ? std::nullopt : std::optional<std::string>(aNorm);

      const std::optional<SemverTriplet> aParsed = parseSemverTriplet(aNorm);
      const std::optional<SemverTriplet> bParsed = parseSemverTriplet(bNorm);
      if (!aParsed || !bParsed) return aNorm;
      const std::array<double, 3> aParts{aParsed->major, aParsed->minor, aParsed->patch};
      const std::array<double, 3> bParts{bParsed->major, bParsed->minor, bParsed->patch};
      for (std::size_t i = 0; i < 3; i++) {
        if (aParts[i] > bParts[i]) return aNorm;
        if (aParts[i] < bParts[i]) return bNorm;
      }
      return aNorm;
    }

    // `/^[a-f0-9]{64}$/.test(value)`
    bool isGenerationHash(std::string_view value) {
      if (value.size() != 64) return false;
      return std::all_of(value.begin(), value.end(), [](char character) { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); });
    }

    // `value: unknown`: nullptr is undefined.
    std::optional<CodexAuthState::NativeOwner> parseNativeAuthOwner(const json::value* value) {
      if (value == nullptr) return std::nullopt;
      const std::optional<std::string> codexHome = utils::getString(rt::get(value, "codex_home"));
      if (!utils::isRecord(value) || !codexHome || !rt::startsWith(*codexHome, "/") ||
          !isGenerationHash(utils::getString(rt::get(value, "generation_hash")).value_or(""))) {
        throw CodexError("Codex native credential ownership is malformed; nothing was replaced.", "codex_auth_owner_unavailable", 503);
      }
      const std::optional<std::string> refreshedAt = utils::getString(rt::get(value, "native_refreshed_at"));
      const std::optional<std::string> userId = utils::getString(rt::get(value, "chatgpt_user_id"));
      const json::value* authMode = rt::get(value, "auth_mode");
      const bool externalMode = authMode != nullptr && authMode->is_string() && authMode->get_string() == "chatgptAuthTokens";
      if (authMode != nullptr && (!externalMode || !rt::truthy(userId))) {
        throw CodexError("Codex external native ownership is malformed; nothing was replaced.", "codex_auth_owner_unavailable", 503);
      }
      CodexAuthState::NativeOwner owner;
      owner.codex_home = rt::toString(rt::get(value, "codex_home"));
      owner.generation_hash = rt::toString(rt::get(value, "generation_hash"));
      if (rt::truthy(refreshedAt)) owner.native_refreshed_at = refreshedAt;
      if (externalMode) {
        owner.auth_mode = std::string("chatgptAuthTokens");
        owner.chatgpt_user_id = userId;
      }
      return owner;
    }

    // `string | null`
    std::optional<std::string> parsePoolRefreshToken(const json::value& candidate, const std::optional<CodexAuthState::NativeOwner>& owner) {
      const std::optional<std::string> refreshToken = utils::getString(rt::get(candidate, "refresh_token"));
      if (!owner || owner->auth_mode != std::optional<std::string>("chatgptAuthTokens")) {
        if (refreshToken == std::optional<std::string>("")) return std::nullopt;
        return refreshToken;
      }
      const std::optional<std::string> accessToken = utils::getString(rt::get(candidate, "access_token"));
      const std::optional<external_auth::ExternalCodexIdentity> identity =
        rt::truthy(accessToken) ? external_auth::externalCodexIdentity(*accessToken) : std::nullopt;
      const json::value* candidateRefresh = rt::get(candidate, "refresh_token");
      const bool emptyRefresh = candidateRefresh != nullptr && candidateRefresh->is_string() && candidateRefresh->get_string().empty();
      const bool userMatches = identity && owner->chatgpt_user_id && identity->userId == *owner->chatgpt_user_id;
      const std::optional<std::string> candidateAccountId = utils::getString(rt::get(candidate, "account_id"));
      const bool accountMatches = identity && candidateAccountId && identity->accountId == *candidateAccountId;
      if (!emptyRefresh || !userMatches || !accountMatches) return std::nullopt;
      return std::string();
    }

    std::optional<CodexAuthState> parsePoolAccount(const json::value& candidate) {
      if (!utils::isRecord(candidate)) return std::nullopt;
      const std::optional<std::string> accessToken = utils::getString(rt::get(candidate, "access_token"));
      const std::optional<CodexAuthState::NativeOwner> owner = parseNativeAuthOwner(rt::get(candidate, "native_owner"));
      const std::optional<std::string> refreshToken = parsePoolRefreshToken(candidate, owner);
      const std::optional<std::string> accountId = utils::getString(rt::get(candidate, "account_id"));
      const std::optional<double> updatedAtMs = rt::getNumber(rt::get(candidate, "updated_at_ms"));
      if (!updatedAtMs || !rt::Number::isFinite(*updatedAtMs)) return std::nullopt;
      if (!rt::truthy(accessToken) || !refreshToken || !rt::truthy(accountId)) return std::nullopt;
      CodexAuthState account;
      account.access_token = *accessToken;
      account.refresh_token = *refreshToken;
      account.account_id = *accountId;
      account.updated_at_ms = *updatedAtMs;
      if (owner) account.native_owner = owner;
      return account;
    }

    /** True when the value contains a C0 control character or DEL, which never appear in a real address. */
    bool hasControlCharacter(std::string_view value) {
      // Control characters are single UTF-8 bytes; no byte of a multi-byte sequence is below 0x80.
      return std::any_of(value.begin(), value.end(), [](char character) {
        const auto code = static_cast<unsigned char>(character);
        return code < 32 || code == 127;
      });
    }

    // `value: unknown`: nullptr is undefined.
    std::optional<std::string> normalizeCodexAccountEmail(const json::value* value) {
      if (value == nullptr || !value->is_string()) return std::nullopt;
      const std::string email = rt::trim(std::string_view(value->get_string()));
      if (rt::length(email) == 0 || rt::length(email) > 320 || hasControlCharacter(email)) return std::nullopt;
      return rt::includes(email, "@") ? std::optional<std::string>(email) : std::nullopt;
    }

    // `Record<string, unknown> | null`: an object or array (utils.isRecord), or std::nullopt.
    std::optional<json::value> jwtPayload(std::string_view token) {
      const std::vector<std::string> parts = rt::split(token, ".");
      if (parts.size() < 2 || parts[1].empty()) return std::nullopt;
      try {
        json::value parsed = rt::jsonParse(rt::decodeUtf8(utils::base64UrlDecode(parts[1])));
        return utils::isRecord(parsed) ? std::optional<json::value>(std::move(parsed)) : std::nullopt;
      } catch (...) {
        return std::nullopt;
      }
    }

    void setCodexResponseActiveTelemetryRecord(const rt::Response& response, const CodexResponseActiveTelemetry& telemetry) {
      codexActiveTelemetryByResponse.set(response, telemetry);
    }

    std::optional<RoutingAccount> takeCodexResponseProbe(const rt::Response& response) {
      std::optional<RoutingAccount> probe = codexProbeByResponse.get(response);
      if (probe) codexProbeByResponse.delete_(response);
      return probe;
    }

    // `Readonly<{ accountId: string | null; probe: RoutingAccount | null }> | null`
    struct CodexResponseTerminalOutcome {
      std::optional<std::string> accountId;
      std::optional<RoutingAccount> probe;
    };

    std::optional<CodexResponseTerminalOutcome> beginCodexResponseTerminalOutcome(const rt::Response& response) {
      if (codexTerminalOutcomeByResponse.has(response)) return std::nullopt;
      codexTerminalOutcomeByResponse.add(response);
      CodexResponseTerminalOutcome terminal;
      terminal.accountId = codexAccountIdByResponse.get(response);
      terminal.probe = takeCodexResponseProbe(response);
      return terminal;
    }

    rt::Task<void> completeCodexProbeTransition(rt::Promise<void> transition) {
      codexProbeTransitionsInFlight.set(transition.identity(), transition);
      rt::Reason failure;
      try {
        co_await transition.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      codexProbeTransitionsInFlight.delete_(transition.identity());
      if (failure) std::rethrow_exception(failure);
    }

    // `string | null`
    std::optional<std::string> codexProviderRequestId(const rt::Response& response) {
      if (std::optional<std::string> id = response.headers().get("X-Request-Id")) return id;
      if (std::optional<std::string> id = response.headers().get("X-Api-Request-Id")) return id;
      return response.headers().get("X-Oneapi-Request-Id");
    }

    CodexAuthState loadAuthSeedFromDisk();

    CodexAuthState loadAuthSeedFromEnv() {
      if (config::config.isDeploy) {
        if (config::config.codexAuthJsonB64.empty()) {
          throw CodexError("Codex auth missing: CODEX_AUTH_JSON_B64 unset and no KV entry.", "codex_auth_missing", 503);
        }
      }

      if (!config::config.codexAuthJsonB64.empty()) {
        std::string decoded;
        try {
          decoded = utils::decodeBase64ToString(config::config.codexAuthJsonB64);
        } catch (...) {
          throw CodexError("Codex auth invalid: CODEX_AUTH_JSON_B64 is not valid base64.", "codex_auth_invalid", 503, std::current_exception());
        }
        json::value parsed;
        try {
          parsed = rt::jsonParse(decoded);
        } catch (...) {
          throw CodexError("Codex auth invalid: CODEX_AUTH_JSON_B64 is not valid JSON.", "codex_auth_invalid", 503, std::current_exception());
        }
        const std::optional<ParseCodexAuthFromAuthJsonResult> tokenData = parseCodexAuthFromAuthJson(parsed);
        if (!tokenData) {
          throw CodexError("Codex auth invalid: CODEX_AUTH_JSON_B64 does not look like a Codex auth.json.", "codex_auth_invalid", 503);
        }
        return CodexAuthState{.access_token = tokenData->access_token, .refresh_token = tokenData->refresh_token, .account_id = tokenData->account_id,
                              .updated_at_ms = rt::dateNow(), .native_owner = tokenData->native_owner};
      }

      if (config::config.isDeploy) {
        throw CodexError("Codex auth missing: CODEX_AUTH_JSON_B64 unset and no KV entry.", "codex_auth_missing", 503);
      }

      return loadAuthSeedFromDisk();
    }

    CodexAuthState loadAuthSeedFromDisk() {
      if (!config::config.isDeploy) {
        const std::optional<std::string> home = rt::env::get("HOME");
        if (!rt::truthy(home)) {
          throw CodexError("Could not resolve home directory for ~/.codex/auth.json.", "codex_auth_invalid", 503);
        }
        rt::Reason failure;
        try {
          const std::string raw = rt::fs::readFileSync(*home + "/.codex/auth.json", "utf8");
          const json::value parsed = rt::jsonParse(raw);
          const std::optional<ParseCodexAuthFromAuthJsonResult> tokenData = parseCodexAuthFromAuthJson(parsed);
          if (!tokenData) {
            throw CodexError("Codex auth invalid: ~/.codex/auth.json does not look like a Codex auth.json.", "codex_auth_invalid", 503);
          }
          return CodexAuthState{.access_token = tokenData->access_token, .refresh_token = tokenData->refresh_token, .account_id = tokenData->account_id,
                                .updated_at_ms = rt::dateNow(), .native_owner = tokenData->native_owner};
        } catch (const CodexError&) {
          throw;
        } catch (...) {
          failure = std::current_exception();
        }
        throw CodexError("Codex auth invalid: ~/.codex/auth.json is missing or unreadable.", "codex_auth_invalid", 503, failure);
      }
      throw CodexError("Codex auth missing: CODEX_AUTH_JSON_B64 unset and no KV entry.", "codex_auth_missing", 503);
    }

    // `CodexAuthState | null`
    std::optional<CodexAuthState> getConfiguredCodexAuthSeed() {
      if (config::config.codexAuthJsonB64.empty()) {
        return loadAuthSeedFromEnv();  // throws when unavailable in deploy or returns fallback in local
      }
      try {
        return loadAuthSeedFromEnv();
      } catch (...) {
        if (config::config.isDeploy) return std::nullopt;
        try {
          return loadAuthSeedFromDisk();
        } catch (...) {
          return std::nullopt;
        }
      }
    }

    CodexAuthPoolState poolFromSeed(const CodexAuthState& auth) { return CodexAuthPoolState{.accounts = {auth}, .updated_at_ms = auth.updated_at_ms}; }

    CodexAuthPoolEntry poolOnlyEntry(const CodexAuthPoolState& pool) {
      CodexAuthPoolEntry entry;
      entry.kv = nullptr;
      entry.entry = std::nullopt;
      entry.pool = pool;
      return entry;
    }

    CodexAuthPoolEntry loadedAuthPoolEntry(const CodexAuthPoolState& pool, double generationAtStart, std::shared_ptr<rt::Kv> kv,
                                           std::optional<rt::KvEntryMaybe<json::value>> entry) {
      if (authCacheGeneration != generationAtStart && cachedAuthPool) return poolOnlyEntry(*cachedAuthPool);
      cacheCodexAuthPool(pool);
      CodexAuthPoolEntry loaded;
      loaded.kv = std::move(kv);
      loaded.entry = std::move(entry);
      loaded.pool = pool;
      return loaded;
    }

    rt::Task<CodexAuthPoolEntry> loadAuthPoolEntry(double generationAtStart) {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) {
        if (upstream_auth::followerMode()) throw CodexError("Codex follower requires a stored auth pool.", "codex_auth_missing", 503);
        std::optional<CodexAuthPoolState> pool = cachedAuthPool;
        if (!pool) {
          const std::optional<CodexAuthState> seed = getConfiguredCodexAuthSeed();
          if (seed) pool = poolFromSeed(*seed);
        }
        if (!pool) {
          throw CodexError("Codex auth missing: CODEX_AUTH_JSON_B64 unset and no KV entry.", "codex_auth_missing", 503);
        }
        co_return loadedAuthPoolEntry(*pool, generationAtStart, nullptr, std::nullopt);
      }

      const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
      auto reading = kv->get(CODEX_AUTH_POOL_KV_KEY, strong);
      const rt::KvEntryMaybe<> stored = co_await std::move(reading);
      const rt::KvEntryMaybe<json::value> entry = stored.as<json::value>();
      const std::optional<CodexAuthPoolState> storedPool = parseCodexAuthPool(entry.value);
      if (storedPool) {
        // A valid persisted pool is the authority. Local/disk seeds may bootstrap
        // an absent row or run without KV, but must never overwrite or append to
        // credentials an admin has already uploaded.
        CodexAuthPoolEntry input;
        input.kv = kv;
        input.entry = entry;
        input.pool = *storedPool;
        rt::Promise<CodexAuthPoolEntry> binding(native_auth::bindNativeCodexAuthPool(std::move(input)));
        const CodexAuthPoolEntry nativeEntry = co_await binding.wait();
        co_return loadedAuthPoolEntry(nativeEntry.pool, generationAtStart, kv, nativeEntry.entry);
      }

      if (upstream_auth::followerMode()) throw CodexError("Codex follower requires a stored auth pool.", "codex_auth_missing", 503);
      const std::optional<CodexAuthState> seed = getConfiguredCodexAuthSeed();
      if (!seed) {
        throw CodexError("Codex auth missing: CODEX_AUTH_JSON_B64 unset and no KV entry.", "codex_auth_missing", 503);
      }
      if (authCacheGeneration != generationAtStart && cachedAuthPool) co_return poolOnlyEntry(*cachedAuthPool);
      const CodexAuthPoolState pool = poolFromSeed(*seed);
      const json::value poolValue = json::value_from(pool);
      const rt::KvValue row(poolValue);
      auto seeding = kv->set(CODEX_AUTH_POOL_KV_KEY, row);
      const rt::KvCommitResult seeded = co_await std::move(seeding);
      CodexAuthPoolEntry input;
      input.kv = kv;
      input.entry = rt::KvEntryMaybe<json::value>{CODEX_AUTH_POOL_KV_KEY, poolValue, seeded.versionstamp};
      input.pool = pool;
      rt::Promise<CodexAuthPoolEntry> binding(native_auth::bindNativeCodexAuthPool(std::move(input)));
      const CodexAuthPoolEntry nativeEntry = co_await binding.wait();
      co_return loadedAuthPoolEntry(nativeEntry.pool, generationAtStart, kv, nativeEntry.entry);
    }
  }

  void requireCodexAuthRefreshAllowed() {
    if (upstream_auth::followerMode()) throw CodexAuthFollowerStaleError();
  }

  std::optional<std::string> getCodexRoutingError(const rt::Response& response) { return codexRoutingErrors.get(response); }

  std::string codexUserAgent(const std::optional<std::string>& clientVersion) {
    const std::string version = pickHigherSemver(clientVersion, std::string(CODEX_CLIENT_VERSION)).value_or(std::string(CODEX_CLIENT_VERSION));
    return "codex_cli_rs/" + version + " (ai.ubq.fi)";
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ParseCodexAuthFromAuthJsonResult& value) {
    out = Writer()
            .required("access_token", value.access_token)
            .required("refresh_token", value.refresh_token)
            .required("account_id", value.account_id)
            .orUndefined("native_owner", value.native_owner)
            .done();
  }

  ParseCodexAuthFromAuthJsonResult tag_invoke(const json::value_to_tag<ParseCodexAuthFromAuthJsonResult>&, const json::value& value) {
    const Reader in(value, "ParseCodexAuthFromAuthJsonResult");
    return ParseCodexAuthFromAuthJsonResult{
      .access_token = in.required<std::string>("access_token"),
      .refresh_token = in.required<std::string>("refresh_token"),
      .account_id = in.required<std::string>("account_id"),
      .native_owner = in.orUndefined<CodexAuthState::NativeOwner>("native_owner"),
    };
  }

  std::optional<ParseCodexAuthFromAuthJsonResult> parseCodexAuthFromAuthJson(const json::value& value) { return parseCodexAuthFromAuthJson(&value); }

  std::optional<ParseCodexAuthFromAuthJsonResult> parseCodexAuthFromAuthJson(const json::value* value) {
    const json::value* authMode = rt::get(value, "auth_mode");
    if (!utils::isRecord(value) || (authMode != nullptr && authMode->is_string() && authMode->get_string() == "chatgptAuthTokens")) return std::nullopt;
    const json::value* tokens = utils::isRecord(rt::get(value, "tokens")) ? rt::get(value, "tokens") : nullptr;
    if (tokens == nullptr) return std::nullopt;
    const std::optional<std::string> accessToken = utils::getString(rt::get(tokens, "access_token"));
    const std::optional<std::string> refreshToken = utils::getString(rt::get(tokens, "refresh_token"));
    const std::optional<std::string> accountId = utils::getString(rt::get(tokens, "account_id"));
    if (!rt::truthy(accessToken) || !rt::truthy(refreshToken) || !rt::truthy(accountId)) return std::nullopt;
    return ParseCodexAuthFromAuthJsonResult{.access_token = *accessToken, .refresh_token = *refreshToken, .account_id = *accountId};
  }

  std::optional<CodexAuthPoolState> parseCodexAuthPool(const json::value& value) { return parseCodexAuthPool(&value); }

  std::optional<CodexAuthPoolState> parseCodexAuthPool(const json::value* value) {
    const json::value* accountsValue = rt::get(value, "accounts");
    if (!utils::isRecord(value) || accountsValue == nullptr || !accountsValue->is_array()) return std::nullopt;
    const json::array& candidates = accountsValue->get_array();
    if (candidates.size() < 1 || static_cast<double>(candidates.size()) > CODEX_AUTH_POOL_MAX_ACCOUNTS) return std::nullopt;
    const std::optional<double> number = rt::getNumber(rt::get(value, "updated_at_ms"));
    const std::optional<double> updatedAtMs = number && rt::Number::isFinite(*number) ? number : std::nullopt;
    if (!updatedAtMs) return std::nullopt;

    std::unordered_set<std::string> accountIds;
    std::vector<CodexAuthState> accounts;
    for (const json::value& candidate : candidates) {
      std::optional<CodexAuthState> account = parsePoolAccount(candidate);
      if (!account || accountIds.contains(account->account_id)) return std::nullopt;
      accountIds.insert(account->account_id);
      accounts.push_back(std::move(*account));
    }

    return CodexAuthPoolState{.accounts = std::move(accounts), .updated_at_ms = *updatedAtMs};
  }

  std::optional<CodexAuthPoolState> upsertCodexAuthAccount(const std::optional<CodexAuthPoolState>& pool, const CodexAuthState& auth) {
    if (auth.refresh_token.empty() && !(auth.native_owner && auth.native_owner->auth_mode == std::optional<std::string>("chatgptAuthTokens"))) {
      throw CodexError("Access-only Codex auth requires proven external native ownership.", "codex_auth_owner_unavailable", 503);
    }
    std::vector<CodexAuthState> accounts = pool ? pool->accounts : std::vector<CodexAuthState>{};
    const auto matching = std::find_if(accounts.begin(), accounts.end(), [&auth](const CodexAuthState& candidate) { return candidate.account_id == auth.account_id; });
    if (matching != accounts.end()) {
      const CodexAuthState previous = *matching;
      const bool sameTokens = previous.access_token == auth.access_token && previous.refresh_token == auth.refresh_token;
      if (previous.native_owner && !sameTokens &&
          (!auth.native_owner || auth.native_owner->codex_home != previous.native_owner->codex_home ||
           getJwtExpMs(auth.access_token).value_or(0) < getJwtExpMs(previous.access_token).value_or(0))) {
        throw CodexError("The native Codex owner must advance this account's credentials before replacement.", "codex_auth_owner_conflict", 409);
      }
      if (previous.native_owner && sameTokens) {
        CodexAuthState kept = auth;
        kept.native_owner = previous.native_owner;
        *matching = kept;
      } else {
        *matching = auth;
      }
    } else if (static_cast<double>(accounts.size()) < CODEX_AUTH_POOL_MAX_ACCOUNTS) {
      accounts.push_back(auth);
    } else {
      return std::nullopt;
    }
    return CodexAuthPoolState{.accounts = std::move(accounts), .updated_at_ms = rt::dateNow()};
  }

  std::optional<double> getJwtExpMs(const json::value& token) { return getJwtExpMs(&token); }

  std::optional<double> getJwtExpMs(const json::value* token) {
    if (token == nullptr || !token->is_string()) return std::nullopt;
    return getJwtExpMs(std::string_view(token->get_string()));
  }

  std::optional<double> getJwtExpMs(std::string_view token) {
    const std::vector<std::string> parts = rt::split(rt::trim(token), ".");
    if (parts.size() != 3 || parts[1].empty()) return std::nullopt;
    const std::string payload = rt::replaceAll(rt::replaceAll(parts[1], "-", "+"), "_", "/");
    const std::string padded = payload + rt::repeat("=", (4 - (rt::length(payload) % 4)) % 4);
    try {
      const std::string decoded = utils::decodeBase64ToString(padded);
      const json::value parsed = rt::jsonParse(decoded);
      const std::optional<double> number = utils::isRecord(parsed) ? rt::getNumber(rt::get(parsed, "exp")) : std::nullopt;
      const std::optional<double> exp = number && rt::Number::isFinite(*number) ? number : std::nullopt;
      return rt::truthy(exp) ? std::optional<double>(*exp * 1000) : std::nullopt;
    } catch (...) {
      return std::nullopt;
    }
  }

  /** Returns the provider email claim without retaining or exposing the token. */
  std::optional<std::string> getCodexAccountEmail(std::string_view accessToken) {
    const std::optional<json::value> payload = jwtPayload(accessToken);
    if (!payload) return std::nullopt;
    const json::value* profileValue = rt::get(*payload, "https://api.openai.com/profile");
    const json::value* profile = utils::isRecord(profileValue) ? profileValue : nullptr;
    if (std::optional<std::string> email = normalizeCodexAccountEmail(rt::get(profile, "email"))) return email;
    return normalizeCodexAccountEmail(rt::get(*payload, "email"));
  }

  bool needsRefresh(const CodexAuthState& auth) {
    const std::optional<double> expMs = getJwtExpMs(auth.access_token);
    const double now = rt::dateNow();
    if (rt::truthy(expMs)) return *expMs - now < 2 * 60'000;
    return now - auth.updated_at_ms > 7 * 60'000;
  }

  bool accessTokenExpired(const CodexAuthState& auth) {
    const std::optional<double> expMs = getJwtExpMs(auth.access_token);
    return expMs.has_value() && *expMs <= rt::dateNow();
  }

  std::optional<std::string> codexAuthWarningForError(const rt::Reason& error) {
    const CodexError* codexError = codexErrorOf(error);
    if (codexError != nullptr &&
        (codexError->code == "codex_auth_invalid" || codexError->code == "codex_auth_refresh_failed" || codexError->code == "refresh_token_reused"))
      return std::string(CODEX_AUTH_REAUTH_WARNING);
    return std::nullopt;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexRefreshLease& value) {
    out = Writer().required("owner", value.owner).required("lease_until_ms", value.lease_until_ms).done();
  }

  CodexRefreshLease tag_invoke(const json::value_to_tag<CodexRefreshLease>&, const json::value& value) {
    const Reader in(value, "CodexRefreshLease");
    return CodexRefreshLease{.owner = in.required<std::string>("owner"), .lease_until_ms = in.required<double>("lease_until_ms")};
  }

  void setCodexResponseAccountTelemetry(const rt::Response& response, double slot, const std::string& accountId) {
    codexSlotByResponse.set(response, slot);
    codexAccountIdByResponse.set(response, accountId);
  }

  void setCodexResponseActiveTelemetry(const rt::Response& response, const RoutingAccount& routing) {
    if (!routing.activeGeneration) return;
    setCodexResponseActiveTelemetryRecord(response, CodexResponseActiveTelemetry{routing.activeGeneration, routing.activeTransitionReason.toOptional()});
  }

  void inheritCodexResponseActiveTelemetry(const rt::Response& response, const rt::Response& source) {
    const std::optional<CodexResponseActiveTelemetry> telemetry = codexActiveTelemetryByResponse.get(source);
    if (telemetry) codexActiveTelemetryByResponse.set(response, *telemetry);
  }

  /** Null fields mean the response was not admitted through a durable active decision. */
  CodexResponseActiveTelemetry getCodexResponseActiveTelemetry(const rt::Response& response) {
    return codexActiveTelemetryByResponse.get(response).value_or(CodexResponseActiveTelemetry{std::nullopt, std::nullopt});
  }

  rt::Response withCodexWarnings(const rt::Response& response, const std::vector<std::string>& warnings) {
    rt::Headers headers(response.headers());
    std::vector<std::string> existing;
    if (const std::optional<std::string> header = headers.get("x-uos-warning")) {
      for (const std::string& value : rt::split(*header, ",")) {
        std::string trimmed = rt::trim(value);
        if (!trimmed.empty()) existing.push_back(std::move(trimmed));
      }
    }
    for (const std::string& warning : warnings) {
      if (std::find(existing.begin(), existing.end(), warning) == existing.end()) existing.push_back(warning);
    }
    if (existing.empty()) return response;
    headers.set("x-uos-warning", rt::join(existing, ", "));
    rt::ResponseInit init;
    init.status = response.status();
    init.statusText = response.statusText();
    init.headers = headers;
    rt::Response decorated(rt::bodyInit(response.body()), init);
    const std::optional<std::string> routingError = codexRoutingErrors.get(response);
    if (rt::truthy(routingError)) codexRoutingErrors.set(decorated, *routingError);
    const std::optional<RoutingAccount> probe = codexProbeByResponse.get(response);
    if (probe) codexProbeByResponse.set(decorated, *probe);
    const std::optional<double> slot = codexSlotByResponse.get(response);
    if (slot) codexSlotByResponse.set(decorated, *slot);
    const std::optional<std::string> accountId = codexAccountIdByResponse.get(response);
    if (accountId) codexAccountIdByResponse.set(decorated, *accountId);
    const std::optional<CodexResponseActiveTelemetry> activeTelemetry = codexActiveTelemetryByResponse.get(response);
    if (activeTelemetry) codexActiveTelemetryByResponse.set(decorated, *activeTelemetry);
    const std::optional<std::string> authWarning = codexAuthWarnings.get(response);
    if (authWarning) codexAuthWarnings.set(decorated, *authWarning);
    if (codexTerminalOutcomeByResponse.has(response)) codexTerminalOutcomeByResponse.add(decorated);
    return decorated;
  }

  rt::Response withCodexAuthWarning(const rt::Response& response, const std::string& warning) {
    rt::Response decorated = withCodexWarnings(response, std::vector<std::string>{warning});
    codexAuthWarnings.set(decorated, warning);
    return decorated;
  }

  /** The metadata stays isolate-local and never becomes a response header or durable credential record. */
  std::optional<RoutingAccount> getCodexRoutingProbe(const rt::Response& response) { return codexProbeByResponse.get(response); }

  /** The slot is isolate-local telemetry only; account IDs never leave the routing layer. */
  std::optional<double> getCodexResponseSlot(const rt::Response& response) { return codexSlotByResponse.get(response); }

  /** Returns a stable digest while keeping the raw account ID inside this module. */
  rt::Task<std::optional<std::string>> getCodexResponseAccountCohortId(rt::Response response) {
    const std::optional<std::string> accountId = codexAccountIdByResponse.get(response);
    if (!accountId) co_return std::nullopt;
    rt::Promise<std::string> hashing(utils::sha256Hex(std::string("uos-prompt-cache-account-cohort-v1") + '\0' + *accountId));
    const std::string digest = co_await hashing.wait();
    co_return digest;
  }

  /**
   * Detach a response from its recovery probe without claiming success. Failed,
   * cancelled, or incomplete streams release ordinary routing, but retain the
   * durable ambiguity tombstone until a later recovery probe proves the account
   * healthy.
   */
  rt::Task<void> releaseCodexResponseProbe(rt::Response response) {
    const std::optional<CodexResponseTerminalOutcome> terminal = beginCodexResponseTerminalOutcome(response);
    if (!terminal) co_return;
    if (terminal->probe) {
      rt::Promise<void> transition(routing_mutations::releaseCodexRoutingProbe(*terminal->probe));
      rt::Promise<void> completing(completeCodexProbeTransition(transition));
      co_await completing.wait();
    }
  }

  /** Only a validated upstream `response.completed` event may clear the recovery probe. */
  rt::Task<void> markCodexResponseCompleted(rt::Response response) {
    const std::optional<CodexResponseTerminalOutcome> terminal = beginCodexResponseTerminalOutcome(response);
    if (!terminal) co_return;
    if (terminal->probe) {
      rt::Promise<void> transition(routing_mutations::markCodexSuccess(*terminal->probe));
      rt::Promise<void> completing(completeCodexProbeTransition(transition));
      co_await completing.wait();
    }
    if (terminal->accountId) {
      rt::Promise<void> recording = provider::health::recordCodexProviderHealth(*terminal->accountId, "success", static_cast<double>(response.status()), {},
                                                                                codexProviderRequestId(response));
      (void)recording.catch_(rt::ignoreRejection);
    }
  }

  /** A trustworthy failure after 2xx headers degrades health without treating cancellation or incompletion as failure. */
  rt::Task<void> markCodexResponseUpstreamError(rt::Response response) {
    const std::optional<CodexResponseTerminalOutcome> terminal = beginCodexResponseTerminalOutcome(response);
    if (!terminal) co_return;
    if (response.ok() && terminal->accountId) {
      rt::Promise<void> recording = provider::health::recordCodexProviderHealth(*terminal->accountId, "upstream_error", static_cast<double>(response.status()), {},
                                                                                codexProviderRequestId(response));
      (void)recording.catch_(rt::ignoreRejection);
    }
    if (terminal->probe) {
      rt::Promise<void> transition(routing_mutations::releaseCodexRoutingProbe(*terminal->probe));
      rt::Promise<void> completing(completeCodexProbeTransition(transition));
      co_await completing.wait();
    }
  }

  void cacheCodexAuthPool(const CodexAuthPoolState& pool) {
    authCacheGeneration += 1;
    cachedAuthPool = pool;
    cachedAuthPoolExpiresAtMs = rt::dateNow() + CODEX_AUTH_CACHE_TTL_MS;
  }

  void resetCodexAuthCacheForTest() {
    authCacheGeneration += 1;
    cachedAuthPool = std::nullopt;
    cachedAuthPoolExpiresAtMs = 0;
    authPoolEntryInFlight = std::nullopt;
    refreshesInFlight.clear();
    codexProbeTransitionsInFlight.clear();
    capacity_routing::resetCodexAccountRoutingForTest();
  }

  rt::Task<CodexAuthPoolEntry> getAuthPoolEntry(bool forceKv, bool bypassInFlight) {
    if (!forceKv && cachedAuthPool && rt::dateNow() < cachedAuthPoolExpiresAtMs) co_return poolOnlyEntry(*cachedAuthPool);

    // A banked-reset retry uses this after a verified, expensive side effect.
    // It must not inherit an in-flight read that began before an auth-pool
    // rotation. A direct strong read is deliberately narrower than the normal
    // warm-cache single-flight path.
    if (bypassInFlight) {
      rt::Promise<CodexAuthPoolEntry> loading(loadAuthPoolEntry(authCacheGeneration));
      CodexAuthPoolEntry entry = co_await loading.wait();
      co_return entry;
    }

    // A bounded single read makes credential replacement converge across warm
    // isolates without restoring a KV lookup to every inference request.
    if (authPoolEntryInFlight) {
      const rt::Promise<CodexAuthPoolEntry> inFlight = *authPoolEntryInFlight;
      CodexAuthPoolEntry entry = co_await inFlight.wait();
      co_return entry;
    }
    rt::Promise<CodexAuthPoolEntry> loading(loadAuthPoolEntry(authCacheGeneration));
    authPoolEntryInFlight = loading.finally_([] { authPoolEntryInFlight = std::nullopt; });
    const rt::Promise<CodexAuthPoolEntry> inFlight = *authPoolEntryInFlight;
    CodexAuthPoolEntry entry = co_await inFlight.wait();
    co_return entry;
  }
}
