// Differential cases for src/recent-model-context.ts beyond tests/recent-model-context.test.ts:
// the expected values were recorded from Bun 1.4.0 running the TypeScript module.
#include "uos/recent_model_context.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

namespace {
  using namespace uos::recent_model_context;
  namespace rt = uos::rt;
  const char* const RESERVED_BUDGET = "contextWindowTokens must be a safe integer greater than the reserved token budget";
}

TEST("deriveAutoCompactTokenLimit at and around its boundaries") {
  EXPECT_EQ(deriveAutoCompactTokenLimit(50'001), 1);
  EXPECT_EQ(deriveAutoCompactTokenLimit(rt::Number::MAX_SAFE_INTEGER), 7656119366529842.0);
  EXPECT_THROWS_MESSAGE(deriveAutoCompactTokenLimit(50'000), rt::RangeError, RESERVED_BUDGET);
  EXPECT_THROWS_MESSAGE(deriveAutoCompactTokenLimit(100'000.5), rt::RangeError, RESERVED_BUDGET);
  EXPECT_THROWS_MESSAGE(deriveAutoCompactTokenLimit(9007199254740992.0), rt::RangeError, RESERVED_BUDGET);
  EXPECT_THROWS_MESSAGE(deriveAutoCompactTokenLimit(rt::Number::NaN), rt::RangeError, RESERVED_BUDGET);
}

TEST("resolvedAutoCompactTokenLimit for small, fractional, invalid and configured windows") {
  EXPECT_EQ(resolvedAutoCompactTokenLimit(40'000, std::nullopt), 34'000);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(1, std::nullopt), 1);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(2, std::nullopt), 1);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(0, std::nullopt), 1);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(-10, std::nullopt), 1);
  EXPECT(rt::Number::isNaN(resolvedAutoCompactTokenLimit(rt::Number::NaN, std::nullopt)));
  EXPECT_THROWS_MESSAGE(resolvedAutoCompactTokenLimit(60'000.5, std::nullopt), rt::RangeError, RESERVED_BUDGET);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(1'000.5, std::nullopt), 850);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(272'000, 1'000.5), 222'000);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(272'000, 0), 222'000);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(272'000, 272'000), 272'000);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(40'000, 30'000), 30'000);
  EXPECT_EQ(resolvedAutoCompactTokenLimit(40'000, 50'000), 34'000);
  EXPECT_THROWS_MESSAGE(resolvedAutoCompactTokenLimit(rt::Number::POSITIVE_INFINITY, std::nullopt), rt::RangeError, RESERVED_BUDGET);
  EXPECT_THROWS_MESSAGE(resolvedAutoCompactTokenLimit(1152921504606846976.0, std::nullopt), rt::RangeError, RESERVED_BUDGET);
}

TEST("the exported constants") {
  EXPECT_EQ(CONTEXT_COMPACTION_TRIGGER_RATIO, 0.85);
  EXPECT_EQ(CONTEXT_COMPACTION_RESERVED_TOKENS, 50'000);
  EXPECT_EQ(CODEX_EFFECTIVE_CONTEXT_WINDOW_PERCENT, 95);
}
