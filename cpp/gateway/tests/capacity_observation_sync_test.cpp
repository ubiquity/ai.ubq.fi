// Links: tests/capacity_observation_sync_test.cpp src/admin/capacity_observation_sync.cpp
//        src/provider/capacity_analytics.cpp src/provider/capacity_parse.cpp src/provider/capacity_contract.cpp
//        src/http.cpp src/kv.cpp src/config.cpp src/release.cpp src/utils.cpp tests/helpers/counting_kv.cpp
//        src/admin/chart_source_proxy.cpp src/auth/local_admin.cpp src/auth/relay.cpp src/provider/cerebras_rate_limits.cpp
//        src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
// Port of tests/capacity-observation-sync.test.ts, including real proxy-to-ingest sync.
#include "uos/admin/capacity_observation_sync.hpp"
#include "uos/admin/chart_source_proxy.hpp"
#include "uos/config.hpp"
#include "uos/provider/capacity_analytics.hpp"
#include "uos/provider/capacity_contract.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include "tests/helpers/counting_kv.hpp"
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace contract = uos::provider::capacity_contract;
namespace analytics = uos::provider::capacity_analytics;
using contract::ProviderCapacityHistoryPoint;
using analytics::RemoteCapacityObservation;
using FakeKv = uos::tests::helpers::counting_kv::CountingKv;

namespace {
  const std::string COHORT_A(64, 'a');
  const std::string COHORT_B(64, 'b');
  const std::string COHORT_C(64, 'c');
  constexpr double BUCKET_MS = contract::PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  double bucket(double index) { return index * BUCKET_MS; }
  struct CredentialScope {
    rt::env::ScopedEnvironment env{{{"HOME", "/nonexistent/w05c8-isolated"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    CredentialScope() { uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialScope() { uos::config::config.codexAuthJsonB64 = saved; }
  };
  ProviderCapacityHistoryPoint historyPoint(double bucketStartAtMs, double sampledAtMs, double usedPercent) {
    auto codex = [sampledAtMs](double slot, const std::string& cohort, const std::string& label, double used) {
      return contract::ProviderCapacityCodexSource{
        .label = label, .slot = slot, .account_cohort_id = cohort, .state = "available",
        .source_observed_at_ms = sampledAtMs, .snapshot_at_ms = sampledAtMs,
        .failure_kind = std::nullopt, .failure_status = std::nullopt,
        .windows = {.primary = contract::ProviderCapacityWindow{.limit_window_seconds = 604800, .used_percent = used,
                    .reset_at_ms = sampledAtMs + slot * 1000}, .secondary = std::nullopt}, .additional_rate_limits = {}};
    };
    contract::ProviderCapacityMeteredSource metered{
      .label = "Metered fallback", .state = "available", .source_observed_at_ms = sampledAtMs,
      .snapshot_at_ms = sampledAtMs,
      .wallet = {.balance_credits = std::nullopt, .baseline_credits = std::nullopt,
        .refill_cycle_remaining_percent = std::nullopt, .refill_cycle_used_percent = std::nullopt,
        .unlimited_quota = true, .total_available = 0, .total_granted = 0, .total_used = 0,
        .cycle_started_at_ms = std::nullopt, .last_credit_at_ms = std::nullopt, .confidence = std::nullopt,
        .cache_state = "fresh", .reset_at_ms = nullptr}};
    return {.bucket_start_at_ms = bucketStartAtMs, .sampled_at_ms = sampledAtMs,
      .sources = {codex(1, COHORT_A, "p***@gmail.com", usedPercent), codex(2, COHORT_B, "u***@gmail.com", usedPercent + 1), metered}};
  }
  template <class F> ProviderCapacityHistoryPoint withSlot(ProviderCapacityHistoryPoint point, int slot, F patch) {
    patch(slot == 1 ? std::get<0>(point.sources) : std::get<1>(point.sources));
    return point;
  }
  ProviderCapacityHistoryPoint unreachable(ProviderCapacityHistoryPoint point, int slot) {
    return withSlot(std::move(point), slot, [](auto& source) {
      source.state = "unavailable"; source.failure_kind = "unreachable"; source.windows = {.primary = std::nullopt, .secondary = std::nullopt};
    });
  }
  ProviderCapacityHistoryPoint notConfigured(ProviderCapacityHistoryPoint point, int slot) {
    return withSlot(std::move(point), slot, [](auto& source) {
      source.state = "unavailable"; source.failure_kind = "not_configured"; source.account_cohort_id.reset();
      source.windows = {.primary = std::nullopt, .secondary = std::nullopt};
    });
  }
  RemoteCapacityObservation remote(ProviderCapacityHistoryPoint point, std::string host = "mac") { return {.host = std::move(host), .point = std::move(point)}; }
  std::vector<ProviderCapacityHistoryPoint> pointsOf(const std::vector<RemoteCapacityObservation>& observations) {
    std::vector<ProviderCapacityHistoryPoint> points;
    for (const auto& observation : observations) points.push_back(observation.point);
    return points;
  }
  json::value bodyFor(const std::string& host, const std::vector<ProviderCapacityHistoryPoint>& points) {
    return json::object{{"host", host}, {"history", json::value_from(points)}};
  }
  rt::Task<rt::Response> ingest(std::shared_ptr<rt::Kv> kv, const json::value& body, double nowMs) {
    rt::RequestInit init{.method = "POST", .headers = rt::Headers{{"Content-Type", "application/json"}}, .body = rt::jsonStringify(body)};
    rt::Request request("https://ai.ubq.fi/admin/providers/capacity/observations", init);
    uos::admin::capacity_observation_sync::CapacityObservationIngestOptions options{.kv = std::move(kv), .now = [nowMs] { return nowMs; }};
    return uos::admin::capacity_observation_sync::handleCapacityObservationIngest(request, options);
  }
  class FailingKv final : public FakeKv {
  public:
    std::unique_ptr<rt::AtomicOperation> atomic() override { throw rt::JsError("database is locked"); }
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions = {}) override { throw rt::JsError("database is locked"); co_return rt::KvCommitResult{}; }
  };
}

TEST("remote capacity host ids stay bounded lowercase labels") {
  EXPECT(analytics::isRemoteCapacityHostId(std::string_view("mac")));
  EXPECT(analytics::isRemoteCapacityHostId(std::string_view("mac-1")));
  EXPECT(!analytics::isRemoteCapacityHostId(std::string_view("Mac")));
  EXPECT(!analytics::isRemoteCapacityHostId(std::string_view("")));
  EXPECT(!analytics::isRemoteCapacityHostId(std::string_view(std::string(33, 'a'))));
}
TEST("remote capacity observations reject any malformed row") {
  const auto valid = historyPoint(bucket(1), bucket(1) + 1000, 7);
  EXPECT_EQ(analytics::parseRemoteCapacityObservations(json::array{json::value_from(valid)})->size(), 1u);
  EXPECT(!analytics::parseRemoteCapacityObservations(json::array{}));
  auto invalid = valid; invalid.bucket_start_at_ms = -1;
  EXPECT(!analytics::parseRemoteCapacityObservations(json::array{json::value_from(invalid)}));
  EXPECT(!analytics::parseRemoteCapacityObservations(json::array{json::value_from(valid), json::object{{"nope", true}}}));
}
TEST("remote capacity observations reject unaligned buckets, samples outside their bucket, and oversized batches") {
  const auto valid = historyPoint(bucket(1), bucket(1) + 1000, 7);
  EXPECT(!analytics::parseRemoteCapacityObservations(json::array{json::value_from(historyPoint(bucket(1) + 1, bucket(1) + 1000, 7))}));
  EXPECT(!analytics::parseRemoteCapacityObservations(json::array{json::value_from(historyPoint(bucket(1), bucket(1) - 1, 7))}));
  EXPECT(!analytics::parseRemoteCapacityObservations(json::array{json::value_from(historyPoint(bucket(1), bucket(2), 7))}));
  EXPECT_EQ(analytics::parseRemoteCapacityObservations(json::array{json::value_from(historyPoint(bucket(1), bucket(2) - 1, 7))})->size(), 1u);
  EXPECT(analytics::MAX_REMOTE_CAPACITY_OBSERVATIONS < contract::PROVIDER_CAPACITY_HISTORY_RETENTION_MS / BUCKET_MS);
  json::array full;
  for (int index = 0; index < analytics::MAX_REMOTE_CAPACITY_OBSERVATIONS; ++index) full.push_back(json::value_from(historyPoint(bucket(index), bucket(index) + 1, 7)));
  EXPECT_EQ(analytics::parseRemoteCapacityObservations(full)->size(), static_cast<std::size_t>(analytics::MAX_REMOTE_CAPACITY_OBSERVATIONS));
  full.push_back(json::value_from(valid)); EXPECT(!analytics::parseRemoteCapacityObservations(full));
  auto oversized = withSlot(valid, 1, [](auto& source) { source.label = std::string(64 * 1024, 'x'); });
  EXPECT(!analytics::parseRemoteCapacityObservations(json::array{json::value_from(oversized)}));
}
ASYNC_TEST("remote capacity observations persist in the analytics namespace with their host") {
  CredentialScope scope;
  auto kv = std::make_shared<FakeKv>();
  std::vector<ProviderCapacityHistoryPoint> points{historyPoint(bucket(0), 1000, 7), historyPoint(bucket(1), bucket(1) + 1000, 0)};
  auto storing = analytics::storeRemoteCapacityObservations(kv, "mac", points, bucket(2)); const double accepted = co_await std::move(storing);
  EXPECT_EQ(accepted, 2);
  auto read = co_await analytics::readRemoteCapacityObservations(kv, bucket(2));
  EXPECT_EQ(read.size(), 2u); EXPECT_EQ(read[0].host, std::string("mac")); EXPECT_EQ(read[1].host, std::string("mac"));
  EXPECT_EQ(read[0].point.bucket_start_at_ms, bucket(0)); EXPECT_EQ(read[1].point.bucket_start_at_ms, bucket(1));
}
ASYNC_TEST("remote capacity observations keep same-bucket transition points") {
  CredentialScope scope;
  auto kv = std::make_shared<FakeKv>();
  auto exhausted = historyPoint(bucket(1), bucket(1) + 1000, 100); auto refilled = historyPoint(bucket(1), bucket(1) + 600000, 0);
  std::vector<ProviderCapacityHistoryPoint> points{exhausted, refilled};
  auto storing = analytics::storeRemoteCapacityObservations(kv, "mac", points, bucket(2)); const double accepted = co_await std::move(storing); EXPECT_EQ(accepted, 2);
  auto read = co_await analytics::readRemoteCapacityObservations(kv, bucket(2)); EXPECT_EQ(read.size(), 2u);
  EXPECT_EQ(read[0].point.sampled_at_ms, exhausted.sampled_at_ms); EXPECT_EQ(read[1].point.sampled_at_ms, refilled.sampled_at_ms);
  std::vector<rt::KvKey> keys;
  for (const auto& row : kv->entries.values()) keys.push_back(row.key);
  const std::vector<rt::KvKey> expected{rt::KvKey{analytics::REMOTE_CAPACITY_OBSERVATION_PREFIX, "mac", bucket(1), exhausted.sampled_at_ms},
    rt::KvKey{analytics::REMOTE_CAPACITY_OBSERVATION_PREFIX, "mac", bucket(1), refilled.sampled_at_ms}};
  EXPECT_EQ(keys, expected);
}
ASYNC_TEST("remote capacity observation writes are committed in bounded atomic batches") {
  CredentialScope scope;
  auto kv = std::make_shared<FakeKv>(); std::vector<ProviderCapacityHistoryPoint> points;
  for (int index = 0; index < 130; ++index) points.push_back(historyPoint(bucket(index), bucket(index) + 1, 7));
  auto storing = analytics::storeRemoteCapacityObservations(kv, "mac", points, bucket(200)); const double accepted = co_await std::move(storing); EXPECT_EQ(accepted, 130);
  int commits = 0; int directSets = 0; double largest = 0;
  for (const auto& command : kv->commands) { if (command.command == "set") ++directSets; if (command.command == "atomic.commit") { ++commits; largest = rt::Math::max(largest, command.atomicMutations); } }
  EXPECT_EQ(directSets, 0); EXPECT(commits >= 2 && commits <= 3); EXPECT(largest <= 64);
  auto read = co_await analytics::readRemoteCapacityObservations(kv, bucket(200)); EXPECT_EQ(read.size(), 130u);
}
ASYNC_TEST("remote capacity observations drop points outside the seven-day retention") {
  CredentialScope scope;
  auto kv = std::make_shared<FakeKv>(); const double nowMs = 8 * 24 * 60 * 60000.0; const double recentBucket = nowMs - BUCKET_MS;
  std::vector<ProviderCapacityHistoryPoint> points{historyPoint(0, 1000, 7), historyPoint(recentBucket, recentBucket + 1000, 0)};
  auto storing = analytics::storeRemoteCapacityObservations(kv, "mac", points, nowMs); const double accepted = co_await std::move(storing); EXPECT_EQ(accepted, 1);
  auto read = co_await analytics::readRemoteCapacityObservations(kv, nowMs); EXPECT_EQ(read.size(), 1u); EXPECT_EQ(read[0].point.bucket_start_at_ms, recentBucket);
}
TEST("pushed samples fill only empty VPS buckets, and only when available") {
  std::vector<ProviderCapacityHistoryPoint> local{historyPoint(bucket(0), bucket(0) + 1000, 7), historyPoint(bucket(3), bucket(3) + 1000, 9)};
  const auto failed = withSlot(historyPoint(bucket(2), bucket(2) + 2000, 8), 2, [](auto& source) { source.state = "unavailable"; source.failure_kind = "http_error"; source.failure_status = 401; });
  std::vector<RemoteCapacityObservation> remotePoints{remote(historyPoint(bucket(0), bucket(0) + 5000, 99)), remote(historyPoint(bucket(1), bucket(1) + 1000, 100)),
    remote(historyPoint(bucket(1), bucket(1) + 600000, 0)), remote(unreachable(historyPoint(bucket(2), bucket(2) + 1000, 8), 1)), remote(failed)};
  auto merged = analytics::mergeCapacityHistory(local, remotePoints);
  const std::vector<ProviderCapacityHistoryPoint> expected{local[0], remotePoints[1].point, remotePoints[2].point, local[1]}; EXPECT_EQ(merged, expected);
  EXPECT_EQ(std::get<0>(merged[0].sources).windows.primary->used_percent, std::optional<double>(7));
}
TEST("pushed samples from other slot accounts never fill a VPS gap") {
  auto before = historyPoint(bucket(0), bucket(0) + 1000, 7);
  auto after = withSlot(historyPoint(bucket(4), bucket(4) + 1000, 7), 1, [](auto& source) { source.account_cohort_id = COHORT_C; });
  auto following = withSlot(historyPoint(bucket(3), bucket(3) + 1000, 8), 1, [](auto& source) { source.account_cohort_id = COHORT_C; });
  auto wrong = withSlot(historyPoint(bucket(2), bucket(2) + 1000, 8), 2, [](auto& source) { source.account_cohort_id = COHORT_C; });
  auto unknown = withSlot(historyPoint(bucket(2), bucket(2) + 2000, 8), 1, [](auto& source) { source.account_cohort_id.reset(); });
  std::vector<ProviderCapacityHistoryPoint> local{before, after}; std::vector<RemoteCapacityObservation> points{
    remote(historyPoint(bucket(1), bucket(1) + 1000, 8)), remote(following), remote(wrong), remote(unknown), remote(notConfigured(historyPoint(bucket(2), bucket(2) + 3000, 8), 2))};
  auto merged = analytics::mergeCapacityHistory(local, points);
  const std::vector<ProviderCapacityHistoryPoint> expected{before, points[0].point, following, after}; EXPECT_EQ(merged, expected);
}
TEST("pushed samples match an unconfigured slot only against an unconfigured VPS slot") {
  std::vector<ProviderCapacityHistoryPoint> local{notConfigured(historyPoint(bucket(0), bucket(0) + 1000, 7), 2)};
  std::vector<RemoteCapacityObservation> points{remote(notConfigured(historyPoint(bucket(1), bucket(1) + 1000, 8), 2)), remote(historyPoint(bucket(2), bucket(2) + 1000, 8))};
  const std::vector<ProviderCapacityHistoryPoint> expected{local[0], points[0].point}; EXPECT_EQ(analytics::mergeCapacityHistory(local, points), expected);
}
TEST("one observer fills a VPS gap bucket, and a VPS with no samples takes none") {
  std::vector<ProviderCapacityHistoryPoint> local{historyPoint(bucket(0), bucket(0) + 1000, 7)};
  std::vector<RemoteCapacityObservation> points{remote(historyPoint(bucket(1), bucket(1) + 2000, 50), "mac-studio"), remote(historyPoint(bucket(1), bucket(1) + 1000, 8)), remote(historyPoint(bucket(1), bucket(1) + 1000, 8))};
  const std::vector<ProviderCapacityHistoryPoint> expected{local[0], points[1].point}; EXPECT_EQ(analytics::mergeCapacityHistory(local, points), expected);
  const std::vector<ProviderCapacityHistoryPoint> empty; EXPECT_EQ(analytics::mergeCapacityHistory(empty, points), empty);
}
ASYNC_TEST("capacity observation ingest validates and stores a batch") {
  CredentialScope scope;
  auto kv = std::make_shared<FakeKv>(); const auto body = bodyFor("mac", {historyPoint(bucket(1), bucket(1) + 1000, 7)});
  auto task = ingest(kv, body, bucket(2)); auto response = co_await std::move(task); EXPECT_EQ(response.status(), 200);
  const auto value = co_await response.json(); const json::value expected = json::object{{"accepted", 1}, {"host", "mac"}}; EXPECT(rt::deepEqual(value, expected));
  const auto read = co_await analytics::readRemoteCapacityObservations(kv, bucket(2)); EXPECT_EQ(read.size(), 1u);
}
ASYNC_TEST("capacity observation ingest rejects invalid hosts and malformed rows") {
  CredentialScope scope;
  auto kv = std::make_shared<FakeKv>();
  const std::vector<json::value> bodies{bodyFor("Mac!", {historyPoint(bucket(1), bucket(1) + 1000, 7)}),
    json::object{{"host", "mac"}, {"history", json::array{json::object{{"nope", true}}}}}, bodyFor("mac", {historyPoint(bucket(1) + 1, bucket(1) + 1000, 7)})};
  for (const auto& body : bodies) { auto task = ingest(kv, body, bucket(2)); const auto response = co_await std::move(task); EXPECT_EQ(response.status(), 400); }
  EXPECT_EQ(kv->entries.size(), 0u);
}
ASYNC_TEST("capacity observation ingest reports a failed write instead of a partial acknowledgement") {
  CredentialScope scope;
  auto kv = std::make_shared<FailingKv>(); const auto body = bodyFor("mac", {historyPoint(bucket(1), bucket(1) + 1000, 7)});
  auto task = ingest(kv, body, bucket(2)); const auto response = co_await std::move(task); EXPECT_EQ(response.status(), 503);
}

namespace {
  namespace proxy = uos::admin::chart_source_proxy;
  std::string inputUrl(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->href();
    return std::get<rt::Request>(input).url();
  }
  rt::Task<void> syncMacHistory(std::shared_ptr<rt::Kv> kv, const std::vector<ProviderCapacityHistoryPoint>& history,
                                double nowMs, std::shared_ptr<proxy::CapacityPushState> state) {
    const json::value data = json::object{{"snapshot_at_ms", rt::makeNumber(nowMs)}, {"history", json::value_from(history)}};
    rt::Response localResponse(rt::jsonStringify(data));
    rt::Request request("http://localhost:7999/admin/providers/capacity");
    proxy::ChartSourceProxyDeps deps;
    deps.token = std::string("test-token"); deps.hostId = std::string("mac"); deps.pushState = state;
    deps.isLoopbackAdmin = [](rt::Request) { return true; }; deps.isMacCompanionService = [] { return true; };
    deps.fetch = [kv, nowMs](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
      if (init.method == std::optional<std::string>("POST")) {
        rt::Request upstream(inputUrl(input), init);
        uos::admin::capacity_observation_sync::CapacityObservationIngestOptions options{.kv = kv, .now = [nowMs] { return nowMs; }};
        return uos::admin::capacity_observation_sync::handleCapacityObservationIngest(upstream, options);
      }
      return rt::fulfilledTask(rt::Response(std::string("{\"history\":[]}")));
    };
    auto task = proxy::proxyAdminChartSource(request, localResponse, deps); co_await std::move(task);
    const auto inFlight = state->inFlight;
    if (inFlight) co_await inFlight->wait();
  }
  int commitCount(const FakeKv& kv) {
    int result = 0; for (const auto& command : kv.commands) if (command.command == "atomic.commit") ++result; return result;
  }
}
ASYNC_TEST("240 companion polls store one finalized point per bucket, including after a restart") {
  CredentialScope scope;
  auto kv = std::make_shared<FakeKv>(); auto state = std::make_shared<proxy::CapacityPushState>();
  rt::OrderedMap<double, ProviderCapacityHistoryPoint> local;
  for (int poll = 0; poll < 240; ++poll) {
    const double nowMs = bucket(1) + poll * 30000 + 1000;
    const double bucketStart = rt::Math::floor(nowMs / BUCKET_MS) * BUCKET_MS;
    local.set(bucketStart, historyPoint(bucketStart, nowMs, 7));
    const auto history = rt::arrayFrom(local.values()); auto task = syncMacHistory(kv, history, nowMs, state); co_await std::move(task);
  }
  EXPECT_EQ(local.size(), 8u); EXPECT_EQ(kv->entries.size(), 7u);
  const auto history = rt::arrayFrom(local.values()); auto closing = syncMacHistory(kv, history, bucket(9), state); co_await std::move(closing);
  EXPECT_EQ(kv->entries.size(), 8u); EXPECT_EQ(commitCount(*kv), 8);
  const auto read = co_await analytics::readRemoteCapacityObservations(kv, bucket(9)); EXPECT_EQ(pointsOf(read), history);
  const auto restarted = std::make_shared<proxy::CapacityPushState>(); auto resync = syncMacHistory(kv, history, bucket(9), restarted); co_await std::move(resync);
  EXPECT_EQ(kv->entries.size(), 8u);
}
ASYNC_TEST("open-bucket transitions survive without retaining intermediate companion refreshes") {
  CredentialScope scope;
  auto kv = std::make_shared<FakeKv>(); auto state = std::make_shared<proxy::CapacityPushState>();
  const auto exhausted = historyPoint(bucket(1), bucket(1) + 1000, 99); const auto refilled = historyPoint(bucket(1), bucket(1) + 30000, 5);
  const std::vector<ProviderCapacityHistoryPoint> first{exhausted, refilled}; auto syncing = syncMacHistory(kv, first, refilled.sampled_at_ms, state); co_await std::move(syncing);
  EXPECT_EQ(kv->entries.size(), 1u);
  const auto latest = historyPoint(bucket(1), bucket(1) + 60000, 6); const std::vector<ProviderCapacityHistoryPoint> second{exhausted, latest};
  auto refreshing = syncMacHistory(kv, second, latest.sampled_at_ms, state); co_await std::move(refreshing); EXPECT_EQ(kv->entries.size(), 1u);
  auto closing = syncMacHistory(kv, second, bucket(2), state); co_await std::move(closing);
  const auto read = co_await analytics::readRemoteCapacityObservations(kv, bucket(2)); EXPECT_EQ(pointsOf(read), second);
}
ASYNC_TEST("one row refused by the real ingest does not drop its 511 valid neighbours") {
  CredentialScope scope;
  auto kv = std::make_shared<FakeKv>(); auto state = std::make_shared<proxy::CapacityPushState>();
  std::vector<ProviderCapacityHistoryPoint> history;
  for (int index = 0; index < analytics::MAX_REMOTE_CAPACITY_OBSERVATIONS; ++index) history.push_back(historyPoint(bucket(index), bucket(index) + 1000, 7));
  constexpr std::size_t badIndex = 250; history[badIndex].bucket_start_at_ms += 1;
  for (int poll = 0; poll < 12 && state->acknowledgedSampledAtMs.value_or(-1) < history.back().sampled_at_ms; ++poll) {
    auto task = syncMacHistory(kv, history, bucket(520), state); co_await std::move(task);
  }
  std::vector<ProviderCapacityHistoryPoint> expected;
  for (std::size_t index = 0; index < history.size(); ++index) if (index != badIndex) expected.push_back(history[index]);
  const auto read = co_await analytics::readRemoteCapacityObservations(kv, bucket(520)); EXPECT_EQ(pointsOf(read), expected);
  EXPECT_EQ(state->acknowledgedSampledAtMs, std::optional<double>(history.back().sampled_at_ms));
}
