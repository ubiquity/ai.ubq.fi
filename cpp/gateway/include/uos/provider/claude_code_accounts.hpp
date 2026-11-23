#pragma once
// Port of src/provider/claude-code-accounts.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>; `kv: Kv | null` is one whose null pointer is
// null. `env?: ClaudeCodeEnvLookup` is an overload without the argument; an empty
// ClaudeCodeEnvLookup is undefined as well. `options = {}` and other optional trailing parameters
// are overloads. parseClaudeCodeAccountStore validates stored values, so it takes rt::KvValue (a
// json::value converts implicitly; a default-constructed KvValue is undefined).
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_fwd.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::provider::claude_code_accounts {
  inline const rt::KvKey CLAUDE_CODE_ACCOUNTS_KEY{"uos_ai", "claude_code_accounts", "v1"};

  /** Locally assigned identity; neither a label nor an opaque ID verifies provider identity. */
  struct ClaudeCodeAccountBinding {
    std::string account_id;
    std::string account_cohort_id;
    std::string credential_fingerprint;
    std::string evidence_generation;
    claude_code_credentials::ClaudeCodeResolvedCredential credential;
    std::optional<claude_code_credentials::ClaudeCodeWorkspaceState> workspace;  // ... | null
    friend bool operator==(const ClaudeCodeAccountBinding&, const ClaudeCodeAccountBinding&) = default;
  };

  struct ClaudeCodeAccountStore {
    double v = 1;  // `1`
    std::vector<ClaudeCodeAccountBinding> accounts;
    std::optional<std::string> active_account_id;  // string | null
    std::string active_reason;
    double updated_at_ms;
    friend bool operator==(const ClaudeCodeAccountStore&, const ClaudeCodeAccountStore&) = default;
  };

  /** Dispatch-only secret-bearing value. Never serialize this binding into admin JSON. */
  struct ClaudeCodeAccountIdentity {
    std::string account_id;
    std::string account_cohort_id;
    claude_code_credentials::ClaudeCodeCredentialKind credential_kind;
    std::optional<std::string> workspace_id;  // string | null
    friend bool operator==(const ClaudeCodeAccountIdentity&, const ClaudeCodeAccountIdentity&) = default;
  };

  ClaudeCodeAccountIdentity claudeCodeAccountIdentity(const ClaudeCodeAccountBinding& binding);

  // `(value: unknown): ClaudeCodeAccountStore | null`; std::nullopt is null.
  std::optional<ClaudeCodeAccountStore> parseClaudeCodeAccountStore(const rt::KvValue& value);

  /** A present empty roster is a tombstone: old singleton/env credentials cannot resurrect. */
  // Throws Error("Claude account roster is invalid; refusing legacy fallback") as the TS does.
  rt::Task<std::optional<ClaudeCodeAccountStore>> readClaudeCodeAccountStore(std::shared_ptr<rt::Kv> kv);

  /** Exactly-once compatible bootstrap; legacy keys remain untouched and unbound evidence is not imported. */
  rt::Task<ClaudeCodeAccountStore> loadClaudeCodeAccountStore(std::shared_ptr<rt::Kv> kv);
  rt::Task<ClaudeCodeAccountStore> loadClaudeCodeAccountStore(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env);

  // `options: Readonly<{ accountId?: string }>` of resolveClaudeCodeAccount.
  struct ResolveClaudeCodeAccountOptions {
    std::optional<std::string> accountId{};
  };

  // `kv: Kv | null`; std::nullopt is null.
  rt::Task<std::optional<ClaudeCodeAccountBinding>> resolveClaudeCodeAccount(std::shared_ptr<rt::Kv> kv);
  rt::Task<std::optional<ClaudeCodeAccountBinding>> resolveClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env);
  rt::Task<std::optional<ClaudeCodeAccountBinding>> resolveClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env,
                                                                            ResolveClaudeCodeAccountOptions options);

  // `Promise<KvEntryMaybe<ClaudeCodeAccountStore> | null>`: the TS type argument is an unchecked
  // annotation on the raw roster entry, so the result is the raw entry (rt::KvEntryMaybe<>, its
  // value the stored roster as read). std::nullopt is null.
  rt::Task<std::optional<rt::KvEntryMaybe<>>> currentClaudeCodeAccountEntry(std::shared_ptr<rt::Kv> kv, ClaudeCodeAccountBinding binding);

  // `kv: Kv | null`
  rt::Task<bool> isClaudeCodeAccountCurrent(std::shared_ptr<rt::Kv> kv, ClaudeCodeAccountBinding binding);

  struct AccountUpdate {
    std::optional<std::string> accountId{};
    std::optional<bool> createNew{};
    std::optional<claude_code_credentials::ClaudeCodeResolvedCredential> credential{};
    // `workspace?: ClaudeCodeWorkspaceState | null`: undefined keeps the previous workspace, null clears it.
    rt::Nullable<claude_code_credentials::ClaudeCodeWorkspaceState> workspace{};
    std::optional<bool> activate{};
    std::optional<ClaudeCodeAccountBinding> expected{};
  };

  // Throws the TS's Error messages ("Claude account was not found", "Claude account changed before
  // update", ...).
  rt::Task<ClaudeCodeAccountBinding> updateClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, AccountUpdate input);
  rt::Task<ClaudeCodeAccountBinding> updateClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, AccountUpdate input, claude_code::ClaudeCodeEnvLookup env);

  // `reason = "admin_selection"`, `expected?: ClaudeCodeAccountBinding`
  rt::Task<bool> selectClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, std::string accountId);
  rt::Task<bool> selectClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, std::string accountId, std::string reason);
  rt::Task<bool> selectClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, std::string accountId, std::string reason, std::optional<ClaudeCodeAccountBinding> expected);

  // `accountId?: string`: std::nullopt is undefined (an empty string is falsy but not nullish, as in
  // the TS).
  rt::Task<bool> removeClaudeCodeAccount(std::shared_ptr<rt::Kv> kv);
  rt::Task<bool> removeClaudeCodeAccount(std::shared_ptr<rt::Kv> kv, std::optional<std::string> accountId);

  // ---- JSON conversions of the stored roster records (members in declaration order) --------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeAccountBinding& value);
  ClaudeCodeAccountBinding tag_invoke(const json::value_to_tag<ClaudeCodeAccountBinding>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeAccountStore& value);
  ClaudeCodeAccountStore tag_invoke(const json::value_to_tag<ClaudeCodeAccountStore>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeAccountIdentity& value);
  ClaudeCodeAccountIdentity tag_invoke(const json::value_to_tag<ClaudeCodeAccountIdentity>&, const json::value& value);
}
