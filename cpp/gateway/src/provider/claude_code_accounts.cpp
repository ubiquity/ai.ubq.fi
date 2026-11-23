#include "uos/provider/claude_code_accounts.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/utils.hpp"

#include <unordered_set>
#include <utility>

namespace uos::provider::claude_code_accounts {
  namespace {
    using claude_code_credentials::ClaudeCodeResolvedCredential;
    using claude_code_credentials::ClaudeCodeWorkspaceState;
    using rt::record::Reader;
    using rt::record::Writer;

    constexpr double MAX_ACCOUNTS = 32;
    constexpr int MAX_CAS_ATTEMPTS = 12;

    const rt::KvGetOptions STRONG{.consistency = rt::KvConsistencyLevel::strong};

    // `typeof value === "object" && value !== null` (utils.isRecord) on a stored value.

    bool lowerHex(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }

    // `typeof value === "string" && /^[a-f0-9-]{36}$/.test(value)` (the class is ASCII and `$` ends
    // the input, so the byte length is the UTF-16 length).
    bool opaqueId(const rt::KvValue& value) {
      if (!value.isString()) return false;
      const std::string& text = value.asString();
      if (text.size() != 36) return false;
      for (const char ch : text)
        if (!lowerHex(ch) && ch != '-') return false;
      return true;
    }

    // `/^[a-f0-9]{64}$/.test(value)`
    bool sha256HexText(const std::string& text) {
      if (text.size() != 64) return false;
      for (const char ch : text)
        if (!lowerHex(ch)) return false;
      return true;
    }

    // `String(raw.source)` as far as it can equal "kv", "CLAUDE_API_KEY" or "CLAUDE_CODE_OAUTH_TOKEN":
    // a string, or an array whose only element stringifies to one (the join of two or more elements
    // holds a comma). std::nullopt for every value whose String() cannot be one of them.
    std::optional<std::string> sourceText(const rt::KvValue& value) {
      if (value.isString()) return value.asString();
      if (!value.isArray()) return std::nullopt;
      const rt::KvArray& items = value.asArray();
      if (items.size() != 1) return std::nullopt;
      const rt::KvValue& item = items.front();
      if (item.isNullish() || item.is<rt::KvHole>()) return std::nullopt;
      return sourceText(item);
    }

    rt::Task<std::string> fingerprint(const ClaudeCodeResolvedCredential& credential, const std::optional<ClaudeCodeWorkspaceState>& workspace) {
      json::array parts;
      parts.emplace_back(json::string(credential.kind));
      parts.emplace_back(json::string(credential.secret));
      if (workspace) parts.emplace_back(json::string(workspace->id));
      else parts.emplace_back(nullptr);
      return utils::sha256Hex(rt::jsonStringify(parts));
    }

    std::optional<ClaudeCodeAccountBinding> parseAccount(const rt::KvValue& value) {
      if (!rt::kvIsObject(value) || !opaqueId(value["account_id"]) || !opaqueId(value["account_cohort_id"]) || !opaqueId(value["evidence_generation"]))
        return std::nullopt;
      const rt::KvValue& credentialFingerprint = value["credential_fingerprint"];
      if (!credentialFingerprint.isString() || !sha256HexText(credentialFingerprint.asString()) || !rt::kvIsObject(value["credential"])) return std::nullopt;
      const rt::KvValue& raw = value["credential"];
      // `{ ...raw, updated_at_ms: raw.updated_at_ms ?? 1 }`: only a plain object can carry the members the
      // parser reads; the spread of an array, Map, Date, ... holds none of them.
      rt::KvObject spread = raw.isObject() ? raw.asObject() : rt::KvObject();
      const rt::KvValue& rawUpdatedAtMs = raw["updated_at_ms"];
      spread.set("updated_at_ms", rawUpdatedAtMs.isNullish() ? rt::KvValue(1) : rawUpdatedAtMs);
      const std::optional<claude_code_credentials::ClaudeCodeStoredCredential> parsed =
        claude_code_credentials::parseClaudeCodeStoredCredential(rt::KvValue(std::move(spread)));
      if (!parsed) return std::nullopt;
      const std::optional<std::string> source = sourceText(raw["source"]);
      if (!source || !(*source == "kv" || *source == "CLAUDE_API_KEY" || *source == "CLAUDE_CODE_OAUTH_TOKEN")) return std::nullopt;
      const rt::KvValue& workspaceValue = value["workspace"];
      std::optional<ClaudeCodeWorkspaceState> workspace;
      if (!workspaceValue.isNull()) {
        workspace = claude_code_credentials::parseClaudeCodeWorkspaceState(workspaceValue);
        if (!workspace) return std::nullopt;
      }
      return ClaudeCodeAccountBinding{
        .account_id = value["account_id"].asString(),
        .account_cohort_id = value["account_cohort_id"].asString(),
        .credential_fingerprint = credentialFingerprint.asString(),
        .evidence_generation = value["evidence_generation"].asString(),
        .credential =
          ClaudeCodeResolvedCredential{
            .kind = parsed->kind,
            .secret = parsed->secret,
            .hint = parsed->hint,
            .label = parsed->label,
            .source = *source,
            .updated_at_ms = rawUpdatedAtMs.isNull() ? std::nullopt : std::optional<double>(parsed->updated_at_ms),
          },
        .workspace = std::move(workspace),
      };
    }

    rt::Task<ClaudeCodeAccountBinding> makeAccount(ClaudeCodeResolvedCredential credential, std::optional<ClaudeCodeWorkspaceState> workspace,
                                                   std::optional<ClaudeCodeAccountBinding> previous) {
      rt::Promise<std::string> hashing(fingerprint(credential, workspace));
      const std::string digest = co_await hashing.wait();
      const bool same = previous && previous->credential_fingerprint == digest;
      std::string accountId = previous ? previous->account_id : rt::crypto::randomUUID();
      std::string accountCohortId = same ? previous->account_cohort_id : rt::crypto::randomUUID();
      std::string evidenceGeneration = same ? previous->evidence_generation : rt::crypto::randomUUID();
      co_return ClaudeCodeAccountBinding{
        .account_id = std::move(accountId),
        .account_cohort_id = std::move(accountCohortId),
        .credential_fingerprint = digest,
        .evidence_generation = std::move(evidenceGeneration),
        .credential = std::move(credential),
        .workspace = std::move(workspace),
      };
    }

    ClaudeCodeAccountStore emptyAccountStore() {
      return {.v = 1, .accounts = {}, .active_account_id = std::nullopt, .active_reason = "unconfigured", .updated_at_ms = rt::dateNow()};
    }

    std::optional<ClaudeCodeWorkspaceState> bootstrapWorkspace(const rt::KvValue& value, const claude_code::ClaudeCodeEnvLookup& env) {
      std::optional<ClaudeCodeWorkspaceState> stored = claude_code_credentials::parseClaudeCodeWorkspaceState(value);
      if (stored) return stored;
      std::optional<std::string> id = claude_code_credentials::readClaudeCodeWorkspaceIdEnv(env);
      if (!id) return std::nullopt;
      return ClaudeCodeWorkspaceState{
        .id = std::move(*id),
        .name = std::nullopt,
        .source = "env",
        .discovered_at_ms = std::nullopt,
        .verified_at_ms = std::nullopt,
        .last_error = std::nullopt,
      };
    }

    const ClaudeCodeAccountBinding* findAccount(const ClaudeCodeAccountStore& store, const std::optional<std::string>& accountId) {
      if (!accountId) return nullptr;
      for (const ClaudeCodeAccountBinding& account : store.accounts)
        if (account.account_id == *accountId) return &account;
      return nullptr;
    }

    bool hasAccount(const ClaudeCodeAccountStore& store, const std::string& accountId) {
      for (const ClaudeCodeAccountBinding& account : store.accounts)
        if (account.account_id == accountId) return true;
      return false;
    }

    rt::KvValue storedRoster(const ClaudeCodeAccountStore& store) { return rt::KvValue(json::value_from(store)); }

    bool sameAccountBinding(const ClaudeCodeAccountBinding* current, const ClaudeCodeAccountBinding& expected) {
      return current != nullptr && current->account_id == expected.account_id && current->account_cohort_id == expected.account_cohort_id &&
             current->evidence_generation == expected.evidence_generation && current->credential_fingerprint == expected.credential_fingerprint;
    }

    struct ChangedAccountStore {
      ClaudeCodeAccountBinding account;
      ClaudeCodeAccountStore next;
    };

    rt::Task<ChangedAccountStore> changedAccountStore(ClaudeCodeAccountStore store, AccountUpdate input) {
      const bool createNew = input.createNew.value_or(false);
      const std::optional<std::string> wanted = input.accountId ? input.accountId : store.active_account_id;
      std::optional<ClaudeCodeAccountBinding> previous;
      if (!createNew)
        if (const ClaudeCodeAccountBinding* found = findAccount(store, wanted)) previous = *found;
      if (input.accountId && !input.accountId->empty() && !previous) throw rt::JsError("Claude account was not found");
      if (input.expected && !sameAccountBinding(previous ? &*previous : nullptr, *input.expected)) throw rt::JsError("Claude account changed before update");
      std::optional<ClaudeCodeResolvedCredential> credential = input.credential;
      if (!credential && previous) credential = previous->credential;
      if (!credential) throw rt::JsError("Claude credential is required");
      if (!previous && static_cast<double>(store.accounts.size()) >= MAX_ACCOUNTS) throw rt::JsError("Claude account storage limit reached");
      std::optional<ClaudeCodeWorkspaceState> workspace;
      if (input.workspace == rt::undefined) {
        if (previous) workspace = previous->workspace;
      } else if (input.workspace.hasValue()) {
        workspace = *input.workspace;
      }
      rt::Promise<ClaudeCodeAccountBinding> making(makeAccount(std::move(*credential), std::move(workspace), previous));
      ClaudeCodeAccountBinding account = co_await making.wait();
      if (!previous)
        for (const ClaudeCodeAccountBinding& configured : store.accounts)
          if (configured.credential_fingerprint == account.credential_fingerprint) throw rt::JsError("This credential/workspace binding is already configured");
      std::vector<ClaudeCodeAccountBinding> accounts;
      if (previous) {
        for (const ClaudeCodeAccountBinding& item : store.accounts) accounts.push_back(item.account_id == account.account_id ? account : item);
      } else {
        accounts = store.accounts;
        accounts.push_back(account);
      }
      const bool activate = input.activate == std::optional<bool>(true) || !store.active_account_id;
      const std::string newActiveReason = input.activate.value_or(false) ? "admin_selection" : "first_configured";
      ClaudeCodeAccountStore next{
        .v = store.v,
        .accounts = std::move(accounts),
        .active_account_id = activate ? std::optional<std::string>(account.account_id) : store.active_account_id,
        .active_reason = activate ? newActiveReason : store.active_reason,
        .updated_at_ms = rt::dateNow(),
      };
      co_return ChangedAccountStore{.account = std::move(account), .next = std::move(next)};
    }
  }

  ClaudeCodeAccountIdentity claudeCodeAccountIdentity(const ClaudeCodeAccountBinding& binding) {
    return {
      .account_id = binding.account_id,
      .account_cohort_id = binding.account_cohort_id,
      .credential_kind = binding.credential.kind,
      .workspace_id = binding.workspace ? std::optional<std::string>(binding.workspace->id) : std::nullopt,
    };
  }

  std::optional<ClaudeCodeAccountStore> parseClaudeCodeAccountStore(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return std::nullopt;
    const rt::KvValue& version = value["v"];
    if (!version.isNumber() || version.asNumber() != 1) return std::nullopt;
    const rt::KvValue& accountsValue = value["accounts"];
    if (!accountsValue.isArray() || static_cast<double>(accountsValue.asArray().size()) > MAX_ACCOUNTS) return std::nullopt;
    // `accounts.map(parseAccount)`, `.some(... === null)` and `.filter(...)` all skip array holes.
    std::vector<ClaudeCodeAccountBinding> valid;
    for (const rt::KvValue& item : accountsValue.asArray()) {
      if (item.is<rt::KvHole>()) continue;
      std::optional<ClaudeCodeAccountBinding> account = parseAccount(item);
      if (!account) return std::nullopt;
      valid.push_back(std::move(*account));
    }
    std::unordered_set<std::string> ids;
    for (const ClaudeCodeAccountBinding& account : valid) ids.insert(account.account_id);
    if (ids.size() != valid.size()) return std::nullopt;
    const rt::KvValue& active = value["active_account_id"];
    if (!active.isNull()) {
      bool configured = false;
      if (active.isString())
        for (const ClaudeCodeAccountBinding& account : valid)
          if (account.account_id == active.asString()) configured = true;
      if (!configured) return std::nullopt;
    }
    const rt::KvValue& activeReason = value["active_reason"];
    const rt::KvValue& updatedAtMs = value["updated_at_ms"];
    if (!activeReason.isString() || !updatedAtMs.isNumber() || !rt::Number::isSafeInteger(updatedAtMs.asNumber()) || updatedAtMs.asNumber() <= 0)
      return std::nullopt;
    return ClaudeCodeAccountStore{
      .v = 1,
      .accounts = std::move(valid),
      .active_account_id = active.isNull() ? std::nullopt : std::optional<std::string>(active.asString()),
      .active_reason = activeReason.asString(),
      .updated_at_ms = updatedAtMs.asNumber(),
    };
  }

  rt::Task<std::optional<ClaudeCodeAccountStore>> readClaudeCodeAccountStore(std::shared_ptr<rt::Kv> kv) {
    const rt::KvEntryMaybe<> entry = co_await kv->get(CLAUDE_CODE_ACCOUNTS_KEY, STRONG);
    if (entry.value.isNull()) co_return std::nullopt;
    std::optional<ClaudeCodeAccountStore> store = parseClaudeCodeAccountStore(entry.value);
    if (!store) throw rt::JsError("Claude account roster is invalid; refusing legacy fallback");
    co_return store;
  }

  rt::Task<ClaudeCodeAccountStore> loadClaudeCodeAccountStore(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env) {
    for (int attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt++) {
      const rt::KvEntryMaybe<> entry = co_await kv->get(CLAUDE_CODE_ACCOUNTS_KEY, STRONG);
      if (!entry.value.isNull()) {
        std::optional<ClaudeCodeAccountStore> store = parseClaudeCodeAccountStore(entry.value);
        if (!store) throw rt::JsError("Claude account roster is invalid; refusing legacy fallback");
        co_return std::move(*store);
      }
      const rt::KvEntryMaybe<> legacyCredential = co_await kv->get(claude_code_credentials::CLAUDE_CODE_AUTH_KV_KEY, STRONG);
      const std::optional<claude_code_credentials::ClaudeCodeStoredCredential> stored =
        claude_code_credentials::parseClaudeCodeStoredCredential(legacyCredential.value);
      std::optional<ClaudeCodeResolvedCredential> credential;
      if (stored) {
        credential = ClaudeCodeResolvedCredential{
          .kind = stored->kind,
          .secret = stored->secret,
          .hint = stored->hint,
          .label = stored->label,
          .source = "kv",
          .updated_at_ms = stored->updated_at_ms,
        };
      } else {
        credential = claude_code_credentials::readClaudeCodeEnvCredential(env);
      }
      // Absence is not an intentional removal: a passive read must not retire a future first credential.
      if (!credential) co_return emptyAccountStore();
      const rt::KvEntryMaybe<> legacyWorkspace = co_await kv->get(claude_code_credentials::CLAUDE_CODE_WORKSPACE_KV_KEY, STRONG);
      std::optional<ClaudeCodeWorkspaceState> workspace = bootstrapWorkspace(legacyWorkspace.value, env);
      rt::Promise<ClaudeCodeAccountBinding> making(makeAccount(std::move(*credential), std::move(workspace), std::nullopt));
      ClaudeCodeAccountBinding account = co_await making.wait();
      std::string activeAccountId = account.account_id;
      ClaudeCodeAccountStore store{
        .v = 1,
        .accounts = {},
        .active_account_id = std::move(activeAccountId),
        .active_reason = "singleton_bootstrap",
        .updated_at_ms = rt::dateNow(),
      };
      store.accounts.push_back(std::move(account));
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(entry, legacyCredential, legacyWorkspace).set(CLAUDE_CODE_ACCOUNTS_KEY, storedRoster(store));
      const rt::KvCommitResult committed = co_await operation->commit();
      if (committed.ok) co_return store;
    }
    throw rt::JsError("Claude account bootstrap changed repeatedly");
  }
  rt::Task<ClaudeCodeAccountStore> loadClaudeCodeAccountStore(std::shared_ptr<rt::Kv> kv) {
    return loadClaudeCodeAccountStore(std::move(kv), claude_code::ClaudeCodeEnvLookup());
  }

  rt::Task<std::optional<ClaudeCodeAccountBinding>> resolveClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env,
                                                                            ResolveClaudeCodeAccountOptions options) {
    if (!kv) co_return std::nullopt;
    rt::Promise<ClaudeCodeAccountStore> loading(loadClaudeCodeAccountStore(kv, std::move(env)));
    const ClaudeCodeAccountStore store = co_await loading.wait();
    const ClaudeCodeAccountBinding* account = findAccount(store, options.accountId ? options.accountId : store.active_account_id);
    if (account == nullptr) co_return std::nullopt;
    co_return *account;
  }
  rt::Task<std::optional<ClaudeCodeAccountBinding>> resolveClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env) {
    return resolveClaudeCodeAccount(std::move(kv), std::move(env), ResolveClaudeCodeAccountOptions());
  }
  rt::Task<std::optional<ClaudeCodeAccountBinding>> resolveClaudeCodeAccount(std::shared_ptr<rt::Kv> kv) {
    return resolveClaudeCodeAccount(std::move(kv), claude_code::ClaudeCodeEnvLookup(), ResolveClaudeCodeAccountOptions());
  }

  rt::Task<std::optional<rt::KvEntryMaybe<>>> currentClaudeCodeAccountEntry(std::shared_ptr<rt::Kv> kv, ClaudeCodeAccountBinding binding) {
    rt::KvEntryMaybe<> entry = co_await kv->get(CLAUDE_CODE_ACCOUNTS_KEY, STRONG);
    const std::optional<ClaudeCodeAccountStore> store = parseClaudeCodeAccountStore(entry.value);
    const ClaudeCodeAccountBinding* account = store ? findAccount(*store, binding.account_id) : nullptr;
    if (account != nullptr && account->account_cohort_id == binding.account_cohort_id && account->evidence_generation == binding.evidence_generation &&
        account->credential_fingerprint == binding.credential_fingerprint)
      co_return entry;
    co_return std::nullopt;
  }

  rt::Task<bool> isClaudeCodeAccountCurrent(std::shared_ptr<rt::Kv> kv, ClaudeCodeAccountBinding binding) {
    if (!kv) co_return false;
    rt::Promise<std::optional<rt::KvEntryMaybe<>>> reading(currentClaudeCodeAccountEntry(kv, std::move(binding)));
    const std::optional<rt::KvEntryMaybe<>> entry = co_await reading.wait();
    co_return entry.has_value();
  }

  rt::Task<ClaudeCodeAccountBinding> updateClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, AccountUpdate input, claude_code::ClaudeCodeEnvLookup env) {
    rt::Promise<ClaudeCodeAccountStore> loading(loadClaudeCodeAccountStore(kv, std::move(env)));
    co_await loading.wait();
    for (int attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt++) {
      const rt::KvEntryMaybe<> entry = co_await kv->get(CLAUDE_CODE_ACCOUNTS_KEY, STRONG);
      std::optional<ClaudeCodeAccountStore> store = entry.value.isNull() ? std::optional<ClaudeCodeAccountStore>(emptyAccountStore())
                                                                         : parseClaudeCodeAccountStore(entry.value);
      if (!store) throw rt::JsError("Claude account roster is invalid");
      rt::Promise<ChangedAccountStore> changing(changedAccountStore(std::move(*store), input));
      ChangedAccountStore changed = co_await changing.wait();
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(entry).set(CLAUDE_CODE_ACCOUNTS_KEY, storedRoster(changed.next));
      const rt::KvCommitResult committed = co_await operation->commit();
      if (committed.ok) co_return std::move(changed.account);
    }
    throw rt::JsError("Claude account update changed repeatedly");
  }
  rt::Task<ClaudeCodeAccountBinding> updateClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, AccountUpdate input) {
    return updateClaudeCodeAccount(std::move(kv), std::move(input), claude_code::ClaudeCodeEnvLookup());
  }

  rt::Task<bool> selectClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, std::string accountId, std::string reason, std::optional<ClaudeCodeAccountBinding> expected) {
    for (int attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt++) {
      const rt::KvEntryMaybe<> entry = co_await kv->get(CLAUDE_CODE_ACCOUNTS_KEY, STRONG);
      const std::optional<ClaudeCodeAccountStore> store = parseClaudeCodeAccountStore(entry.value);
      if (!store || !hasAccount(*store, accountId)) co_return false;
      if (expected) {
        if (store->active_account_id != std::optional<std::string>(expected->account_id)) co_return false;
        rt::Promise<bool> checking(isClaudeCodeAccountCurrent(kv, *expected));
        const bool current = co_await checking.wait();
        if (!current) co_return false;
      }
      if (store->active_account_id == std::optional<std::string>(accountId)) co_return true;
      ClaudeCodeAccountStore next = *store;
      next.active_account_id = accountId;
      next.active_reason = reason;
      next.updated_at_ms = rt::dateNow();
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(entry).set(CLAUDE_CODE_ACCOUNTS_KEY, storedRoster(next));
      const rt::KvCommitResult committed = co_await operation->commit();
      if (committed.ok) co_return true;
    }
    co_return false;
  }
  rt::Task<bool> selectClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, std::string accountId, std::string reason) {
    return selectClaudeCodeAccount(std::move(kv), std::move(accountId), std::move(reason), std::nullopt);
  }
  rt::Task<bool> selectClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, std::string accountId) {
    return selectClaudeCodeAccount(std::move(kv), std::move(accountId), std::string("admin_selection"), std::nullopt);
  }

  rt::Task<bool> removeClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, std::optional<std::string> accountId) {
    const bool named = accountId && !accountId->empty();
    if (!named) {
      rt::Promise<ClaudeCodeAccountStore> loading(loadClaudeCodeAccountStore(kv));
      co_await loading.wait();
    }
    for (int attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt++) {
      const rt::KvEntryMaybe<> entry = co_await kv->get(CLAUDE_CODE_ACCOUNTS_KEY, STRONG);
      std::optional<ClaudeCodeAccountStore> store = entry.value.isNull() ? std::optional<ClaudeCodeAccountStore>(emptyAccountStore())
                                                                         : parseClaudeCodeAccountStore(entry.value);
      if (!store) throw rt::JsError("Claude account roster is invalid");
      if (named && !hasAccount(*store, *accountId)) co_return false;
      const std::optional<std::string> target = accountId ? accountId : store->active_account_id;
      std::vector<ClaudeCodeAccountBinding> accounts;
      for (const ClaudeCodeAccountBinding& account : store->accounts)
        if (!target || account.account_id != *target) accounts.push_back(account);
      const bool removedActive = target == store->active_account_id;
      const std::size_t remaining = accounts.size();
      ClaudeCodeAccountStore next{
        .v = store->v,
        .accounts = std::move(accounts),
        .active_account_id = store->active_account_id,
        .active_reason = store->active_reason,
        .updated_at_ms = rt::dateNow(),
      };
      if (removedActive) {
        next.active_account_id = next.accounts.empty() ? std::nullopt : std::optional<std::string>(next.accounts.front().account_id);
        next.active_reason = "account_removed";
      }
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(entry).set(CLAUDE_CODE_ACCOUNTS_KEY, storedRoster(next));
      const rt::KvCommitResult committed = co_await operation->commit();
      if (committed.ok) co_return remaining != store->accounts.size();
    }
    co_return false;
  }
  rt::Task<bool> removeClaudeCodeAccount(std::shared_ptr<rt::Kv> kv) { return removeClaudeCodeAccount(std::move(kv), std::nullopt); }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeAccountBinding& value) {
    out = Writer()
            .required("account_id", value.account_id)
            .required("account_cohort_id", value.account_cohort_id)
            .required("credential_fingerprint", value.credential_fingerprint)
            .required("evidence_generation", value.evidence_generation)
            .required("credential", value.credential)
            .orNull("workspace", value.workspace)
            .done();
  }
  ClaudeCodeAccountBinding tag_invoke(const json::value_to_tag<ClaudeCodeAccountBinding>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeAccountBinding");
    return {
      .account_id = in.required<std::string>("account_id"),
      .account_cohort_id = in.required<std::string>("account_cohort_id"),
      .credential_fingerprint = in.required<std::string>("credential_fingerprint"),
      .evidence_generation = in.required<std::string>("evidence_generation"),
      .credential = in.required<ClaudeCodeResolvedCredential>("credential"),
      .workspace = in.orNull<ClaudeCodeWorkspaceState>("workspace"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeAccountStore& value) {
    out = Writer()
            .required("v", value.v)
            .required("accounts", value.accounts)
            .orNull("active_account_id", value.active_account_id)
            .required("active_reason", value.active_reason)
            .required("updated_at_ms", value.updated_at_ms)
            .done();
  }
  ClaudeCodeAccountStore tag_invoke(const json::value_to_tag<ClaudeCodeAccountStore>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeAccountStore");
    const double version = in.required<double>("v");
    if (version != 1) in.fail("v", rt::get(value, "v"), "1");
    return {
      .v = version,
      .accounts = in.required<std::vector<ClaudeCodeAccountBinding>>("accounts"),
      .active_account_id = in.orNull<std::string>("active_account_id"),
      .active_reason = in.required<std::string>("active_reason"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeAccountIdentity& value) {
    out = Writer()
            .required("account_id", value.account_id)
            .required("account_cohort_id", value.account_cohort_id)
            .required("credential_kind", value.credential_kind)
            .orNull("workspace_id", value.workspace_id)
            .done();
  }
  ClaudeCodeAccountIdentity tag_invoke(const json::value_to_tag<ClaudeCodeAccountIdentity>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeAccountIdentity");
    return {
      .account_id = in.required<std::string>("account_id"),
      .account_cohort_id = in.required<std::string>("account_cohort_id"),
      .credential_kind = in.required<std::string>("credential_kind"),
      .workspace_id = in.orNull<std::string>("workspace_id"),
    };
  }
}
