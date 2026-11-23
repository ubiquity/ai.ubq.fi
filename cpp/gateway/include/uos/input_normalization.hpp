#pragma once
// Port of src/input-normalization.ts.
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>
namespace uos::input_normalization {
  template<class T> struct NormalizationResultOk {
    static constexpr bool ok = true;
    std::conditional_t<std::is_void_v<T>, std::monostate, T> value;
  };
  struct NormalizationResultNotOk {
    static constexpr bool ok = false;
    std::string message;
    std::string param;
    friend bool operator==(const NormalizationResultNotOk&, const NormalizationResultNotOk&) = default;
  };
  template<class T> using NormalizationResult = std::variant<NormalizationResultOk<T>, NormalizationResultNotOk>;
  NormalizationResult<void> validatePromptCacheControls(const json::object& rawRecord);
  std::string normalizeModelForCodex(std::string_view model);
  std::optional<json::object> normalizeModelList(const json::value& payload);
  std::optional<json::object> normalizeModelList(const json::value* payload);
  inline const rt::OrderedMap<std::string, std::string> CEREBRAS_MODEL_DISPLAY_NAMES{{"gpt-oss-120b", "GPT-OSS 120B"}, {"qwen-3.8-27b", "Qwen 3.8 27B"}};
  std::optional<json::object> configuredCerebrasModelCapabilities();
  std::optional<json::object> configuredCerebrasModelCapabilities(const std::string& model);
  json::array withConfiguredCerebrasModel(const json::array& models, bool enabled);
  json::array withConfiguredGeminiModels(const json::array& models, bool enabled);
  json::array withConfiguredGeminiCapabilities(const json::array& data, bool enabled);
  json::array withConfiguredDeepSeekModels(const json::array& models, bool enabled);
  json::array withConfiguredDeepSeekCapabilities(const json::array& data, bool enabled);
  json::array withConfiguredLithosModels(const json::array& models, bool enabled);
  json::array withConfiguredDeepSeekWaterfallModels(const json::array& models, bool enabled);
  json::array withConfiguredDeepSeekWaterfallCapabilities(const json::array& data, bool enabled);
  inline constexpr std::string_view OPENROUTER_SERVED_MODEL_ID = "typesafe/jev-latest";
  json::array configuredOpenRouterModels();
  json::array configuredOpenRouterModelCapabilities();
  json::array withConfiguredLithosCapabilities(const json::array& data, bool enabled);
  std::optional<json::object> normalizeModelCapabilitiesEntry(const json::value& value);
  std::optional<json::object> normalizeModelCapabilitiesEntry(const json::value* value);
  NormalizationResult<::uos::types::MessageContentItem> normalizeResponseContentItem(const json::value& value, const std::string& param, const std::string& role);
  NormalizationResult<::uos::types::MessageContentItem> normalizeResponseContentItem(const json::value* value, const std::string& param, const std::string& role);
  NormalizationResult<::uos::types::ResponseMessageItem> normalizeResponseMessageItem(const json::value& value, const std::string& param);
  NormalizationResult<::uos::types::ResponseMessageItem> normalizeResponseMessageItem(const json::value* value, const std::string& param);
  NormalizationResult<::uos::types::ResponseInputItem> normalizeFunctionCallOutputItem(const json::object& value, const std::string& param);
  struct NormalizedChatMessage {
    std::optional<std::string> instruction;
    std::optional<std::vector<::uos::types::MessageContentItem>> instructionContent;
    std::vector<::uos::types::ResponseInputItem> input;
  };
  NormalizationResult<NormalizedChatMessage> normalizeChatMessage(const json::value& value, double index);
  NormalizationResult<NormalizedChatMessage> normalizeChatMessage(const json::value* value, double index);
}
