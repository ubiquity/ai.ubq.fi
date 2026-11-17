// Links: tests/provider_capacity_analytics_differential_test.cpp src/provider/capacity_analytics.cpp src/provider/capacity_parse.cpp src/provider/capacity_contract.cpp src/config.cpp src/release.cpp src/utils.cpp
// Differential cases for src/provider/capacity-analytics.ts (its bun tests, capacity-observation-route/-sync and
// chart-source-proxy, need modules of later waves). The vectors (tests/provider_capacity_modules_differential_vectors.inc,
// shared with provider_capacity_history_events_differential_test.cpp) were recorded from the TS modules under bun 1.4.0
// with an in-memory Deno KV whose operations complete one macrotask later; the KV scenario repeats the generator's steps.
#include "tests/provider_capacity_differential_support.hpp"
#include "uos/provider/capacity_analytics.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace uos;
using namespace uos::tests::capacity_differential;
namespace analytics = provider::capacity_analytics;
using contract::ProviderCapacityHistoryPoint;

namespace {
  const json::object& section() { return vectors().at("analytics").as_object(); }

  // The generator's oversized inputs, rebuilt from their markers.
  json::value parseInputOf(const json::value& input) {
    const json::object* object = input.if_object();
    if (object && object->contains("$repeatFirstPoint")) {
      json::array points;
      for (int index = 0; index < 513; ++index) points.push_back(section().at("firstPoint"));
      return points;
    }
    if (object && object->contains("$bigLabelPoint")) {
      json::value point = section().at("firstPoint");
      point.as_object()["sources"].as_array()[0].as_object()["label"] = json::string(std::string(16 * 1024, 'x'));
      json::array points;
      points.push_back(std::move(point));
      return points;
    }
    return input;
  }

  // The generator's `point(bucket, sampled, codex(1, T), codex(2, T))` from a template of that shape: the metered row is
  // `metered(sampled)`.
  ProviderCapacityHistoryPoint retimed(const json::value& pointTemplate, double bucket, double sampled) {
    ProviderCapacityHistoryPoint point = recordOf<ProviderCapacityHistoryPoint>(pointTemplate);
    point.bucket_start_at_ms = bucket;
    point.sampled_at_ms = sampled;
    std::get<2>(point.sources).source_observed_at_ms = sampled;
    std::get<2>(point.sources).snapshot_at_ms = sampled;
    std::get<2>(point.sources).wallet.cycle_started_at_ms = sampled - 1000;
    return point;
  }

  // A store whose atomic commit reports `{ ok: false }` (the generator's `refusing` stub).
  class RefusingAtomic final : public rt::ForwardingAtomicOperation {
  public:
    using rt::ForwardingAtomicOperation::ForwardingAtomicOperation;
    rt::Task<rt::KvCommitResult> commit() override {
      rt::KvCommitResult refused;
      co_return refused;
    }
  };
  class RefusingKv final : private OwnedInnerKv, public rt::ForwardingKv {
  public:
    RefusingKv() : OwnedInnerKv(), rt::ForwardingKv(*owned) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<RefusingAtomic>(owned->atomic()); }
  };
}

TEST("capacity analytics: host ids are bounded lowercase labels") {
  for (const json::value& row : section().at("hostIds").as_array()) {
    const json::value& input = row.at("input");
    const bool expected = row.at("output").as_bool();
    if (isUndefinedTag(input)) {
      EXPECT_EQ(analytics::isRemoteCapacityHostId(static_cast<const json::value*>(nullptr)), expected);
      continue;
    }
    EXPECT_EQ(analytics::isRemoteCapacityHostId(input), expected);
    if (input.is_string()) EXPECT_EQ(analytics::isRemoteCapacityHostId(std::string_view(input.get_string())), expected);
  }
}

TEST("capacity analytics: a pushed batch parses only when every row is a bounded bucketed point") {
  for (const json::value& row : section().at("parse").as_array()) {
    const json::value input = parseInputOf(row.at("input"));
    EXPECT_JSON(plain(analytics::parseRemoteCapacityObservations(input)), row.at("output"), rt::jsonStringify(row.at("input")).substr(0, 200));
  }
  EXPECT(!analytics::parseRemoteCapacityObservations(static_cast<const json::value*>(nullptr)).has_value());
}

ASYNC_TEST("capacity analytics: store, read and refuse pushed rows over the KV") {
  const json::object& store = section().at("store").as_object();
  auto database = std::make_shared<rt::MemoryKvDatabase>([] { return rt::dateNow(); });
  std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>(database);
  const double now = rt::toNumber(store.at("now"));
  const std::vector<ProviderCapacityHistoryPoint> points = recordsOf<ProviderCapacityHistoryPoint>(store.at("points"));
  auto storing = analytics::storeRemoteCapacityObservations(kv, "host-a", points, now);
  const double stored = co_await std::move(storing);
  EXPECT_EQ(stored, rt::toNumber(store.at("stored")));

  std::vector<ProviderCapacityHistoryPoint> many;
  const json::value& manyTemplate = store.at("manyTemplate");
  constexpr double T = 1'800'000'000'000;
  constexpr double BUCKET = 15 * 60'000;
  for (int index = 0; index < static_cast<int>(rt::toNumber(store.at("manyCount"))); ++index) {
    many.push_back(retimed(manyTemplate, T + index * BUCKET, T + index * BUCKET + 1));
  }
  const std::uint64_t versionBefore = database->version();
  auto storingMany = analytics::storeRemoteCapacityObservations(kv, "host-b", many, T + 80 * BUCKET);
  const double storedMany = co_await std::move(storingMany);
  EXPECT_EQ(storedMany, rt::toNumber(store.at("storedMany")));
  EXPECT_EQ(static_cast<double>(database->version() - versionBefore), rt::toNumber(store.at("manyCommits")));

  const rt::KvKey prefix = analytics::REMOTE_CAPACITY_OBSERVATION_PREFIX;
  {
    const rt::KvValue first(store.at("points").as_array()[0]);
    auto badHost = kv->set(rt::KvKey{prefix, std::string("Bad Host"), T, T + 1}, first);
    co_await std::move(badHost);
    json::object nope;
    nope["nope"] = true;
    const rt::KvValue malformed(nope);
    auto malformedRow = kv->set(rt::KvKey{prefix, std::string("host-c"), T, T + 2}, malformed);
    co_await std::move(malformedRow);
    const rt::KvValue oldShapeValue(json::value_from(retimed(manyTemplate, T + BUCKET, T + BUCKET + 5)));
    auto oldShapeRow = kv->set(rt::KvKey{prefix, std::string("host-c"), T + BUCKET}, oldShapeValue);
    co_await std::move(oldShapeRow);
  }
  auto listing = rowsOf(kv, prefix, {});
  const json::value rows = co_await std::move(listing);
  EXPECT_JSON(rows, store.at("rows"), "rows");

  auto reading = analytics::readRemoteCapacityObservations(kv, T + 80 * BUCKET);
  const std::vector<analytics::RemoteCapacityObservation> read = co_await std::move(reading);
  json::array readJson;
  for (const analytics::RemoteCapacityObservation& observation : read) {
    json::object item;
    item["host"] = json::string(observation.host);
    item["point"] = json::value_from(observation.point);
    readJson.push_back(std::move(item));
  }
  EXPECT_JSON(readJson, store.at("read"), "read");

  auto failing = std::make_shared<FailingListKv>(true);
  auto failedRead = analytics::readRemoteCapacityObservations(failing, now);
  const std::vector<analytics::RemoteCapacityObservation> readFailed = co_await std::move(failedRead);
  EXPECT(readFailed.empty());
  EXPECT(store.at("readFailed").as_array().empty());

  auto refusing = std::make_shared<RefusingKv>();
  std::vector<ProviderCapacityHistoryPoint> one = {points[0]};
  auto refused = analytics::storeRemoteCapacityObservations(refusing, "host-a", one, now);
  const std::string thrown(store.at("thrown").as_string());
  EXPECT_REJECTS_MESSAGE(std::move(refused), rt::JsError, thrown.c_str());
}

TEST("capacity analytics: pushed samples fill only unobserved buckets beside matching local samples") {
  for (const json::value& row : section().at("merge").as_array()) {
    const std::vector<ProviderCapacityHistoryPoint> local = recordsOf<ProviderCapacityHistoryPoint>(row.at("local"));
    std::vector<analytics::RemoteCapacityObservation> remote;
    for (const json::value& item : row.at("remote").as_array()) {
      remote.push_back(analytics::RemoteCapacityObservation{.host = std::string(item.at("host").as_string()), .point = recordOf<ProviderCapacityHistoryPoint>(item.at("point"))});
    }
    EXPECT_JSON(plain(analytics::mergeCapacityHistory(local, remote)), row.at("output"), "merge");
  }
}
