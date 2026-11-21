#pragma once
// Port of src/health.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"
#include <optional>
#include <string_view>
namespace uos::health {
  void setActiveUpstreamHealthTimeoutMsForTest(std::optional<double> timeoutMs);
  rt::Task<json::object> getClaudeSubscriptionStatusSnapshot();
  struct PassiveProviderHealthOptions { std::optional<bool> includeQuota{}; };
  rt::Task<json::object> getPassiveProviderHealthSnapshot();
  rt::Task<json::object> getPassiveProviderHealthSnapshot(PassiveProviderHealthOptions options);
  rt::Task<rt::Response> handleHealthProviders();
  rt::Task<rt::Response> handleHealthProviders(PassiveProviderHealthOptions options);
  // Native startup records the enforced mode before the listener starts.
  void setConfinementMode(std::string_view mode);
  rt::Response handleHealth();
  rt::Task<rt::Response> handleFollowerHealth();
  rt::Task<rt::Response> handleHealthUpstream();
}
