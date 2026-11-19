// Port of tests/recent-model-context.test.ts.
#include "uos/recent_model_context.hpp"

#include "uos/models/codex_models.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <optional>

namespace {
  using namespace uos::recent_model_context;
  using uos::models::codex_models::CodexModelsSnapshot;
  using uos::models::codex_models::normalizeCodexModelsPayload;
  namespace json = uos::json;
  namespace rt = uos::rt;
}

TEST("auto-compaction uses the earlier 90 percent or 50k-reserve boundary") {
  EXPECT_EQ(deriveAutoCompactTokenLimit(1'000'000), 900'000);
  EXPECT_EQ(deriveAutoCompactTokenLimit(400'000), 350'000);
  EXPECT_EQ(deriveAutoCompactTokenLimit(262'144), 212'144);
  EXPECT_EQ(deriveAutoCompactTokenLimit(204'800), 154'800);
  EXPECT_THROWS(deriveAutoCompactTokenLimit(50'000), rt::RangeError);
}

TEST("a declared earlier threshold is preserved and a later threshold is clamped") {
  EXPECT_EQ(resolvedAutoCompactTokenLimit(272'000, 200'000), 200'000);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(1'050'000, 990'000), 945'000);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(272'000, std::nullopt), 222'000);
  // A threshold larger than the window it belongs to is not a usable limit.
  EXPECT_EQ(resolvedAutoCompactTokenLimit(272'000, 1'000'000), 222'000);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(272'000, std::nullopt), 222'000);
}

TEST("Codex snapshot normalization preserves native effective context percentage") {
  const std::optional<CodexModelsSnapshot> snapshot = normalizeCodexModelsPayload(rt::jsonParse(R"({
    "models": [
      {
        "slug": "gpt-5.6-terra",
        "context_window": 272000,
        "max_context_window": 1000000,
        "auto_compact_token_limit": null,
        "effective_context_window_percent": 91
      }
    ]
  })"));
  const json::value* first = snapshot ? rt::get(snapshot->at("models").as_array(), 0) : nullptr;
  EXPECT_EQ(first ? rt::getNumber(*first, "effective_context_window_percent") : std::nullopt, std::optional<double>(91));
}
