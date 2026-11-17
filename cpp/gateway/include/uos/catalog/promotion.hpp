#pragma once
// Port of src/catalog/promotion.ts.
#include "uos/models/codex_models.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include <memory>
#include <optional>
#include <string>
namespace uos::catalog::promotion {
  struct PromptCacheScopePromotionLease { rt::KvKey key; std::string owner; };
  struct PromptCacheScopePromotionResult { std::string status; std::optional<std::string> reason{}; };
  struct PromptCacheScopePromotionInput {
    std::string model;
    models::codex_models::PromptCacheScope scope;
    PromptCacheScopePromotionLease lease;
    std::string authPoolVersionstamp;
    std::string catalogVersionstamp;
    std::string runtimeVersionstamp;
  };
  rt::Task<PromptCacheScopePromotionResult> promoteCodexPromptCacheScope(std::shared_ptr<rt::Kv> kv, PromptCacheScopePromotionInput input);
}
