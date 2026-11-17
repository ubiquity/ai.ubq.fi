#pragma once
// Port of src/provider/claude-code-models.ts.
//
// `options = {}` parameters and optional trailing parameters are overloads. `kv?: Kv | null` is
// std::optional<std::shared_ptr<rt::Kv>> (std::nullopt is undefined, which reads getKv(); a null
// pointer is null); `kv: Kv` is a non-null std::shared_ptr<rt::Kv>.
// `Pick<ClaudeCodeResolvedCredential, "kind" | "secret">` is claude_code::ClaudeCodeAuthHeaders
// (the same `{ kind, secret }` shape; see claude_code_fwd.hpp).
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_fwd.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::provider::claude_code_models {
  // Members in the order the TS builds a model (modelFromRecord writes `max_output_tokens` right
  // after `context_window_tokens`, then the observed capability flags), which is the order a
  // serialized model shows.
  struct ClaudeCodeModel {
    std::string id;
    std::optional<std::string> display_name{};
    double created;
    std::optional<double> context_window_tokens{};
    std::optional<double> max_output_tokens{};
    std::optional<bool> supports_disabled_thinking{};
    std::optional<bool> supports_adaptive_thinking{};
    std::optional<std::vector<std::string>> supported_reasoning_levels{};
    std::optional<bool> supports_image_input{};
    std::optional<bool> supports_pdf_input{};
    friend bool operator==(const ClaudeCodeModel&, const ClaudeCodeModel&) = default;
  };

  struct ClaudeCodeModelsSnapshot {
    std::vector<ClaudeCodeModel> models;
    double updated_at_ms;
    std::optional<bool> stale{};
    friend bool operator==(const ClaudeCodeModelsSnapshot&, const ClaudeCodeModelsSnapshot&) = default;
  };

  // `type DiscoveryOptions = Readonly<{ fetcher?: ClaudeCodeFetch; now?: () => number }>`
  struct DiscoveryOptions {
    claude_code::ClaudeCodeFetch fetcher{};  // empty: the global fetch
    std::function<double()> now{};           // empty: Date.now
  };

  // `type CatalogOptions = DiscoveryOptions & Readonly<{ kv?; env?; force?; accountBinding?; expectedBinding? }>`
  struct CatalogOptions : DiscoveryOptions {
    // `Readonly<{ credential: Pick<ClaudeCodeResolvedCredential, "kind" | "secret">; workspaceId: string | null }>`
    struct ExpectedBinding {
      claude_code::ClaudeCodeAuthHeaders credential;
      std::optional<std::string> workspaceId;  // string | null
      friend bool operator==(const ExpectedBinding&, const ExpectedBinding&) = default;
    };

    std::optional<std::shared_ptr<rt::Kv>> kv{};
    claude_code::ClaudeCodeEnvLookup env{};  // empty: undefined
    std::optional<bool> force{};
    std::optional<claude_code_accounts::ClaudeCodeAccountBinding> accountBinding{};
    std::optional<ExpectedBinding> expectedBinding{};
  };

  class ClaudeCodeModelsError : public rt::JsError {
  public:
    const std::optional<double> status;  // number | null

    explicit ClaudeCodeModelsError(std::optional<double> status_)
      : rt::JsError("Claude model discovery is unavailable", "ClaudeCodeModelsError"), status(status_) {}
  };

  /** Native effort strings are source observations, never a model-name tier table. */
  std::vector<std::string> claudeCodeReasoningLevels(const ClaudeCodeModel& model);

  /** List only IDs observed on this account, following Anthropic's after_id cursor to completion. */
  // `workspaceId: string | null`; throws ClaudeCodeModelsError as the TS does.
  rt::Task<ClaudeCodeModelsSnapshot> discoverClaudeCodeModels(claude_code::ClaudeCodeAuthHeaders credential, std::optional<std::string> workspaceId);
  rt::Task<ClaudeCodeModelsSnapshot> discoverClaudeCodeModels(claude_code::ClaudeCodeAuthHeaders credential, std::optional<std::string> workspaceId,
                                                              DiscoveryOptions options);

  /** A failed refresh retains only this generation's snapshot and explicitly marks it stale. */
  // std::nullopt is null.
  rt::Task<std::optional<ClaudeCodeModelsSnapshot>> fetchClaudeCodeModels();
  rt::Task<std::optional<ClaudeCodeModelsSnapshot>> fetchClaudeCodeModels(CatalogOptions options);

  /** Reuse the successful upload validation without publishing it for a rotated credential/workspace. */
  // `workspaceId: string | null`, `env?: ClaudeCodeEnvLookup` (an empty one is undefined too),
  // `accountBinding?: ClaudeCodeAccountBinding`.
  rt::Task<void> cacheValidatedClaudeCodeModels(std::shared_ptr<rt::Kv> kv, std::string secret, std::optional<std::string> workspaceId,
                                                ClaudeCodeModelsSnapshot snapshot);
  rt::Task<void> cacheValidatedClaudeCodeModels(std::shared_ptr<rt::Kv> kv, std::string secret, std::optional<std::string> workspaceId,
                                                ClaudeCodeModelsSnapshot snapshot, claude_code::ClaudeCodeEnvLookup env);
  rt::Task<void> cacheValidatedClaudeCodeModels(std::shared_ptr<rt::Kv> kv, std::string secret, std::optional<std::string> workspaceId,
                                                ClaudeCodeModelsSnapshot snapshot, claude_code::ClaudeCodeEnvLookup env,
                                                std::optional<claude_code_accounts::ClaudeCodeAccountBinding> accountBinding);

  void resetClaudeCodeModelsCacheForTest();

  // ---- JSON conversions of the snake_case records (members in declaration order) -----------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeModel& value);
  ClaudeCodeModel tag_invoke(const json::value_to_tag<ClaudeCodeModel>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeModelsSnapshot& value);
  ClaudeCodeModelsSnapshot tag_invoke(const json::value_to_tag<ClaudeCodeModelsSnapshot>&, const json::value& value);
}
