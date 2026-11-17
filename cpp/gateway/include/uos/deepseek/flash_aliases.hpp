#pragma once
// Port of src/deepseek/flash-aliases.ts.
#include "uos/rt/prelude.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace uos::deepseek::flash_aliases {
  using DeepSeekFlashProvider = std::string;
  struct DeepSeekFlashAliases {
    struct Models { std::string lithos; std::string deepseek; std::string openrouter; std::string surplus; std::string openlux; };
    Models models;
  };
  inline const DeepSeekFlashAliases DEEPSEEK_FLASH_FALLBACK_ALIASES{{"deepseek-ai/DeepSeek-V4.1-Flash", "deepseek-flash", "~deepseek/deepseek-flash-latest", "deepseek-v4.1-flash", "deepseek-v4.1-flash"}};
  struct CatalogModel { std::string id; };
  struct DeepSeekFlashCatalogInput {
    std::optional<std::vector<CatalogModel>> lithos{};
    std::optional<std::vector<CatalogModel>> openrouter{};
    std::optional<std::vector<CatalogModel>> surplus{};
    std::optional<std::vector<CatalogModel>> openlux{};
  };
  DeepSeekFlashAliases resolveDeepSeekFlashAliases(const DeepSeekFlashCatalogInput& input = {});
  DeepSeekFlashAliases currentDeepSeekFlashAliases();
  void warmDeepSeekFlashCatalogs();
  bool isDeepSeekFlashPaidModel(const std::string& model);
}
