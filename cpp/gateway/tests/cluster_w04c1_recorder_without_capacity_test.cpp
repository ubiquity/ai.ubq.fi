// Links: tests/cluster_w04c1_recorder_without_capacity_test.cpp src/provider/claude_code_quota.cpp
//        src/provider/claude_code_usage_schema.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/utils.cpp
// Intentionally omits capacity.cpp: no recorder is installed merely by linking quota or the Claude account modules.
#include "uos/provider/claude_code_quota.hpp"
#include "uos/rt/test.hpp"

namespace {
  namespace rt = uos::rt;
  namespace quota = uos::provider::claude_code_quota;
  rt::Task<void> recorder(std::shared_ptr<rt::Kv>, quota::ClaudeCodeAcceptedObservation) { co_return; }
}

TEST("without capacity linked the Claude observation recorder slot starts empty") {
  auto dispose = quota::registerClaudeCodeObservationRecorder(&recorder);
  EXPECT(static_cast<bool>(dispose));
  dispose();
}
