#pragma once
// Port of src/codex/experiment.ts.
// Codex capacity and cache scope experiment, split out of src/codex.ts.
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::codex::experiment {
  /**
   * Control-plane credentials for the redacted admin capacity snapshot. The
   * caller uses these only to make account-bound upstream reads; account IDs
   * and access tokens must never cross the HTTP response boundary.
   */
  struct CodexCapacityAccount {
    double slot;
    std::string account_id;
    std::string access_token;
    std::optional<std::string> email;
    friend bool operator==(const CodexCapacityAccount&, const CodexCapacityAccount&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexCapacityAccount& value);
  CodexCapacityAccount tag_invoke(const json::value_to_tag<CodexCapacityAccount>&, const json::value& value);

  [[nodiscard]] rt::Task<std::vector<CodexCapacityAccount>> getCodexCapacityAccounts();

  /**
   * Private control-plane state for the fixed prompt-cache experiment. Account
   * identities stay inside this transport module and are never persisted by the
   * experiment coordinator or returned from its admin endpoint.
   */
  struct CodexCacheScopeExperimentSession {
    // `readonly [string, string]`
    std::array<std::string, 2> expectedAccountIds;
    std::string authPoolVersionstamp;
    friend bool operator==(const CodexCacheScopeExperimentSession&, const CodexCacheScopeExperimentSession&) = default;
  };

  class CodexCacheScopeExperimentError : public rt::JsError {
  public:
    explicit CodexCacheScopeExperimentError(std::string message_) : rt::JsError(std::move(message_), "CodexCacheScopeExperimentError") {}
  };

  // `type CodexCacheScopeExperimentRefreshResult` (module-private in the TS), by `status`.
  struct CodexCacheScopeExperimentRefreshResultRefreshed {
    static constexpr std::string_view status = "refreshed";
    bool tokenChanged;
    CodexCacheScopeExperimentSession session;
  };
  struct CodexCacheScopeExperimentRefreshResultAuthPoolDrift {
    static constexpr std::string_view status = "auth_pool_drift";
  };
  using CodexCacheScopeExperimentRefreshResult =
    std::variant<CodexCacheScopeExperimentRefreshResultRefreshed, CodexCacheScopeExperimentRefreshResultAuthPoolDrift>;

  [[nodiscard]] rt::Task<CodexCacheScopeExperimentSession> beginCodexCacheScopeExperiment();

  /**
   * Refresh one pinned slot exactly once. The returned session carries the new
   * pool versionstamp; callers must use it for every following dispatch.
   */
  // `signal?: AbortSignal`: the overload without it, or std::nullopt, is undefined.
  [[nodiscard]] rt::Task<CodexCacheScopeExperimentRefreshResult> refreshCodexCacheScopeExperimentSlot(CodexCacheScopeExperimentSession session, double slot);
  [[nodiscard]] rt::Task<CodexCacheScopeExperimentRefreshResult> refreshCodexCacheScopeExperimentSlot(CodexCacheScopeExperimentSession session, double slot,
                                                                                                      std::optional<rt::AbortSignal> signal);

  // The `options` of fetchCodexResponsesForCacheScopeExperiment.
  struct FetchCodexResponsesForCacheScopeExperimentOptions {
    CodexCacheScopeExperimentSession session;
    double slot;
    std::string conversationId;
    std::optional<std::string> clientVersion{};  // string | null
    std::optional<rt::AbortSignal> signal{};
  };

  /**
   * Slot-pinned internal transport for the cache-scope experiment. It never
   * tries a sibling account or retries inference. Conversation IDs are internal
   * upstream headers, never public request fields or durable evidence.
   */
  // `body: unknown`
  [[nodiscard]] rt::Task<rt::Response> fetchCodexResponsesForCacheScopeExperiment(json::value body, FetchCodexResponsesForCacheScopeExperimentOptions options);
}
