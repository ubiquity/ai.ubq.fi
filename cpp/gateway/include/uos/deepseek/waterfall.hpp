#pragma once
// Port of src/deepseek/waterfall.ts.
#include "uos/deepseek/flash_aliases.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
namespace uos::deepseek::waterfall {
  inline constexpr std::string_view DEEPSEEK_WATERFALL_MODEL_ID = "ubiquity/deepseek-flash";
  /** The pre-rename id remains accepted for clients and saved sessions. */
  inline constexpr std::string_view DEEPSEEK_WATERFALL_LEGACY_MODEL_ID = "ubiquity/deepseek-v4.1-flash";
  inline constexpr std::string_view DEEPSEEK_WATERFALL_DISPLAY_NAME = "Ubiquity DeepSeek Flash";
  inline constexpr std::string_view DEEPSEEK_WATERFALL_DESCRIPTION = "Latest DeepSeek Flash waterfall: official DeepSeek, OpenRouter, LithosAI, Surplus, and OpenLux. Each hop uses the provider's latest alias and falls back only on infrastructure or serving failures.";
  // The gateway's 800,000-token window triggers compaction at 720,000 tokens,
  // below the observed aggregate admission rejection near 792,000.
  inline constexpr double DEEPSEEK_WATERFALL_CONTEXT_WINDOW_TOKENS = 800'000;
  inline constexpr double DEEPSEEK_WATERFALL_EFFECTIVE_CONTEXT_WINDOW_PERCENT = 95;
  inline constexpr double DEEPSEEK_WATERFALL_CREATED = 1'791'244'800;
  inline constexpr std::array<std::string_view, 3> DEEPSEEK_WATERFALL_REASONING_LEVELS{"low", "high", "max"};
  inline constexpr std::string_view DEEPSEEK_WATERFALL_DEFAULT_REASONING_LEVEL = "high";
  using DeepSeekWaterfallProvider = flash_aliases::DeepSeekFlashProvider;
  inline constexpr std::string_view DEEPSEEK_WATERFALL_PAID_MODEL_ID = "deepseek-v4.1-flash";
  inline constexpr std::array<std::string_view, 5> DEEPSEEK_WATERFALL_ORDER{"lithos", "deepseek", "openrouter", "surplus", "openlux"};
  inline const rt::OrderedMap<std::string, std::string> DEEPSEEK_WATERFALL_PROVIDER_MODEL{
    {"lithos", "deepseek-ai/DeepSeek-V4.1-Flash"}, {"deepseek", "deepseek-flash"}, {"openrouter", "~deepseek/deepseek-flash-latest"},
    {"surplus", "deepseek-v4.1-flash"}, {"openlux", "deepseek-v4.1-flash"}};
  inline const rt::OrderedMap<std::string, std::vector<std::string>> DEEPSEEK_WATERFALL_PAID_PIN{
    {"surplus", {"surplus"}}, {"openlux", {"metered"}}};
  bool isDeepSeekWaterfallModel(std::string_view model);
  bool isDeepSeekWaterfallReasoningLevel(std::string_view effort);
  bool deepSeekWaterfallProviderConfigured(std::string_view provider);
  struct DeepSeekWaterfallPlanInput {
    std::function<bool(const DeepSeekWaterfallProvider&)> enabled;
    std::function<bool(const DeepSeekWaterfallProvider&)> configured;
  };
  std::vector<DeepSeekWaterfallProvider> deepSeekWaterfallPlan(const DeepSeekWaterfallPlanInput& input);
  bool isDeepSeekWaterfallRetryStatus(double status);
  std::optional<std::string> deepSeekWaterfallRequestedEffort(const json::object& rawRecord);
  std::string deepSeekWaterfallFailureReason(std::string_view provider, double failure);
  std::string deepSeekWaterfallFailureReason(std::string_view provider, std::string_view failure);
  std::string deepSeekWaterfallFailureReason(std::string_view provider, const std::variant<double, std::string>& failure);
  bool deepSeekWaterfallCatalogEnabled(const std::optional<provider::selection::ProviderSelection>& selection);
}
