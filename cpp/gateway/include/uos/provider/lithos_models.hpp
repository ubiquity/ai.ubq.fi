#pragma once
// Port of src/provider/lithos-models.ts.
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/prelude.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace uos::provider::lithos_models {
  struct LithosModel { std::string id; double created; friend bool operator==(const LithosModel&, const LithosModel&) = default; };
  struct LithosModelsSnapshot { std::vector<LithosModel> models; double updated_at_ms; friend bool operator==(const LithosModelsSnapshot&, const LithosModelsSnapshot&) = default; };
  inline constexpr std::string_view LITHOS_MODELS_URL = "https://api.lithosai.cloud/v1/models";
  inline constexpr double LITHOS_MODELS_CACHE_TTL_MS = 5 * 60'000;
  struct FetchLithosModelsOptions { rt::Fetch fetcher{}; std::optional<rt::AbortSignal> signal{}; std::optional<bool> force{}; std::optional<bool> cachedOnly{}; };
  rt::Task<std::optional<LithosModelsSnapshot>> fetchLithosModels();
  rt::Task<std::optional<LithosModelsSnapshot>> fetchLithosModels(FetchLithosModelsOptions options);
  std::optional<LithosModelsSnapshot> lithosModelsSnapshot();
  void tag_invoke(const json::value_from_tag&, json::value& out, const LithosModel& value);
  LithosModel tag_invoke(const json::value_to_tag<LithosModel>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const LithosModelsSnapshot& value);
  LithosModelsSnapshot tag_invoke(const json::value_to_tag<LithosModelsSnapshot>&, const json::value& value);
}
