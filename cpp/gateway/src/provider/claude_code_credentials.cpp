#include "uos/provider/claude_code_credentials.hpp"
#include "uos/rt/memory.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/provider/claude_code_accounts.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/weak.hpp"

#include <utility>

namespace uos::provider::claude_code_credentials {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    const rt::KvGetOptions STRONG{.consistency = rt::KvConsistencyLevel::strong};

    // `typeof value === "object" && value !== null` (utils.isRecord) on a stored value.

    std::optional<std::string> trimmedOrNull(const rt::KvValue& value) {
      if (!value.isString()) return std::nullopt;
      std::string trimmed = rt::trim(value.asString());
      if (rt::length(trimmed) == 0) return std::nullopt;
      return trimmed;
    }
    std::optional<std::string> trimmedOrNull(std::string_view value) {
      std::string trimmed = rt::trim(value);
      if (rt::length(trimmed) == 0) return std::nullopt;
      return trimmed;
    }
    std::optional<std::string> trimmedOrNull(const std::optional<std::string>& value) {
      if (!value) return std::nullopt;
      return trimmedOrNull(std::string_view(*value));
    }

    std::optional<double> finitePositive(const rt::KvValue& value) {
      if (!value.isNumber()) return std::nullopt;
      const double number = value.asNumber();
      if (!rt::Number::isFinite(number) || !(number > 0)) return std::nullopt;
      return rt::Math::trunc(number);
    }

    std::optional<ClaudeCodeCredentialKind> parseCredentialKind(const rt::KvValue& value) {
      if (value.isString() && (value.asString() == "api_key" || value.asString() == "oauth")) return value.asString();
      return std::nullopt;
    }

    std::optional<ClaudeCodeStoredCredential> cachedCredential;
    double cachedCredentialExpiresAtMs = 0;
    // `let cachedCredentialKv: Kv | null`: the store's identity (never a raw pointer).
    std::weak_ptr<const void> cachedCredentialKv;
    rt::WeakMap<rt::Kv, double> stateGenerations;

    void advanceStateGeneration(const std::shared_ptr<rt::Kv>& kv) { stateGenerations.set(*kv, claudeCodeStateGeneration(kv) + 1); }

    void cacheStoredCredential(std::optional<ClaudeCodeStoredCredential> credential) {
      std::size_t bytes = 0;
      if (credential) {
        bytes = sizeof(*credential) + rt::memory::bytes(credential->kind) + rt::memory::bytes(credential->secret) + rt::memory::bytes(credential->hint);
        if (credential->label) bytes += rt::memory::bytes(*credential->label);
      }
      rt::memory::setCache(rt::memory::Cache::claude_credentials, credential ? 1 : 0, bytes);
      cachedCredential = std::move(credential);
      cachedCredentialExpiresAtMs = rt::dateNow() + CLAUDE_CODE_CREDENTIAL_CACHE_TTL_MS;
    }

    bool sameKv(const std::weak_ptr<const void>& cached, const std::shared_ptr<rt::Kv>& kv) {
      const std::shared_ptr<const void> held = cached.lock();
      return held != nullptr && held.get() == kv->identity();
    }

    // The process.env fallback of `env ?? ((name) => process.env[name])`; every caller folds null and
    // undefined together.
    std::optional<std::string> lookupEnv(const claude_code::ClaudeCodeEnvLookup& env, std::string_view name) {
      if (env) return env(std::string(name));
      return rt::env::get(name);
    }

    // `roster.accounts.find((account) => account.account_id === roster.active_account_id)`
    const claude_code_accounts::ClaudeCodeAccountBinding* activeAccount(const claude_code_accounts::ClaudeCodeAccountStore& roster) {
      for (const auto& account : roster.accounts)
        if (roster.active_account_id && account.account_id == *roster.active_account_id) return &account;
      return nullptr;
    }
  }

  ClaudeCodeCredentialKind classifyClaudeCodeCredentialKind(const std::string& secret) {
    static const rt::Regex OAUTH("^sk-ant-o(at|rt)", "i");
    const std::string value = rt::trim(secret);
    if (OAUTH.test(value)) return "oauth";
    return "api_key";
  }

  std::string partialClaudeCodeCredentialHint(const std::string& secret) {
    const std::string value = rt::trim(secret);
    if (static_cast<double>(rt::length(value)) <= CLAUDE_CODE_HINT_PREFIX_LENGTH + CLAUDE_CODE_HINT_SUFFIX_LENGTH) return "configured";
    return rt::slice(value, 0, CLAUDE_CODE_HINT_PREFIX_LENGTH) + "..." + rt::slice(value, -CLAUDE_CODE_HINT_SUFFIX_LENGTH);
  }

  std::optional<ClaudeCodeStoredCredential> parseClaudeCodeStoredCredential(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return std::nullopt;
    const std::optional<std::string> secret = trimmedOrNull(value["secret"]);
    std::optional<ClaudeCodeCredentialKind> kind = parseCredentialKind(value["kind"]);
    if (!kind && secret) kind = classifyClaudeCodeCredentialKind(*secret);
    const std::optional<double> updatedAtMs = finitePositive(value["updated_at_ms"]);
    if (!secret || !kind || !updatedAtMs) return std::nullopt;
    if (static_cast<double>(rt::length(*secret)) > CLAUDE_CODE_CREDENTIAL_MAX_LENGTH) return std::nullopt;
    std::optional<std::string> hint = trimmedOrNull(value["hint"]);
    return ClaudeCodeStoredCredential{
      .kind = *kind,
      .secret = *secret,
      .hint = hint ? std::move(*hint) : partialClaudeCodeCredentialHint(*secret),
      .label = trimmedOrNull(value["label"]),
      .updated_at_ms = *updatedAtMs,
    };
  }

  std::optional<ClaudeCodeWorkspaceState> parseClaudeCodeWorkspaceState(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return std::nullopt;
    std::optional<std::string> id = trimmedOrNull(value["id"]);
    if (!id || static_cast<double>(rt::length(*id)) > CLAUDE_CODE_WORKSPACE_ID_MAX_LENGTH) return std::nullopt;
    const rt::KvValue& rawSource = value["source"];
    const bool known = rawSource.isString() && (rawSource.asString() == "kv" || rawSource.asString() == "env" || rawSource.asString() == "discovered");
    return ClaudeCodeWorkspaceState{
      .id = std::move(*id),
      .name = trimmedOrNull(value["name"]),
      .source = known ? rawSource.asString() : std::string("kv"),
      .discovered_at_ms = finitePositive(value["discovered_at_ms"]),
      .verified_at_ms = finitePositive(value["verified_at_ms"]),
      .last_error = trimmedOrNull(value["last_error"]),
    };
  }

  std::optional<std::string> parseClaudeCodeWorkspaceId(std::string_view value) {
    std::optional<std::string> id = trimmedOrNull(value);
    if (!id || static_cast<double>(rt::length(*id)) > CLAUDE_CODE_WORKSPACE_ID_MAX_LENGTH) return std::nullopt;
    if (!rt::startsWith(*id, "wrkspc_")) return std::nullopt;
    return id;
  }
  std::optional<std::string> parseClaudeCodeWorkspaceId(const json::value& value) {
    if (!value.is_string()) return std::nullopt;
    return parseClaudeCodeWorkspaceId(std::string_view(value.get_string()));
  }
  std::optional<std::string> parseClaudeCodeWorkspaceId(const json::value* value) {
    if (value == nullptr) return std::nullopt;
    return parseClaudeCodeWorkspaceId(*value);
  }

  double claudeCodeStateGeneration(const std::shared_ptr<rt::Kv>& kv) {
    if (!kv) return 0;
    const double* generation = stateGenerations.find(*kv);
    return generation != nullptr ? *generation : 0;
  }

  void resetClaudeCodeCredentialCacheForTest() {
    cachedCredential = std::nullopt;
    rt::memory::setCache(rt::memory::Cache::claude_credentials, 0, 0);
    cachedCredentialExpiresAtMs = 0;
    cachedCredentialKv.reset();
  }

  rt::Task<std::optional<ClaudeCodeStoredCredential>> loadClaudeCodeStoredCredential(std::shared_ptr<rt::Kv> kv) {
    if (!kv) co_return std::nullopt;
    rt::Promise<std::optional<claude_code_accounts::ClaudeCodeAccountStore>> rosterRead(claude_code_accounts::readClaudeCodeAccountStore(kv));
    const std::optional<claude_code_accounts::ClaudeCodeAccountStore> roster = co_await rosterRead.wait();
    if (roster) {
      const claude_code_accounts::ClaudeCodeAccountBinding* account = activeAccount(*roster);
      if (account == nullptr || account->credential.source != "kv") co_return std::nullopt;
      const rt::KvValue credential(json::value_from(account->credential));
      co_return parseClaudeCodeStoredCredential(credential);
    }
    if (sameKv(cachedCredentialKv, kv) && cachedCredentialExpiresAtMs > rt::dateNow()) co_return cachedCredential;
    const double generation = claudeCodeStateGeneration(kv);
    const rt::KvEntryMaybe<> entry = co_await kv->get(CLAUDE_CODE_AUTH_KV_KEY, STRONG);
    if (generation != claudeCodeStateGeneration(kv)) {
      rt::Promise<std::optional<ClaudeCodeStoredCredential>> retry(loadClaudeCodeStoredCredential(kv));
      std::optional<ClaudeCodeStoredCredential> reloaded = co_await retry.wait();
      co_return reloaded;
    }
    std::optional<ClaudeCodeStoredCredential> parsed = parseClaudeCodeStoredCredential(entry.value);
    cachedCredentialKv = kv->weakIdentity();
    cacheStoredCredential(parsed);
    co_return parsed;
  }

  rt::Task<std::optional<ClaudeCodeStoredCredential>> storeClaudeCodeStoredCredential(std::shared_ptr<rt::Kv> kv, StoreClaudeCodeStoredCredentialInput input) {
    if (!kv) co_return std::nullopt;
    const std::optional<std::string> secret = trimmedOrNull(std::string_view(input.secret));
    if (!secret || static_cast<double>(rt::length(*secret)) > CLAUDE_CODE_CREDENTIAL_MAX_LENGTH) co_return std::nullopt;
    ClaudeCodeStoredCredential stored{
      .kind = classifyClaudeCodeCredentialKind(*secret),
      .secret = *secret,
      .hint = partialClaudeCodeCredentialHint(*secret),
      .label = trimmedOrNull(input.label),
      .updated_at_ms = rt::dateNow(),
    };
    claude_code_accounts::AccountUpdate update;
    update.credential = ClaudeCodeResolvedCredential{
      .kind = stored.kind,
      .secret = stored.secret,
      .hint = stored.hint,
      .label = stored.label,
      .source = "kv",
      .updated_at_ms = stored.updated_at_ms,
    };
    rt::Promise<claude_code_accounts::ClaudeCodeAccountBinding> updated(claude_code_accounts::updateClaudeCodeAccount(kv, std::move(update)));
    co_await updated.wait();
    advanceStateGeneration(kv);
    cachedCredentialKv = kv->weakIdentity();
    cacheStoredCredential(stored);
    co_return stored;
  }

  rt::Task<bool> clearClaudeCodeStoredCredential(std::shared_ptr<rt::Kv> kv) {
    if (!kv) co_return false;
    rt::Promise<bool> removed(claude_code_accounts::removeClaudeCodeAccount(kv));
    co_await removed.wait();
    advanceStateGeneration(kv);
    cachedCredentialKv = kv->weakIdentity();
    cacheStoredCredential(std::nullopt);
    co_return true;
  }

  rt::Task<std::optional<ClaudeCodeWorkspaceState>> loadClaudeCodeWorkspaceState(std::shared_ptr<rt::Kv> kv) {
    if (!kv) co_return std::nullopt;
    rt::Promise<std::optional<claude_code_accounts::ClaudeCodeAccountStore>> rosterRead(claude_code_accounts::readClaudeCodeAccountStore(kv));
    const std::optional<claude_code_accounts::ClaudeCodeAccountStore> roster = co_await rosterRead.wait();
    if (roster) {
      const claude_code_accounts::ClaudeCodeAccountBinding* account = activeAccount(*roster);
      if (account == nullptr) co_return std::nullopt;
      co_return account->workspace;
    }
    const rt::KvEntryMaybe<> entry = co_await kv->get(CLAUDE_CODE_WORKSPACE_KV_KEY, STRONG);
    co_return parseClaudeCodeWorkspaceState(entry.value);
  }

  rt::Task<bool> storeClaudeCodeWorkspaceState(std::shared_ptr<rt::Kv> kv, ClaudeCodeWorkspaceState state) {
    if (!kv) co_return false;
    rt::Promise<std::optional<claude_code_accounts::ClaudeCodeAccountStore>> rosterRead(claude_code_accounts::readClaudeCodeAccountStore(kv));
    const std::optional<claude_code_accounts::ClaudeCodeAccountStore> roster = co_await rosterRead.wait();
    if (roster && roster->active_account_id && !roster->active_account_id->empty()) {
      claude_code_accounts::AccountUpdate update;
      update.workspace = state;
      rt::Promise<claude_code_accounts::ClaudeCodeAccountBinding> updated(claude_code_accounts::updateClaudeCodeAccount(kv, std::move(update)));
      co_await updated.wait();
    } else {
      const rt::KvValue stored(json::value_from(state));
      co_await kv->set(CLAUDE_CODE_WORKSPACE_KV_KEY, stored);
    }
    advanceStateGeneration(kv);
    co_return true;
  }

  std::optional<ClaudeCodeResolvedCredential> readClaudeCodeEnvCredential(const claude_code::ClaudeCodeEnvLookup& env) {
    const std::optional<std::string> apiKey = trimmedOrNull(lookupEnv(env, CLAUDE_API_KEY_ENV));
    if (apiKey) {
      return ClaudeCodeResolvedCredential{
        .kind = classifyClaudeCodeCredentialKind(*apiKey),
        .secret = *apiKey,
        .hint = partialClaudeCodeCredentialHint(*apiKey),
        .label = std::nullopt,
        .source = std::string(CLAUDE_API_KEY_ENV),
        .updated_at_ms = std::nullopt,
      };
    }
    const std::optional<std::string> oauthToken = trimmedOrNull(lookupEnv(env, CLAUDE_CODE_OAUTH_TOKEN_ENV));
    if (oauthToken) {
      return ClaudeCodeResolvedCredential{
        .kind = "oauth",
        .secret = *oauthToken,
        .hint = partialClaudeCodeCredentialHint(*oauthToken),
        .label = std::nullopt,
        .source = std::string(CLAUDE_CODE_OAUTH_TOKEN_ENV),
        .updated_at_ms = std::nullopt,
      };
    }
    return std::nullopt;
  }
  std::optional<ClaudeCodeResolvedCredential> readClaudeCodeEnvCredential() { return readClaudeCodeEnvCredential(claude_code::ClaudeCodeEnvLookup()); }

  rt::Task<std::optional<ClaudeCodeResolvedCredential>> resolveClaudeCodeCredential(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env) {
    if (kv) {
      rt::Promise<std::optional<claude_code_accounts::ClaudeCodeAccountBinding>> resolving(claude_code_accounts::resolveClaudeCodeAccount(kv, env));
      const std::optional<claude_code_accounts::ClaudeCodeAccountBinding> account = co_await resolving.wait();
      if (!account) co_return std::nullopt;
      co_return account->credential;
    }
    co_return readClaudeCodeEnvCredential(env);
  }
  rt::Task<std::optional<ClaudeCodeResolvedCredential>> resolveClaudeCodeCredential(std::shared_ptr<rt::Kv> kv) {
    return resolveClaudeCodeCredential(std::move(kv), claude_code::ClaudeCodeEnvLookup());
  }

  std::optional<std::string> readClaudeCodeWorkspaceIdEnv(const claude_code::ClaudeCodeEnvLookup& env) {
    const std::optional<std::string> value = lookupEnv(env, CLAUDE_CODE_WORKSPACE_ID_ENV);
    if (!value) return std::nullopt;
    return parseClaudeCodeWorkspaceId(std::string_view(*value));
  }
  std::optional<std::string> readClaudeCodeWorkspaceIdEnv() { return readClaudeCodeWorkspaceIdEnv(claude_code::ClaudeCodeEnvLookup()); }

  rt::Task<std::optional<ClaudeCodeResolvedWorkspace>> resolveClaudeCodeWorkspace(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env) {
    if (kv) {
      rt::Promise<std::optional<claude_code_accounts::ClaudeCodeAccountStore>> rosterRead(claude_code_accounts::readClaudeCodeAccountStore(kv));
      const std::optional<claude_code_accounts::ClaudeCodeAccountStore> roster = co_await rosterRead.wait();
      if (roster) {
        const claude_code_accounts::ClaudeCodeAccountBinding* account = activeAccount(*roster);
        if (account == nullptr || !account->workspace) co_return std::nullopt;
        co_return ClaudeCodeResolvedWorkspace{.id = account->workspace->id, .name = account->workspace->name, .source = account->workspace->source};
      }
    }
    rt::Promise<std::optional<ClaudeCodeWorkspaceState>> storedRead(loadClaudeCodeWorkspaceState(kv));
    const std::optional<ClaudeCodeWorkspaceState> stored = co_await storedRead.wait();
    if (stored) co_return ClaudeCodeResolvedWorkspace{.id = stored->id, .name = stored->name, .source = stored->source};
    std::optional<std::string> fromEnv = readClaudeCodeWorkspaceIdEnv(env);
    if (fromEnv) co_return ClaudeCodeResolvedWorkspace{.id = std::move(*fromEnv), .name = std::nullopt, .source = "env"};
    co_return std::nullopt;
  }
  rt::Task<std::optional<ClaudeCodeResolvedWorkspace>> resolveClaudeCodeWorkspace(std::shared_ptr<rt::Kv> kv) {
    return resolveClaudeCodeWorkspace(std::move(kv), claude_code::ClaudeCodeEnvLookup());
  }

  ClaudeCodeWorkspaceState mergeClaudeCodeWorkspaceState(const std::optional<ClaudeCodeWorkspaceState>& previous, const MergeClaudeCodeWorkspaceStateInput& input) {
    const double nowMs = input.nowMs ? *input.nowMs : rt::dateNow();
    const bool sameId = previous && previous->id == input.id;
    const std::optional<std::string> previousName = sameId ? previous->name : std::nullopt;
    std::optional<double> discoveredAtMs = sameId ? previous->discovered_at_ms : std::nullopt;
    if (input.source == "discovered") discoveredAtMs = nowMs;
    std::optional<double> verifiedAtMs = sameId ? previous->verified_at_ms : std::nullopt;
    if (input.verified.value_or(false)) verifiedAtMs = nowMs;
    return {
      .id = input.id,
      .name = input.name ? input.name : previousName,
      .source = input.source,
      .discovered_at_ms = discoveredAtMs,
      .verified_at_ms = verifiedAtMs,
      .last_error = input.last_error,
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeStoredCredential& value) {
    out = Writer()
            .required("kind", value.kind)
            .required("secret", value.secret)
            .required("hint", value.hint)
            .orNull("label", value.label)
            .required("updated_at_ms", value.updated_at_ms)
            .done();
  }
  ClaudeCodeStoredCredential tag_invoke(const json::value_to_tag<ClaudeCodeStoredCredential>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeStoredCredential");
    return {
      .kind = in.required<std::string>("kind"),
      .secret = in.required<std::string>("secret"),
      .hint = in.required<std::string>("hint"),
      .label = in.orNull<std::string>("label"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeResolvedCredential& value) {
    out = Writer()
            .required("kind", value.kind)
            .required("secret", value.secret)
            .required("hint", value.hint)
            .orNull("label", value.label)
            .required("source", value.source)
            .orNull("updated_at_ms", value.updated_at_ms)
            .done();
  }
  ClaudeCodeResolvedCredential tag_invoke(const json::value_to_tag<ClaudeCodeResolvedCredential>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeResolvedCredential");
    return {
      .kind = in.required<std::string>("kind"),
      .secret = in.required<std::string>("secret"),
      .hint = in.required<std::string>("hint"),
      .label = in.orNull<std::string>("label"),
      .source = in.required<std::string>("source"),
      .updated_at_ms = in.orNull<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeWorkspaceState& value) {
    out = Writer()
            .required("id", value.id)
            .orNull("name", value.name)
            .required("source", value.source)
            .orNull("discovered_at_ms", value.discovered_at_ms)
            .orNull("verified_at_ms", value.verified_at_ms)
            .orNull("last_error", value.last_error)
            .done();
  }
  ClaudeCodeWorkspaceState tag_invoke(const json::value_to_tag<ClaudeCodeWorkspaceState>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeWorkspaceState");
    return {
      .id = in.required<std::string>("id"),
      .name = in.orNull<std::string>("name"),
      .source = in.required<std::string>("source"),
      .discovered_at_ms = in.orNull<double>("discovered_at_ms"),
      .verified_at_ms = in.orNull<double>("verified_at_ms"),
      .last_error = in.orNull<std::string>("last_error"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeResolvedWorkspace& value) {
    out = Writer().required("id", value.id).orNull("name", value.name).required("source", value.source).done();
  }
  ClaudeCodeResolvedWorkspace tag_invoke(const json::value_to_tag<ClaudeCodeResolvedWorkspace>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeResolvedWorkspace");
    return {.id = in.required<std::string>("id"), .name = in.orNull<std::string>("name"), .source = in.required<std::string>("source")};
  }
}
