// Port of src/admin/chart-source-proxy.ts.
#include "uos/admin/chart_source_proxy.hpp"
#include "uos/auth/local_admin.hpp"
#include "uos/http.hpp"
#include "uos/provider/capacity_analytics.hpp"
#include "uos/provider/capacity_contract.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <array>
#include <utility>
#include <vector>

namespace uos::admin::chart_source_proxy {
  namespace {
    constexpr std::string_view REMOTE_CHART_ORIGIN = "https://ai.ubq.fi";
    constexpr double REMOTE_CHART_TIMEOUT_MS = 10000;
    constexpr double REMOTE_PUSH_TIMEOUT_MS = 5000;
    constexpr std::string_view REMOTE_CAPACITY_OBSERVATION_PATH = "/admin/providers/capacity/observations";
    constexpr std::string_view DEFAULT_CAPACITY_OBSERVATION_HOST_ID = "mac";
    constexpr std::string_view REMOTE_USER_AGENT = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36";
    constexpr std::array<std::string_view, 6> REMOTE_CAPACITY_CHART_FIELDS = {"history", "reset_events", "rate_limit_reset_events", "downtime_events", "prompt_cache", "ledger_growth"};
    std::shared_ptr<CapacityPushState> processPushState() {
      static auto state = std::make_shared<CapacityPushState>();
      return state;
    }
    std::optional<std::string> readAdminToken() {
      try {
        const auto raw = rt::env::get("DENO_DEPLOY_TOKEN");
        if (raw) {
          static const rt::Regex separator("[\\n,]", "g");
          for (const auto& entry : rt::split(*raw, separator)) {
            const auto token = rt::trim(entry);
            if (!token.empty()) return token;
          }
        }
      } catch (...) {}
      return std::nullopt;
    }
    bool isChartProxyPath(const rt::Request& request) {
      rt::URL url(request.url());
      return url.pathname() == "/admin/providers/capacity" || (url.pathname() == "/admin/errors" && url.searchParams().get("limit") == std::optional<std::string>("1"));
    }
    rt::URL remoteChartUrl(const std::string& pathname, const std::string& remoteOrigin) {
      return rt::URL(pathname == "/admin/errors" ? "/admin/errors?limit=1" : pathname, remoteOrigin);
    }
    rt::Task<void> releaseBody(rt::Response response) {
      if (auto body = response.body()) {
        try { co_await body->cancel(); } catch (...) {}
        co_await rt::microtask();
      }
    }
    rt::Headers remoteHeaders(const std::string& token) {
      return rt::Headers{{"Accept", "application/json"}, {"Authorization", "Bearer " + token}, {"User-Agent", std::string(REMOTE_USER_AGENT)}};
    }
    rt::Task<std::optional<json::value>> readRemoteChart(std::string pathname, std::string remoteOrigin, std::string token, rt::Fetch fetcher) {
      try {
        rt::RequestInit init;
        init.headers = remoteHeaders(token);
        init.cache = "no-store";
        init.signal = rt::AbortSignal::timeout(REMOTE_CHART_TIMEOUT_MS);
        const auto url = remoteChartUrl(pathname, remoteOrigin);
        auto task = fetcher(url, init);
        rt::Promise<rt::Response> pending(std::move(task));
        auto response = co_await pending.wait();
        if (!response.ok()) {
          rt::Promise<void> cancellation(releaseBody(response));
          co_await cancellation.wait();
          co_return std::nullopt;
        }
        rt::Promise<json::value> parsed(response.json());
        auto value = co_await parsed.wait();
        co_return value;
      } catch (...) { co_return std::nullopt; }
    }
    using PendingSample = json::object;
    std::vector<PendingSample> pendingCapacitySamples(const json::value& payload, const std::optional<double>& acknowledgedSampledAtMs, double batchSize) {
      if (!rt::isRecord(payload)) return {};
      const auto* history = rt::getArray(payload, "history");
      const auto snapshotAtMs = rt::getNumber(payload, "snapshot_at_ms");
      if (!history || !snapshotAtMs || !rt::Number::isFinite(*snapshotAtMs)) return {};
      std::vector<PendingSample> samples;
      rt::OrderedMap<double, double> newestByBucket;
      for (const auto& point : *history) {
        if (!rt::isRecord(point)) continue;
        const auto bucket = rt::getNumber(point, "bucket_start_at_ms");
        const auto sampled = rt::getNumber(point, "sampled_at_ms");
        if (!bucket || !sampled || !rt::Number::isFinite(*bucket) || !rt::Number::isFinite(*sampled)) continue;
        samples.push_back(point.as_object());
        const auto* newest = newestByBucket.get(*bucket);
        newestByBucket.set(*bucket, rt::Math::max(newest ? *newest : *sampled, *sampled));
      }
      std::vector<PendingSample> pending;
      for (const auto& point : samples) {
        const double bucket = *rt::getNumber(point, "bucket_start_at_ms");
        const double sampled = *rt::getNumber(point, "sampled_at_ms");
        const auto* newest = newestByBucket.get(bucket);
        if ((!acknowledgedSampledAtMs || sampled > *acknowledgedSampledAtMs) &&
            (bucket + provider::capacity_contract::PROVIDER_CAPACITY_HISTORY_BUCKET_MS <= *snapshotAtMs || sampled < (newest ? *newest : sampled))) pending.push_back(point);
      }
      rt::sort(pending, [](const auto& left, const auto& right) { return *rt::getNumber(left, "sampled_at_ms") - *rt::getNumber(right, "sampled_at_ms"); });
      double count = std::isnan(batchSize) ? 0 : rt::Math::trunc(batchSize);
      const double size = static_cast<double>(pending.size());
      count = count < 0 ? rt::Math::max(size + count, 0.0) : rt::Math::min(count, size);
      pending.resize(static_cast<std::size_t>(count));
      return pending;
    }
    enum class PushOutcome { stored, refused, timeout, retry };
    rt::Task<PushOutcome> pushCapacitySamples(std::vector<PendingSample> samples, std::string hostId, std::string remoteOrigin, std::string token, rt::Fetch fetcher) {
      const auto signal = rt::AbortSignal::timeout(REMOTE_PUSH_TIMEOUT_MS);
      try {
        json::array history;
        for (const auto& point : samples) history.push_back(point);
        json::object payload{{"host", hostId}, {"history", std::move(history)}};
        rt::RequestInit init;
        init.method = "POST";
        init.headers = remoteHeaders(token);
        init.headers->set("Content-Type", "application/json");
        init.body = rt::jsonStringify(payload);
        init.cache = "no-store";
        init.signal = signal;
        const rt::URL url(REMOTE_CAPACITY_OBSERVATION_PATH, remoteOrigin);
        auto task = fetcher(url, init);
        rt::Promise<rt::Response> pending(std::move(task));
        auto response = co_await pending.wait();
        if (!response.ok()) {
          rt::Promise<void> cancellation(releaseBody(response));
          co_await cancellation.wait();
          co_return response.status() == 400 || response.status() == 413 ? PushOutcome::refused : PushOutcome::retry;
        }
        rt::Promise<json::value> parsed(response.json());
        const auto acknowledgement = co_await parsed.wait();
        co_return rt::isRecord(acknowledgement) && rt::getString(acknowledgement, "host") == std::optional(hostId) && rt::getNumber(acknowledgement, "accepted")
                    ? PushOutcome::stored : PushOutcome::retry;
      } catch (...) {
        const auto failure = std::current_exception();
        const auto thrown = rt::thrownValue(failure);
        const bool timeout = signal.aborted() || rt::errorName(failure) == "TimeoutError" || (thrown && rt::isRecord(*thrown) && rt::getString(*thrown, "name") == std::optional<std::string>("TimeoutError"));
        co_return timeout ? PushOutcome::timeout : PushOutcome::retry;
      }
    }
    rt::Task<void> pushLoop(std::vector<PendingSample> samples, std::string hostId, std::string remoteOrigin, std::string token, rt::Fetch fetcher, std::shared_ptr<CapacityPushState> state) {
      for (;;) {
        auto task = pushCapacitySamples(samples, hostId, remoteOrigin, token, fetcher);
        rt::Promise<PushOutcome> pending(std::move(task));
        const auto outcome = co_await pending.wait();
        if (outcome == PushOutcome::refused && samples.size() > 1) {
          samples.resize(samples.size() / 2);
          continue;
        }
        if (outcome == PushOutcome::timeout) state->batchSize = rt::Math::max(1.0, rt::Math::floor(static_cast<double>(samples.size()) / 2));
        const auto newest = samples.empty() ? std::nullopt : rt::getNumber(samples.back(), "sampled_at_ms");
        if ((outcome == PushOutcome::stored || outcome == PushOutcome::refused) && newest)
          state->acknowledgedSampledAtMs = rt::Math::max(state->acknowledgedSampledAtMs.value_or(*newest), *newest);
        co_return;
      }
    }
    void startCapacityPush(const json::value& payload, const std::string& hostId, const std::string& remoteOrigin, const std::string& token, const rt::Fetch& fetcher, const std::shared_ptr<CapacityPushState>& state) {
      if (state->inFlight) return;
      auto samples = pendingCapacitySamples(payload, state->acknowledgedSampledAtMs, state->batchSize.value_or(provider::capacity_analytics::MAX_REMOTE_CAPACITY_OBSERVATIONS));
      if (samples.empty()) return;
      rt::Promise<void> pending(pushLoop(samples, hostId, remoteOrigin, token, fetcher, state));
      const auto cleanup = [state] { state->inFlight = std::nullopt; };
      state->inFlight = pending.finally_(cleanup);
    }
    json::value mergeRemoteCapacityChart(const json::value& localPayload, const json::value& remotePayload) {
      if (!rt::isRecord(localPayload) || !rt::isRecord(remotePayload)) return localPayload;
      auto merged = localPayload.as_object();
      for (const auto& field : REMOTE_CAPACITY_CHART_FIELDS) if (const auto* value = rt::get(remotePayload, field)) rt::objectSet(merged, field, *value);
      return merged;
    }
    rt::Task<rt::Response> proxy(rt::Request request, rt::Promise<rt::Response> localResponse, ChartSourceProxyDeps deps) {
      auto local = co_await localResponse.wait();
      const bool loopback = deps.isLoopbackAdmin ? deps.isLoopbackAdmin(request) : auth::local_admin::isAdminAuthDisabledForRequest(request);
      if (!loopback || !isChartProxyPath(request)) co_return local;
      const auto token = deps.token.isUndefined() ? readAdminToken() : deps.token.isNull() ? std::nullopt : std::optional(*deps.token);
      if (!rt::truthy(token) || !local.ok()) co_return local;
      const auto remoteOrigin = deps.remoteOrigin.value_or(std::string(REMOTE_CHART_ORIGIN));
      const auto fetcher = deps.fetch ? deps.fetch : rt::globalFetch();
      const auto pathname = rt::URL(request.url()).pathname();
      json::value localPayload;
      try {
        auto copy = local.clone();
        rt::Promise<json::value> parsed(copy.json());
        localPayload = co_await parsed.wait();
      } catch (...) { co_return local; }
      const rt::Headers headers{{"Cache-Control", "no-store"}};
      if (pathname == "/admin/errors") {
        rt::Promise<std::optional<json::value>> pending(readRemoteChart(pathname, remoteOrigin, *token, fetcher));
        const auto overlay = co_await pending.wait();
        if (!rt::isRecord(localPayload) || !overlay || !rt::isRecord(*overlay) || !rt::getArray(*overlay, "five_xx_buckets")) co_return local;
        auto payload = localPayload.as_object();
        payload["five_xx_buckets"] = *rt::getArray(*overlay, "five_xx_buckets");
        co_return http::json(200, payload, headers);
      }
      const bool companion = deps.isMacCompanionService ? deps.isMacCompanionService() : auth::local_admin::isMacCompanionServiceListener();
      const auto hostId = deps.hostId.isUndefined() ? std::optional(std::string(DEFAULT_CAPACITY_OBSERVATION_HOST_ID)) : deps.hostId.isNull() ? std::nullopt : std::optional(*deps.hostId);
      if (companion && hostId && provider::capacity_analytics::isRemoteCapacityHostId(std::string_view(*hostId)))
        startCapacityPush(localPayload, *hostId, remoteOrigin, *token, fetcher, deps.pushState ? deps.pushState : processPushState());
      rt::Promise<std::optional<json::value>> pending(readRemoteChart(pathname, remoteOrigin, *token, fetcher));
      const auto remotePayload = co_await pending.wait();
      if (!remotePayload) co_return local;
      co_return http::json(200, mergeRemoteCapacityChart(localPayload, *remotePayload), headers);
    }
  }
  rt::Task<rt::Response> proxyAdminChartSource(rt::Request request, rt::Response localResponse) {
    return proxy(std::move(request), rt::Promise<rt::Response>::resolve(localResponse), ChartSourceProxyDeps());
  }
  rt::Task<rt::Response> proxyAdminChartSource(rt::Request request, rt::Response localResponse, const ChartSourceProxyDeps& deps) {
    return proxy(std::move(request), rt::Promise<rt::Response>::resolve(localResponse), deps);
  }
  rt::Task<rt::Response> proxyAdminChartSource(rt::Request request, rt::Promise<rt::Response> localResponse) {
    return proxy(std::move(request), std::move(localResponse), ChartSourceProxyDeps());
  }
  rt::Task<rt::Response> proxyAdminChartSource(rt::Request request, rt::Promise<rt::Response> localResponse, const ChartSourceProxyDeps& deps) {
    return proxy(std::move(request), std::move(localResponse), deps);
  }
}
