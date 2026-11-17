// Differential case for src/provider/cerebras-rate-limits.ts (no bun test imports
// it): the export as Bun prints it (JSON.stringify).
#include "uos/provider/cerebras_rate_limits.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

TEST("differential: CEREBRAS_RATE_LIMIT_HEADERS matches Bun") {
  uos::json::array out;
  for (const auto name : uos::provider::cerebras_rate_limits::CEREBRAS_RATE_LIMIT_HEADERS) out.emplace_back(uos::json::string(name));
  EXPECT_EQ(uos::rt::jsonStringify(uos::json::value(std::move(out))),
            R"(["x-ratelimit-limit-requests-minute","x-ratelimit-remaining-requests-minute","x-ratelimit-reset-requests-minute","x-ratelimit-limit-tokens-minute","x-ratelimit-remaining-tokens-minute","x-ratelimit-reset-tokens-minute","x-ratelimit-limit-requests-day","x-ratelimit-remaining-requests-day","x-ratelimit-reset-requests-day","x-ratelimit-limit-tokens-day","x-ratelimit-remaining-tokens-day","x-ratelimit-reset-tokens-day"])");
}
