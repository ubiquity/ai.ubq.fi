#pragma once
// Port of src/catalog/models.ts.
#include "uos/models/codex_models.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include <optional>
#include <string>
#include <vector>

namespace uos::catalog::models {
  json::array claudeCodeCodexModels(const std::vector<provider::claude_code_models::ClaudeCodeModel>& models);
  json::array withClaudeCodeModels(json::array models, const std::vector<provider::claude_code_models::ClaudeCodeModel>& claude);
  rt::Task<void> maybeUpdateNormalizedSnapshot(rt::Kv& kv, std::string version, std::string authGeneration, json::object parsed, double updatedAtMs);
  bool etagMatches(const std::optional<std::string>& requestValue, const std::optional<std::string>& etag);
  rt::OrderedMap<std::string, json::object> codexSnapshotRecords(const std::optional<::uos::models::codex_models::CodexModelsSnapshot>& snapshot);
  json::object meteredCodexModelRecord(const json::object& model, const json::object* codexRecord = nullptr);
  json::array uniqueResponsesModels(const json::array& models);
  template<class T> std::vector<T> uniqueResponsesModels(const std::vector<T>& models) {
    rt::OrderedSet<std::string> seen;
    std::vector<T> result;
    for (const auto& model : models) {
      bool responses = false;
      for (const auto& endpoint : model.supported_endpoint_types) if (endpoint == "openai-response") responses = true;
      if (!responses || seen.has(model.id)) continue;
      seen.add(model.id);
      result.push_back(model);
    }
    return result;
  }
  json::array deepSeekOfficialCodexModels();
  json::array withDeepSeekOfficialModels(json::array models);
  json::object buildDeepSeekWaterfallCodexRecord();
  json::array withDeepSeekWaterfallModel(json::array models);
  json::array lithosCodexModels();
  json::array withLithosModels(json::array models);
  json::array cerebrasCodexModels();
  json::array withCerebrasModels(json::array models);
  json::array geminiCodexModels();
  json::array withGeminiModels(json::array models);
  json::array openRouterCodexModels();
  json::array withOpenRouterModels(json::array models);
  rt::OrderedSet<std::string> catalogModelIds(const json::array& models);
}
