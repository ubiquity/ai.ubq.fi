#include "uos/deepseek/flash_aliases.hpp"
#include "uos/rt/memory.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/lithos_models.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

namespace uos::deepseek::flash_aliases {
namespace {
  struct FlashCandidate { std::string id; std::string version; double dateOrder; std::string baseId; };
  const rt::Regex FLASH_ID_PATTERN{R"(^deepseek-v(\d+(?:\.\d+)?)-flash(?:-(\d{4}|\d{8}))?$)"};
  const rt::Regex VARIANT_MARKER_PATTERN{R"((?:^|[-:])(?:fast|ultra|chat|vision|exp|web|e2ee|free|batch)(?:$|[-:]))"};
  const rt::Regex LATEST_ALIAS_PATTERN{R"((?:^|[/-])deepseek(?:-v[\d.]+)?-flash-latest$)"};
  double dateOrder(const std::optional<std::string>& date) {
    if (!date || date->empty()) return 0;
    const auto year = rt::length(*date) == 8 ? rt::stringToNumber(rt::slice(*date, 0, 4)) : rt::Date(rt::dateNow()).getUTCFullYear();
    const auto month = rt::stringToNumber(rt::length(*date) == 8 ? rt::slice(*date, 4, 6) : rt::slice(*date, 0, 2));
    const auto day = rt::stringToNumber(rt::length(*date) == 8 ? rt::slice(*date, 6, 8) : rt::slice(*date, 2, 4));
    if (!rt::Number::isSafeInteger(year) || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    return year * 10000 + month * 100 + day;
  }
  std::optional<FlashCandidate> candidateFrom(const CatalogModel& model) {
    const auto trimmed = rt::trim(model.id);
    const auto lower = rt::toLowerCase(trimmed);
    if (lower.empty() || VARIANT_MARKER_PATTERN.test(lower)) return std::nullopt;
    const auto slug = rt::slice(lower, rt::lastIndexOf(lower, "/") + 1);
    const auto match = FLASH_ID_PATTERN.exec(slug);
    if (!match) return std::nullopt;
    const auto date = (*match)[2];
    const auto order = dateOrder(date);
    if (date && order == 0) return std::nullopt;
    const auto prefix = rt::slice(trimmed, 0, rt::lastIndexOf(trimmed, "/") + 1);
    static const rt::Regex suffix{R"(-\d{4,8}$)"};
    return FlashCandidate{trimmed, (*match)[1].value_or(""), order, prefix + rt::replace(slug, suffix, "")};
  }
  std::vector<double> versionParts(const std::string& version) {
    std::vector<double> parts;
    for (const auto& part : rt::split(version, ".")) { const auto number = rt::stringToNumber(part); parts.push_back(rt::truthy(number) ? number : 0); }
    return parts;
  }
  double compareCandidates(const FlashCandidate& left, const FlashCandidate& right) {
    const auto a = versionParts(left.version), b = versionParts(right.version);
    const auto size = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < size; ++i) { const auto difference = (i < a.size() ? a[i] : 0) - (i < b.size() ? b[i] : 0); if (difference != 0) return difference; }
    return left.dateOrder - right.dateOrder;
  }
  std::optional<std::string> latestAliasFor(const std::vector<CatalogModel>& models) {
    std::vector<std::string> aliases;
    for (const auto& model : models) { const auto id = rt::trim(model.id); if (rt::startsWith(id, "~") && LATEST_ALIAS_PATTERN.test(rt::toLowerCase(id))) aliases.push_back(id); }
    static const rt::Regex unversioned{R"(^~\w+/deepseek-flash-latest$)", "i"};
    rt::sort(aliases, [](const auto& a, const auto& b) { const bool first = unversioned.test(a), second = unversioned.test(b); return first != second ? first ? -1 : 1 : rt::localeCompare(a, b); });
    return aliases.empty() ? std::nullopt : std::optional(aliases.front());
  }
  std::optional<std::string> providerAliasFrom(const std::vector<CatalogModel>& models) {
    if (auto latest = latestAliasFor(models)) return latest;
    std::optional<FlashCandidate> newest;
    for (const auto& model : models) if (auto candidate = candidateFrom(model); candidate && (!newest || compareCandidates(*candidate, *newest) > 0)) newest = candidate;
    if (!newest) return std::nullopt;
    std::optional<std::string> baseAlias;
    for (const auto& model : models) if (auto candidate = candidateFrom(model); candidate && candidate->version == newest->version && candidate->dateOrder == 0) baseAlias = candidate->id;
    return baseAlias.value_or(newest->id);
  }
  template <class Rows> std::vector<CatalogModel> catalogModels(const Rows& rows) { std::vector<CatalogModel> models; for (const auto& row : rows) models.push_back({row.id}); return models; }
  std::optional<std::pair<std::string, DeepSeekFlashAliases>> currentAliasesCache;
}
/**
 * Resolve every provider's latest Flash alias. Provider catalogues may be absent
 * or partial; the cold-start alias remains in that case. No provider is removed.
 */
DeepSeekFlashAliases resolveDeepSeekFlashAliases(const DeepSeekFlashCatalogInput& input) {
  auto lithos = input.lithos;
  if (!lithos) {
    if (const auto snapshot = provider::lithos_models::lithosModelsSnapshot()) lithos = catalogModels(snapshot->models);
    else { std::vector<CatalogModel> fallback; for (const auto id : provider::lithos::LITHOS_MODEL_IDS) fallback.push_back({std::string(id)}); lithos = std::move(fallback); }
  }
  const auto& fallback = DEEPSEEK_FLASH_FALLBACK_ALIASES.models;
  return {{providerAliasFrom(*lithos).value_or(fallback.lithos), "deepseek-flash", providerAliasFrom(input.openrouter.value_or(std::vector<CatalogModel>{})).value_or(fallback.openrouter),
    providerAliasFrom(input.surplus.value_or(std::vector<CatalogModel>{})).value_or(fallback.surplus), providerAliasFrom(input.openlux.value_or(std::vector<CatalogModel>{})).value_or(fallback.openlux)}};
}
/** The current aliases from cached catalogues. Reads never wait on a refresh. */
DeepSeekFlashAliases currentDeepSeekFlashAliases() {
  const auto lithos = provider::lithos_models::lithosModelsSnapshot();
  const auto openrouter = models::openrouter_models::openRouterModelsSnapshot();
  const auto surplus = provider::surplus::surplusModelsSnapshot();
  const auto openlux = provider::metered::meteredModelsSnapshot();
  const auto signature = rt::numberToString(lithos ? lithos->updated_at_ms : 0) + ":" + rt::numberToString(openrouter ? openrouter->updated_at_ms : 0) + ":" + rt::numberToString(surplus ? surplus->updated_at_ms : 0) + ":" + rt::numberToString(openlux ? openlux->updated_at_ms : 0);
  if (currentAliasesCache && currentAliasesCache->first == signature) return currentAliasesCache->second;
  DeepSeekFlashCatalogInput input;
  if (lithos) input.lithos = catalogModels(lithos->models);
  if (openrouter) input.openrouter = catalogModels(openrouter->models);
  if (surplus) input.surplus = catalogModels(surplus->models);
  if (openlux) input.openlux = catalogModels(openlux->models);
  auto aliases = resolveDeepSeekFlashAliases(input);
  currentAliasesCache = std::make_pair(signature, aliases);
  const auto& rows = currentAliasesCache->second.models;
  rt::memory::setCache(rt::memory::Cache::deepseek_flash_aliases, 5,
                       sizeof(*currentAliasesCache) + rt::memory::bytes(signature) + rt::memory::bytes(rows.lithos) +
                       rt::memory::bytes(rows.deepseek) + rt::memory::bytes(rows.openrouter) +
                       rt::memory::bytes(rows.surplus) + rt::memory::bytes(rows.openlux));
  return aliases;
}
/** Start catalogue refreshes without waiting. */
void warmDeepSeekFlashCatalogs() {
  rt::spawn(models::openrouter_models::fetchOpenRouterModels(), rt::ignoreRejection);
  if (provider::lithos::readLithosApiKey()) rt::spawn(provider::lithos_models::fetchLithosModels(), rt::ignoreRejection);
  if (provider::surplus::readSurplusApiKey()) rt::spawn(provider::surplus::fetchSurplusModels(), rt::ignoreRejection);
  if (provider::metered::readMeteredApiKey()) rt::spawn(provider::metered::fetchMeteredModels(), rt::ignoreRejection);
}
/** Whether an id is one of the paid catalogue's DeepSeek Flash aliases. */
bool isDeepSeekFlashPaidModel(const std::string& model) {
  const auto trimmed = rt::toLowerCase(rt::trim(model));
  const auto slug = rt::slice(trimmed, rt::lastIndexOf(trimmed, "/") + 1);
  static const rt::Regex pattern{R"(^deepseek-v\d+(?:\.\d+)?-flash(?:-\d{4}|\d{8})?$)"};
  return pattern.test(slug);
}
}
