#pragma once
// Port of src/admin/chart-source-proxy.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/nullable.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace uos::admin::chart_source_proxy {
  struct CapacityPushState {
    std::optional<double> acknowledgedSampledAtMs;
    std::optional<rt::Promise<void>> inFlight;
    std::optional<double> batchSize{};
  };
  struct ChartSourceProxyDeps {
    rt::Fetch fetch{};
    rt::Nullable<std::string> token{};
    std::optional<std::string> remoteOrigin{};
    std::function<bool(rt::Request)> isLoopbackAdmin{};
    std::function<bool()> isMacCompanionService{};
    rt::Nullable<std::string> hostId{};
    std::shared_ptr<CapacityPushState> pushState{};
  };
  rt::Task<rt::Response> proxyAdminChartSource(rt::Request request, rt::Response localResponse);
  rt::Task<rt::Response> proxyAdminChartSource(rt::Request request, rt::Response localResponse, const ChartSourceProxyDeps& deps);
  rt::Task<rt::Response> proxyAdminChartSource(rt::Request request, rt::Promise<rt::Response> localResponse);
  rt::Task<rt::Response> proxyAdminChartSource(rt::Request request, rt::Promise<rt::Response> localResponse, const ChartSourceProxyDeps& deps);
}
