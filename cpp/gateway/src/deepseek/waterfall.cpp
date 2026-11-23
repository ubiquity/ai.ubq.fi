/** The pre-rename id remains accepted for clients and saved sessions. */

/** Stable creation instant for catalogue rows (2026-10-06T00:00:00Z). */

/**
 * The paid catalogue alias used by the Surplus and OpenLux hops. Paid hops
 * re-enter the ordinary Responses pipeline under this provider-owned alias so
 * admission, reservation and ledger settlement stay the paid path's.
 */

/**
 * The cost-first economy order (2026-10-07): LithosAI Base and the DeepSeek
 * off-peak rate are half of OpenRouter's per-token price for the same weights,
 * so the cheaper direct routes lead. Surplus and OpenLux are the two paid hops
 * and are each pinned to their own tier by {@link DEEPSEEK_WATERFALL_PAID_PIN}.
 */

/**
 * The provider-owned latest alias each hop is dispatched with. These are not
 * gateway release pins: no snapshot date or gateway-side version is encoded.
 */

/**
 * The paid tier each paid hop is pinned to. Both paid hops re-enter the
 * ordinary paid pipeline under {@link DEEPSEEK_WATERFALL_PAID_MODEL_ID}, and
 * without a pin that pipeline would pick the fixed Surplus -> Metered cost
 * order for either hop — so the Surplus hop could silently advance to metered
 * and the OpenLux hop might never be tried. The routing layer names OpenLux
 * "metered", hence the `metered` entry for the openlux hop.
 */

/** Whether the route has a credential for one hop. */

/** The hop order after removing providers the operator switched off or that have no credential. */

/**
 * A hop status worth trying the next provider for: upstream transport, quota or
 * server failures that are not the client's fault. A request-validation 4xx is
 * the client's answer and must be returned as-is, and a valid completion — even
 * a wrong one — is a model result, never a serving failure.
 */

/** Requested reasoning effort from a Responses record, or null when omitted. */

/** The exact fallback reason recorded for one failed hop. */

/**
 * Whether the synthetic route may be advertised and dispatched: the operator
 * leaves the gateway identity enabled (absent or empty selection, or `ubiquity`
 * checked) and at least one hop has a credential. An active selection without
 * `ubiquity` switches the whole route off.
 */
#include "uos/deepseek/waterfall.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/provider/presentation.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
namespace uos::deepseek::waterfall {
  bool isDeepSeekWaterfallModel(std::string_view model) {
    const auto normalized = rt::toLowerCase(rt::trim(model));
    return normalized == DEEPSEEK_WATERFALL_MODEL_ID || normalized == DEEPSEEK_WATERFALL_LEGACY_MODEL_ID;
  }
  bool isDeepSeekWaterfallReasoningLevel(std::string_view effort) {
    return std::find(DEEPSEEK_WATERFALL_REASONING_LEVELS.begin(), DEEPSEEK_WATERFALL_REASONING_LEVELS.end(), effort) != DEEPSEEK_WATERFALL_REASONING_LEVELS.end();
  }
  bool deepSeekWaterfallProviderConfigured(std::string_view provider) {
    if (provider == "lithos") return provider::lithos::readLithosApiKey().has_value();
    if (provider == "deepseek") return index::readDeepSeekApiKey().has_value();
    if (provider == "openrouter") return provider::openrouter::readOpenRouterApiKey().has_value();
    if (provider == "openlux") return provider::metered::readMeteredApiKey().has_value();
    return provider::surplus::readSurplusApiKey().has_value();
  }
  std::vector<DeepSeekWaterfallProvider> deepSeekWaterfallPlan(const DeepSeekWaterfallPlanInput& input) {
    std::vector<DeepSeekWaterfallProvider> result;
    for (const auto provider : DEEPSEEK_WATERFALL_ORDER) {
      const std::string name(provider);
      if (input.enabled(name) && input.configured(name)) result.push_back(name);
    }
    return result;
  }
  bool isDeepSeekWaterfallRetryStatus(double status) { return status >= 500 || status == 429 || status == 402 || status == 403; }
  std::optional<std::string> deepSeekWaterfallRequestedEffort(const json::object& rawRecord) {
    const auto* reasoning = rt::getObject(rawRecord, "reasoning");
    if (reasoning) {
      auto effort = rt::getString(*reasoning, "effort");
      if (effort && !rt::trim(*effort).empty()) return rt::trim(*effort);
    }
    auto legacy = rt::getString(rawRecord, "reasoning_effort");
    if (legacy && !rt::trim(*legacy).empty()) return rt::trim(*legacy);
    return std::nullopt;
  }
  std::string deepSeekWaterfallFailureReason(std::string_view provider, double failure) {
    return "deepseek_waterfall:" + std::string(provider) + ":" + rt::numberToString(failure);
  }
  std::string deepSeekWaterfallFailureReason(std::string_view provider, std::string_view failure) {
    return "deepseek_waterfall:" + std::string(provider) + ":" + (failure == "transport" ? "transport_failure" : failure == "stream" ? "stream_failure" : "undefined");
  }
  std::string deepSeekWaterfallFailureReason(std::string_view provider, const std::variant<double, std::string>& failure) {
    if (const auto* status = std::get_if<double>(&failure)) return deepSeekWaterfallFailureReason(provider, *status);
    return deepSeekWaterfallFailureReason(provider, std::string_view(std::get<std::string>(failure)));
  }
  bool deepSeekWaterfallCatalogEnabled(const std::optional<provider::selection::ProviderSelection>& selection) {
    if (!provider::selection::isProviderEnabled(provider::presentation::GATEWAY_PROVIDER_ID, selection)) return false;
    return std::any_of(DEEPSEEK_WATERFALL_ORDER.begin(), DEEPSEEK_WATERFALL_ORDER.end(), deepSeekWaterfallProviderConfigured);
  }
}
