#pragma once
// Port of src/models/codex-models-whitelist.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace uos::models::codex_models_whitelist {
  inline const rt::KvKey CODEX_MODELS_WHITELIST_KV_KEY{"uos_ai", "codex_models_whitelist"};
  struct CodexModelsWhitelist {
    std::vector<std::string> model_ids;
    double updated_at_ms;
    friend bool operator==(const CodexModelsWhitelist&, const CodexModelsWhitelist&) = default;
  };
  std::optional<CodexModelsWhitelist> normalizeCodexModelsWhitelist(const json::value& value);
  std::optional<CodexModelsWhitelist> normalizeCodexModelsWhitelist(const json::value* value);
  std::optional<CodexModelsWhitelist> normalizeCodexModelsWhitelist(const rt::KvValue& value);
  std::vector<std::string> normalizeWhitelistModelIds(const json::array& rawIds);
  std::vector<std::string> normalizeWhitelistModelIds(const std::vector<std::string>& rawIds);
  rt::Task<std::optional<CodexModelsWhitelist>> loadCodexModelsWhitelist(std::shared_ptr<rt::Kv> kv);
  rt::Task<bool> storeCodexModelsWhitelist(std::shared_ptr<rt::Kv> kv, std::vector<std::string> modelIds);
  json::array filterWhitelistedModelList(const json::array& models, const std::optional<CodexModelsWhitelist>& whitelist);
  std::vector<json::object> filterWhitelistedModelList(const std::vector<json::object>& models, const std::optional<CodexModelsWhitelist>& whitelist);
  json::array filterWhitelistedCatalogModels(const json::array& models, const std::optional<CodexModelsWhitelist>& whitelist);
  std::vector<json::object> filterWhitelistedCatalogModels(const std::vector<json::object>& models, const std::optional<CodexModelsWhitelist>& whitelist);
  template<class T> std::vector<T> filterWhitelistedModelMap(const std::vector<T>& entries, const std::optional<CodexModelsWhitelist>& whitelist) {
    if (!whitelist || whitelist->model_ids.empty()) return entries;
    std::vector<T> result;
    for (const auto& entry : entries) {
      bool allowed = false;
      for (const auto& id : whitelist->model_ids) if (id == entry.id) { allowed = true; break; }
      if (allowed) result.push_back(entry);
    }
    return result;
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexModelsWhitelist& value);
  CodexModelsWhitelist tag_invoke(const json::value_to_tag<CodexModelsWhitelist>&, const json::value& value);
}
