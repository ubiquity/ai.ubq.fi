#pragma once
// Port of src/codex/index.ts.
// The Codex subscription transport entry point and the public Codex surface it re-exports.
#include "uos/codex/auth.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/codex/experiment.hpp"
#include "uos/models/codex_models.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

namespace uos::codex::index {
  // `body: unknown`; `options: FetchCodexResponsesOptions = {}` is the overload without it.
  [[nodiscard]] rt::Task<rt::Response> fetchCodexResponses(json::value body);
  [[nodiscard]] rt::Task<rt::Response> fetchCodexResponses(json::value body, dispatch::FetchCodexResponsesOptions options);

  // export { ... } from "./auth.ts"
  using auth::CODEX_AUTH_POOL_KV_KEY;
  using auth::CODEX_AUTH_REAUTH_MESSAGE;
  using auth::CODEX_AUTH_REAUTH_WARNING;
  using auth::CODEX_MODELS_KV_KEY;
  using auth::CODEX_QUOTA_BLOCKED_ERROR_CODE;
  using auth::CODEX_SUBSCRIPTIONS_UNAVAILABLE_ERROR_CODE;
  using auth::CODEX_UPSTREAM_DEGRADED_ERROR_CODE;
  using auth::CodexError;
  // C++ addition next to CodexError: its `instanceof` over a caught rt::Reason.
  using auth::codexErrorOf;
  using auth::cacheCodexAuthPool;
  using auth::getCodexAccountEmail;
  using auth::getCodexResponseAccountCohortId;
  using auth::getCodexResponseActiveTelemetry;
  using auth::getCodexResponseSlot;
  using auth::getCodexRoutingError;
  using auth::getCodexRoutingProbe;
  using auth::getJwtExpMs;
  using auth::markCodexResponseCompleted;
  using auth::markCodexResponseUpstreamError;
  using auth::parseCodexAuthFromAuthJson;
  using auth::parseCodexAuthPool;
  using auth::releaseCodexResponseProbe;
  using auth::resetCodexAuthCacheForTest;
  using auth::upsertCodexAuthAccount;

  // export { ... } from "./experiment.ts"
  using experiment::CodexCacheScopeExperimentError;
  using experiment::beginCodexCacheScopeExperiment;
  using experiment::fetchCodexResponsesForCacheScopeExperiment;
  using experiment::getCodexCapacityAccounts;
  using experiment::refreshCodexCacheScopeExperimentSlot;
  // export type { CodexCacheScopeExperimentSession, CodexCapacityAccount } from "./experiment.ts"
  using experiment::CodexCacheScopeExperimentSession;
  using experiment::CodexCapacityAccount;
  // export { orderCodexAuthAccounts } from "./dispatch.ts"
  using dispatch::orderCodexAuthAccounts;
  // export { ... } from "../models/codex-models-fetch.ts"
  using ::uos::models::codex_models_fetch::buildCodexRequest;
  using ::uos::models::codex_models_fetch::fetchCodexModels;
  using ::uos::models::codex_models_fetch::loadCodexModelsSnapshot;
  using ::uos::models::codex_models_fetch::loadFullCodexModelsSnapshot;
  using ::uos::models::codex_models_fetch::preserveCodexDefaultModel;
  using ::uos::models::codex_models_fetch::storeCodexModelsSnapshot;
  using ::uos::models::codex_models_fetch::validateCodexAuthJson;
  // export type { CodexModelsSnapshot } from "../models/codex-models.ts"
  using ::uos::models::codex_models::CodexModelsSnapshot;
  // export { getCodexModelsSnapshotDefaultModel } from "../models/codex-models.ts"
  using ::uos::models::codex_models::getCodexModelsSnapshotDefaultModel;
}
