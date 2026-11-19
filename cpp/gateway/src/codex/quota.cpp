#include "uos/codex/quota.hpp"

#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <array>
#include <string>
#include <utility>

namespace uos::codex::quota {
  namespace {
    constexpr std::string_view DEFAULT_CODEX_PREFIX = "x-codex";

    constexpr std::array<std::string_view, 9> RATE_LIMIT_FAMILY_SUFFIXES = {
      "-limit-name",
      "-primary-used-percent",
      "-primary-window-minutes",
      "-primary-reset-at",
      "-primary-reset-after-seconds",
      "-secondary-used-percent",
      "-secondary-window-minutes",
      "-secondary-reset-at",
      "-secondary-reset-after-seconds",
    };

    constexpr std::array<std::string_view, 5> SHARED_CODEX_QUOTA_HEADERS = {
      "x-codex-credits-has-credits",
      "x-codex-credits-unlimited",
      "x-codex-credits-balance",
      "x-codex-primary-over-secondary-limit-percent",
      "x-codex-rate-limit-reached-type",
    };

    std::string formatPercent(double value) {
      const double rounded = rt::Math::round(rt::Math::min(100, rt::Math::max(0, value)) * 1'000'000) / 1'000'000;
      return rt::numberToString(rounded);
    }
  }

  rt::Headers buildCodexQuotaHeaders(const rt::Headers& input, const std::optional<ClientQuotaSnapshot>& snapshot) {
    rt::Headers headers(input);

    // Codex discovers named limit families from any x-*-primary-used-percent header. Version 0.144.6
    // parses every discovered family but persists only one response-derived snapshot, so forwarding
    // multiple families makes whichever name sorts last overwrite the others. Remove every parseable
    // family before publishing the one client-specific capacity source this gateway can measure.
    for (const std::string_view name : headers.keys()) {
      if (!name.starts_with("x-")) continue;
      for (std::string_view suffix : RATE_LIMIT_FAMILY_SUFFIXES) {
        if (name.ends_with(suffix)) {
          headers.delete_(name);
          break;
        }
      }
    }
    for (std::string_view name : SHARED_CODEX_QUOTA_HEADERS) headers.delete_(name);

    if (snapshot && snapshot->used_percent) {
      const std::string usedPercent = formatPercent(*snapshot->used_percent);
      headers.set(std::string(DEFAULT_CODEX_PREFIX) + "-limit-name", METERED_CODEX_LIMIT_NAME);
      headers.set(std::string(DEFAULT_CODEX_PREFIX) + "-primary-used-percent", usedPercent);
    }
    return headers;
  }

  rt::Response withCodexQuotaHeaders(const rt::Response& response, const std::optional<ClientQuotaSnapshot>& snapshot) {
    rt::BodyInit body = rt::bodyInit(response.body());
    rt::ResponseInit init;
    init.status = static_cast<double>(response.status());
    init.statusText = response.statusText();
    init.headers = buildCodexQuotaHeaders(response.headers(), snapshot);
    return rt::Response(std::move(body), std::move(init));
  }
}
