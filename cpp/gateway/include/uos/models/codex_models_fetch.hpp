#pragma once
// Port of src/models/codex-models-fetch.ts.
// Codex models fetching and snapshots, split out of src/codex.ts.
#include "uos/models/codex_models.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace uos::models::codex_models_fetch {
  // `Readonly<{ clientVersion?: string | null; ifNoneMatch?: string | null; signal?: AbortSignal;
  // onProviderTransportFailure?: () => void }>`. std::nullopt is null and undefined alike; an empty
  // std::function is undefined.
  struct FetchCodexModelsOptions {
    std::optional<std::string> clientVersion{};
    std::optional<std::string> ifNoneMatch{};
    std::optional<rt::AbortSignal> signal{};
    std::function<void()> onProviderTransportFailure{};
  };

  // `options = {}` is the overload without it.
  [[nodiscard]] rt::Task<rt::Response> fetchCodexModels();
  [[nodiscard]] rt::Task<rt::Response> fetchCodexModels(FetchCodexModelsOptions options);

  [[nodiscard]] rt::Task<std::optional<codex_models::CodexModelsSnapshot>> loadCodexModelsSnapshot();

  // `candidate: string | null | undefined` (std::nullopt for both); the result is undefined as std::nullopt.
  std::optional<std::string> preserveCodexDefaultModel(const codex_models::CodexModelsSnapshot& snapshot, const std::optional<std::string>& candidate);

  // `kvOverride?: Kv | null`: the overload without it is undefined (getKv()); in the other, std::nullopt is
  // undefined and a null pointer is null.
  [[nodiscard]] rt::Task<std::optional<codex_models::CodexModelsSnapshot>> loadFullCodexModelsSnapshot();
  [[nodiscard]] rt::Task<std::optional<codex_models::CodexModelsSnapshot>> loadFullCodexModelsSnapshot(std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  [[nodiscard]] rt::Task<bool> storeCodexModelsSnapshot(codex_models::CodexModelsSnapshot snapshot);

  // `Readonly<{ reasoning?: Record<string, unknown> | null; instructions?: string | null }>`: an undefined
  // member is not written into the body, null is written as null.
  struct BuildCodexRequestOptions {
    rt::Nullable<json::object> reasoning{};
    rt::Nullable<std::string> instructions{};
  };

  // `options = {}` is the overload without it. `Record<string, unknown>`.
  json::object buildCodexRequest(const std::string& model, const std::vector<::uos::types::ResponseInputItem>& input);
  json::object buildCodexRequest(const std::string& model, const std::vector<::uos::types::ResponseInputItem>& input, const BuildCodexRequestOptions& options);

  // `Readonly<{ clientVersion?: string | null }>`
  struct ValidateCodexAuthJsonOptions {
    std::optional<std::string> clientVersion{};
  };

  // validateCodexAuthJson's result, by `ok`.
  struct ValidateCodexAuthJsonResultOk {
    static constexpr bool ok = true;
    ::uos::types::CodexAuthState auth;
    bool refreshed;
    double status;
    std::optional<std::string> contentType;
    // `unknown`: the parsed JSON body, or null.
    json::value models;
    std::string modelsBody;
    std::optional<std::string> etag;
    std::string clientVersion;
  };
  struct ValidateCodexAuthJsonResultNotOk {
    static constexpr bool ok = false;
    double status;
    std::string body;
  };
  using ValidateCodexAuthJsonResult = std::variant<ValidateCodexAuthJsonResultOk, ValidateCodexAuthJsonResultNotOk>;

  // `options = {}` is the overload without it.
  [[nodiscard]] rt::Task<ValidateCodexAuthJsonResult> validateCodexAuthJson(::uos::types::CodexAuthState auth);
  [[nodiscard]] rt::Task<ValidateCodexAuthJsonResult> validateCodexAuthJson(::uos::types::CodexAuthState auth, ValidateCodexAuthJsonOptions options);
}
