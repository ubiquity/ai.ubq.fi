#pragma once
// Port of src/models/catalog.ts.
#include "uos/authority/contracts.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"
#include <optional>
#include <string>
#include <vector>
namespace uos::models::catalog {
  struct PublicModelProvider {
    std::string id;
    std::string owned_by;
    std::vector<std::string> supported_endpoints;
    friend bool operator==(const PublicModelProvider&, const PublicModelProvider&) = default;
  };
  struct PublicModelCatalogEntry {
    std::string id;
    std::vector<PublicModelProvider> providers;
    std::optional<double> created{};
    std::optional<double> context_window_tokens{};
    std::optional<double> max_context_window_tokens{};
    std::optional<double> auto_compact_token_limit_tokens{};
    std::optional<double> effective_context_window_percent{};
    std::optional<std::vector<std::string>> supported_reasoning_levels{};
    std::optional<std::string> default_reasoning_effort{};
    std::optional<std::string> context_source{};
    std::optional<std::string> reasoning_source{};
    friend bool operator==(const PublicModelCatalogEntry&, const PublicModelCatalogEntry&) = default;
  };
  using ModelCatalogSourceId = std::string;
  struct ModelCatalogSource {
    std::string status;
    double count;
    std::optional<double> updated_at_ms;
    std::optional<bool> configured{};
    std::optional<bool> disabled{};
    std::optional<bool> stale{};
    friend bool operator==(const ModelCatalogSource&, const ModelCatalogSource&) = default;
  };
  struct ModelCatalogSnapshot {
    std::vector<PublicModelCatalogEntry> models;
    authority::contracts::Record<ModelCatalogSource> sources;
    friend bool operator==(const ModelCatalogSnapshot&, const ModelCatalogSnapshot&) = default;
  };
  rt::Task<rt::Response> handleModels(std::optional<rt::Request> req = std::nullopt);
  rt::Task<ModelCatalogSnapshot> buildModelCatalogSnapshot();
  authority::contracts::Record<ModelCatalogSource> selectedCatalogSources(const authority::contracts::Record<ModelCatalogSource>& sources, const std::optional<provider::selection::ProviderSelection>& selection);
  rt::Task<rt::Response> handlePublicModelCatalog();
  rt::Task<rt::Response> handleModelCapabilities();
  void tag_invoke(const json::value_from_tag&, json::value& out, const PublicModelProvider& value);
  PublicModelProvider tag_invoke(const json::value_to_tag<PublicModelProvider>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PublicModelCatalogEntry& value);
  PublicModelCatalogEntry tag_invoke(const json::value_to_tag<PublicModelCatalogEntry>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ModelCatalogSource& value);
  ModelCatalogSource tag_invoke(const json::value_to_tag<ModelCatalogSource>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ModelCatalogSnapshot& value);
  ModelCatalogSnapshot tag_invoke(const json::value_to_tag<ModelCatalogSnapshot>&, const json::value& value);
}
