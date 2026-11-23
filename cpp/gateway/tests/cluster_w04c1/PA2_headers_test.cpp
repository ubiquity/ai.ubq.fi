// Links: tests/cluster_w04c1/PA2_headers_test.cpp (plus src/rt/*.cpp, which unit.sh adds)
// Phase A check for the w04c1 cycle cluster: every header PA2 owns compiles in one translation
// unit, in reverse include order, and its load-time constants initialise and link without any
// module implementation.
#include "uos/provider/capacity.hpp"
#include "uos/provider/capacity_analytics.hpp"
#include "uos/provider/claude_capacity_rollups.hpp"
#include "uos/provider/claude_capacity_history.hpp"
#include "uos/provider/capacity_contract.hpp"
#include "uos/provider/claude_code_usage.hpp"
#include "uos/provider/claude_code.hpp"
#include "uos/provider/presentation.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/capacity_parse.hpp"
#include "uos/provider/capacity_history.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/claude_code_usage_fwd.hpp"
#include "uos/provider/claude_code_fwd.hpp"

#include "uos/rt/test.hpp"

TEST("PA2 cluster headers: load-time constants are initialised") {
  namespace provider = uos::provider;
  EXPECT_EQ(provider::presentation::PROVIDER_PRESENTATION.size(), std::size_t{10});
  const auto* gemini = provider::presentation::PROVIDER_PRESENTATION.get(std::string("gemini"));
  ASSERT(gemini);
  EXPECT_EQ(gemini->label, "Gemini");
  EXPECT(provider::presentation::PROVIDER_PRESENTATION.get(std::string("ubiquity"))->label == "Ubiquity");
  EXPECT(provider::capacity_contract::SHA256_HEX.test(std::string(64, 'a')));
  EXPECT_EQ(provider::capacity::PROVIDER_CAPACITY_SNAPSHOT_RETENTION_MS, 604'800'000.0);
  EXPECT_EQ(provider::claude_code_quota::CLAUDE_CODE_QUOTA_KV_KEY.size(), std::size_t{2});
}
