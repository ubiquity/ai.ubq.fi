#include "uos/provider/cerebras_rate_limits.hpp"

namespace uos::provider::cerebras_rate_limits {
  const std::array<std::string_view, 12> CEREBRAS_RATE_LIMIT_HEADERS = std::to_array<std::string_view>({
    "x-ratelimit-limit-requests-minute",
    "x-ratelimit-remaining-requests-minute",
    "x-ratelimit-reset-requests-minute",
    "x-ratelimit-limit-tokens-minute",
    "x-ratelimit-remaining-tokens-minute",
    "x-ratelimit-reset-tokens-minute",
    "x-ratelimit-limit-requests-day",
    "x-ratelimit-remaining-requests-day",
    "x-ratelimit-reset-requests-day",
    "x-ratelimit-limit-tokens-day",
    "x-ratelimit-remaining-tokens-day",
    "x-ratelimit-reset-tokens-day",
  });
}
