#pragma once
// Port of src/provider/claude-code-credentials.ts.
//
// `kv: Kv | null` is std::shared_ptr<rt::Kv> (a null pointer is null). `env?: ClaudeCodeEnvLookup`
// is an overload without the argument; an empty ClaudeCodeEnvLookup is undefined as well (the
// process.env lookup). The parsers of stored records take rt::KvValue (a json::value converts
// implicitly; a default-constructed KvValue is undefined). parseClaudeCodeWorkspaceId takes
// json::value plus a pointer overload (nullptr is undefined) and a std::string_view overload.
#include "uos/provider/claude_code_fwd.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace uos::provider::claude_code_credentials {
  inline const rt::KvKey CLAUDE_CODE_AUTH_KV_KEY{"ubq_ai", "claude_code_auth"};
  inline const rt::KvKey CLAUDE_CODE_WORKSPACE_KV_KEY{"ubq_ai", "claude_code_workspace"};
  inline constexpr std::string_view CLAUDE_API_KEY_ENV = "CLAUDE_API_KEY";
  inline constexpr std::string_view CLAUDE_CODE_OAUTH_TOKEN_ENV = "CLAUDE_CODE_OAUTH_TOKEN";
  inline constexpr std::string_view CLAUDE_CODE_WORKSPACE_ID_ENV = "CLAUDE_CODE_WORKSPACE_ID";
  inline constexpr double CLAUDE_CODE_CREDENTIAL_CACHE_TTL_MS = 60'000;
  /** Console hints show a long prefix; 14 characters covers `sk-ant-usr-1A6`. */
  inline constexpr double CLAUDE_CODE_HINT_PREFIX_LENGTH = 14;
  inline constexpr double CLAUDE_CODE_HINT_SUFFIX_LENGTH = 4;
  inline constexpr double CLAUDE_CODE_CREDENTIAL_MAX_LENGTH = 512;
  inline constexpr double CLAUDE_CODE_WORKSPACE_ID_MAX_LENGTH = 256;

  /**
   * `api_key` credentials travel as `x-api-key`; `oauth` credentials travel as
   * `Authorization: Bearer` plus the OAuth beta header, exactly like the
   * documented `claude setup-token` flow the tier shipped with. Everything else
   * that looks like a Console key (`sk-ant-api*`, `sk-ant-usr*`) is an API key.
   */
  // "api_key" | "oauth"
  using ClaudeCodeCredentialKind = std::string;
  // "kv" | typeof CLAUDE_API_KEY_ENV | typeof CLAUDE_CODE_OAUTH_TOKEN_ENV
  using ClaudeCodeCredentialSource = std::string;

  ClaudeCodeCredentialKind classifyClaudeCodeCredentialKind(const std::string& secret);

  /** The Console-style partial hint (`sk-ant-usr-1A6...BwAA`) with the secret absent. */
  std::string partialClaudeCodeCredentialHint(const std::string& secret);

  struct ClaudeCodeStoredCredential {
    ClaudeCodeCredentialKind kind;
    std::string secret;
    std::string hint;
    std::optional<std::string> label;  // string | null
    double updated_at_ms;
    friend bool operator==(const ClaudeCodeStoredCredential&, const ClaudeCodeStoredCredential&) = default;
  };

  struct ClaudeCodeResolvedCredential {
    ClaudeCodeCredentialKind kind;
    std::string secret;
    std::string hint;
    std::optional<std::string> label;  // string | null
    ClaudeCodeCredentialSource source;
    std::optional<double> updated_at_ms;  // number | null
    friend bool operator==(const ClaudeCodeResolvedCredential&, const ClaudeCodeResolvedCredential&) = default;
  };

  /** A stored credential must parse completely; a partial record is rejected. */
  // `(value: unknown): ClaudeCodeStoredCredential | null`; std::nullopt is null.
  std::optional<ClaudeCodeStoredCredential> parseClaudeCodeStoredCredential(const rt::KvValue& value);

  // "kv" | "env" | "discovered"
  using ClaudeCodeWorkspaceSource = std::string;

  struct ClaudeCodeWorkspaceState {
    std::string id;
    std::optional<std::string> name;  // string | null
    ClaudeCodeWorkspaceSource source;
    std::optional<double> discovered_at_ms;  // number | null
    std::optional<double> verified_at_ms;    // number | null
    /** Last workspace-resolution failure text, sanitized for operator display. */
    std::optional<std::string> last_error;  // string | null
    friend bool operator==(const ClaudeCodeWorkspaceState&, const ClaudeCodeWorkspaceState&) = default;
  };

  struct ClaudeCodeResolvedWorkspace {
    std::string id;
    std::optional<std::string> name;  // string | null
    ClaudeCodeWorkspaceSource source;
    friend bool operator==(const ClaudeCodeResolvedWorkspace&, const ClaudeCodeResolvedWorkspace&) = default;
  };

  // `(value: unknown): ClaudeCodeWorkspaceState | null`; std::nullopt is null.
  std::optional<ClaudeCodeWorkspaceState> parseClaudeCodeWorkspaceState(const rt::KvValue& value);

  // `(value: unknown): string | null`; std::nullopt is null.
  std::optional<std::string> parseClaudeCodeWorkspaceId(const json::value& value);
  std::optional<std::string> parseClaudeCodeWorkspaceId(const json::value* value);
  std::optional<std::string> parseClaudeCodeWorkspaceId(std::string_view value);

  // `WeakMap<Kv, number>` generation of `kv` (0 for null or an unseen store).
  double claudeCodeStateGeneration(const std::shared_ptr<rt::Kv>& kv);

  void resetClaudeCodeCredentialCacheForTest();

  rt::Task<std::optional<ClaudeCodeStoredCredential>> loadClaudeCodeStoredCredential(std::shared_ptr<rt::Kv> kv);

  // `input: Readonly<{ secret: string; label?: string | null }>` of storeClaudeCodeStoredCredential.
  struct StoreClaudeCodeStoredCredentialInput {
    std::string secret;
    // null and undefined are treated alike (no label).
    std::optional<std::string> label{};
  };

  rt::Task<std::optional<ClaudeCodeStoredCredential>> storeClaudeCodeStoredCredential(std::shared_ptr<rt::Kv> kv, StoreClaudeCodeStoredCredentialInput input);

  rt::Task<bool> clearClaudeCodeStoredCredential(std::shared_ptr<rt::Kv> kv);

  rt::Task<std::optional<ClaudeCodeWorkspaceState>> loadClaudeCodeWorkspaceState(std::shared_ptr<rt::Kv> kv);

  rt::Task<bool> storeClaudeCodeWorkspaceState(std::shared_ptr<rt::Kv> kv, ClaudeCodeWorkspaceState state);

  /**
   * Environment credentials carry their own source so the admin panel can say
   * where a working credential came from. `CLAUDE_API_KEY` wins over the legacy
   * OAuth token when both are set, because the API key is the Console-billed
   * direction and the OAuth token is the owner-gated legacy path.
   */
  std::optional<ClaudeCodeResolvedCredential> readClaudeCodeEnvCredential();
  std::optional<ClaudeCodeResolvedCredential> readClaudeCodeEnvCredential(const claude_code::ClaudeCodeEnvLookup& env);

  /** KV credential first, then `CLAUDE_API_KEY`, then `CLAUDE_CODE_OAUTH_TOKEN`. */
  rt::Task<std::optional<ClaudeCodeResolvedCredential>> resolveClaudeCodeCredential(std::shared_ptr<rt::Kv> kv);
  rt::Task<std::optional<ClaudeCodeResolvedCredential>> resolveClaudeCodeCredential(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env);

  std::optional<std::string> readClaudeCodeWorkspaceIdEnv();
  std::optional<std::string> readClaudeCodeWorkspaceIdEnv(const claude_code::ClaudeCodeEnvLookup& env);

  /** KV workspace first, then the environment pin, then a discovered workspace. */
  rt::Task<std::optional<ClaudeCodeResolvedWorkspace>> resolveClaudeCodeWorkspace(std::shared_ptr<rt::Kv> kv);
  rt::Task<std::optional<ClaudeCodeResolvedWorkspace>> resolveClaudeCodeWorkspace(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env);

  // `input: Readonly<{ id; name?: string | null; source; nowMs?: number; last_error?: string | null;
  // verified?: boolean }>` of mergeClaudeCodeWorkspaceState. The TS folds null and undefined
  // together for `name` and `last_error`.
  struct MergeClaudeCodeWorkspaceStateInput {
    std::string id;
    std::optional<std::string> name{};
    ClaudeCodeWorkspaceSource source;
    std::optional<double> nowMs{};  // undefined: Date.now()
    std::optional<std::string> last_error{};
    std::optional<bool> verified{};
  };

  /**
   * Fold discovery or validation results into stored workspace state. A pinned
   * KV or environment selection is never replaced by a discovery result; the
   * failure detail is retained only for operator display.
   */
  // `previous: ClaudeCodeWorkspaceState | null`
  ClaudeCodeWorkspaceState mergeClaudeCodeWorkspaceState(const std::optional<ClaudeCodeWorkspaceState>& previous, const MergeClaudeCodeWorkspaceStateInput& input);

  // ---- JSON conversions of the stored records (members in declaration order) ---------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeStoredCredential& value);
  ClaudeCodeStoredCredential tag_invoke(const json::value_to_tag<ClaudeCodeStoredCredential>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeResolvedCredential& value);
  ClaudeCodeResolvedCredential tag_invoke(const json::value_to_tag<ClaudeCodeResolvedCredential>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeWorkspaceState& value);
  ClaudeCodeWorkspaceState tag_invoke(const json::value_to_tag<ClaudeCodeWorkspaceState>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeResolvedWorkspace& value);
  ClaudeCodeResolvedWorkspace tag_invoke(const json::value_to_tag<ClaudeCodeResolvedWorkspace>&, const json::value& value);
}
