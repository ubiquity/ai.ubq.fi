// Differential cases for src/paid-fallback/ledger-state.ts, whose bun tests drive it only through
// unported modules (ledger admission, backfill, settlement, the handler). Recorded from the
// TypeScript under Bun 1.4.0 with Date.now fixed, over one seeded store: newest-first listing (ICU
// ordering of tied request ids, every limit shape, the 1,000-row cap), provider usage per window,
// window projections (stored, scanned, falsy stored values), outstanding counts over malformed
// rows, the pending-marker scan, the gate recompute (also after 128 conflicting commits), the
// reconciliation queue (absent, recording, rejecting, throwing) and resolveKv.
// Links: tests/paid_fallback_ledger_state_differential_test.cpp src/paid_fallback/ledger_state.cpp src/authority/native_paid.cpp
//   src/authority/native_paid_contracts.cpp src/types.cpp src/utils.cpp src/kv.cpp src/storage/kv.cpp src/release.cpp
//   src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/kv.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "paid_fallback_ledger_state_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace types = uos::types;
using namespace uos::paid_fallback::ledger_state;

namespace {
  const json::object& vectors() {
    static const json::value parsed = rt::jsonParse(kLedgerStateVectors);
    return parsed.as_object();
  }
  const json::object& results() { return vectors().at("results").as_object(); }

  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    std::fprintf(stderr, "    %s\n      actual:   %s\n      expected: %s\n", label.c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(expected).c_str());
    return false;
  }

  rt::KvValue untag(const json::value& value) {
    if (value.is_object()) {
      const json::object& object = value.as_object();
      if (object.size() == 1) {
        if (const std::optional<std::string_view> text = rt::getString(object, "$num")) return rt::KvValue(rt::stringToNumber(*text));
      }
      rt::KvObject out;
      for (const auto& member : object) out.set(std::string(member.key()), untag(member.value()));
      return rt::KvValue(std::move(out));
    }
    if (value.is_array()) {
      rt::KvArray out;
      for (const json::value& element : value.as_array()) out.push_back(untag(element));
      return rt::KvValue(std::move(out));
    }
    return rt::KvValue(value);
  }

  json::value tag(const json::value& value) {
    if (rt::isNumber(value)) {
      const double number = rt::toNumber(value);
      if (number == 0 && std::signbit(number)) return json::object{{"$num", "-0"}};
      if (!rt::Number::isFinite(number)) return json::object{{"$num", rt::numberToString(number)}};
      return value;
    }
    if (value.is_array()) {
      json::array out;
      for (const json::value& element : value.as_array()) out.push_back(tag(element));
      return out;
    }
    if (value.is_object()) {
      json::object out;
      for (const auto& member : value.as_object()) out[member.key()] = tag(member.value());
      return out;
    }
    return value;
  }

  rt::KvKey keyOf(const json::value& parts) {
    rt::KvKey key;
    for (const json::value& part : parts.as_array()) {
      if (part.is_string()) key.emplace_back(std::string(part.get_string()));
      else key.emplace_back(rt::toNumber(part));
    }
    return key;
  }

  json::value keyJson(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(part.isString() ? json::value(part.string()) : rt::makeNumber(part.number()));
    return parts;
  }

  // `{ ok: tag(result) }` or `{ error: "Name: message" }`.
  template <class T> rt::Task<json::value> describe(std::function<rt::Task<T>()> run, std::function<json::value(const T&)> toJson) {
    json::object outcome;
    try {
      rt::Task<T> running = run();
      const T result = co_await std::move(running);
      outcome["ok"] = tag(toJson(result));
    } catch (const rt::JsError& error) {
      outcome["error"] = error.name + ": " + error.message;
    }
    co_return outcome;
  }

  template <class T> json::value jsonOf(const T& value) { return json::value_from(value); }
  template <class T> json::value jsonOf(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }
  json::value jsonOf(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }

  rt::Task<std::shared_ptr<rt::MemoryKv>> seededKv() {
    auto kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
    for (const json::value& row : vectors().at("rows").as_array()) {
      const rt::KvKey key = keyOf(row.as_array()[0]);
      const rt::KvValue value = untag(row.as_array()[1]);
      co_await kv->set(key, value);
    }
    co_return kv;
  }

  // A store whose every commit conflicts (the oracle's `commitFailuresForced = Infinity`).
  class ConflictingOperation final : public rt::ForwardingAtomicOperation {
  public:
    ConflictingOperation(std::unique_ptr<rt::AtomicOperation> inner_, std::shared_ptr<int> commits_)
      : rt::ForwardingAtomicOperation(std::move(inner_)), commits(std::move(commits_)) {}
    rt::Task<rt::KvCommitResult> commit() override {
      *commits += 1;
      rt::KvCommitResult conflict;
      conflict.ok = false;
      co_return conflict;
    }

  private:
    std::shared_ptr<int> commits;
  };

  class ConflictingKv final : public rt::ForwardingKv {
  public:
    explicit ConflictingKv(std::shared_ptr<rt::MemoryKv> store_) : rt::ForwardingKv(*store_), store(std::move(store_)) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<ConflictingOperation>(rt::ForwardingKv::atomic(), commits); }
    std::shared_ptr<int> commits = std::make_shared<int>(0);

  private:
    std::shared_ptr<rt::MemoryKv> store;
  };

  // A store with the `enqueue` method the TS probes for.
  class QueueKv final : public rt::ForwardingKv, public KvEnqueue {
  public:
    enum class Mode { record, reject, throwNow };
    QueueKv(std::shared_ptr<rt::MemoryKv> store_, Mode mode_) : rt::ForwardingKv(*store_), store(std::move(store_)), mode(mode_) {}

    rt::Task<void> enqueue(rt::KvValue message, KvEnqueueOptions options) override {
      if (mode == Mode::throwNow) throw rt::JsError("sync throw");
      return record(std::move(message), std::move(options));
    }

    json::array queued;

  private:
    rt::Task<void> record(rt::KvValue message, KvEnqueueOptions options) {
      if (mode == Mode::reject) throw rt::JsError("queue down");
      json::object recorded;
      recorded["delay"] = rt::makeNumber(*options.delay);
      json::array schedule;
      for (double delay : *options.backoffSchedule) schedule.push_back(rt::makeNumber(delay));
      recorded["backoffSchedule"] = std::move(schedule);
      queued.push_back(json::array{message.toJson(), std::move(recorded)});
      co_return;
    }

    std::shared_ptr<rt::MemoryKv> store;
    Mode mode;
  };

  std::shared_ptr<rt::MemoryKv> emptyKv() { return std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); }); }
}

ASYNC_TEST("newest-first listing matches the oracle for every limit") {
  const double now = *rt::getNumber(vectors().at("NOW"));
  const auto realNow = rt::globalDateNow();
  rt::globalDateNow() = [now] { return now; };
  rt::Task<std::shared_ptr<rt::MemoryKv>> seeding = seededKv();
  const std::shared_ptr<rt::Kv> kv = co_await std::move(seeding);
  uos::kv::setKvForTest(kv);
  const std::function<json::value(const std::vector<types::PaidFallbackRequestV3>&)> rows = [](const std::vector<types::PaidFallbackRequestV3>& value) {
    return json::value_from(value);
  };
  int cases = 0;
  for (const json::value& item : results().at("lists").as_array()) {
    const json::array& pair = item.as_array();
    const bool absent = pair[0].is_string() && pair[0].get_string() == "<absent>";
    const double limit = absent ? 0 : untag(pair[0]).asNumber();
    rt::Task<json::value> describing = describe<std::vector<types::PaidFallbackRequestV3>>(
      [absent, limit, kv] { return absent ? listPaidFallbackRequestsV3("list-a") : listPaidFallbackRequestsV3("list-a", limit, kv); }, rows);
    const json::value outcome = co_await std::move(describing);
    EXPECT(same(outcome, pair[1], "list limit " + rt::jsonStringify(pair[0])));
    ++cases;
  }
  EXPECT_EQ(cases, 10);
  rt::Task<json::value> describing =
    describe<std::vector<types::PaidFallbackRequestV3>>([] { return listPaidFallbackRequestsV3("list-a", 10, std::shared_ptr<rt::Kv>()); }, rows);
  json::value outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("listNullKv"), "list null kv"));
  describing = describe<std::vector<types::PaidFallbackRequestV3>>([kv] { return listPaidFallbackRequestsV3("nobody", 10, kv); }, rows);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("listMissingKey"), "list missing key"));

  // The 1,000-row cap.
  const std::shared_ptr<rt::Kv> many = emptyKv();
  const json::object& template_ = vectors().at("rows").as_array()[0].as_array()[1].as_object();
  for (int index = 0; index < 1005; index += 1) {
    const std::string id = "m" + rt::padStart(rt::numberToString(index), 4, "0");
    json::object row = template_;
    row["key_id"] = "many";
    row["request_id"] = id;
    row["created_at_ms"] = index % 7;
    row["updated_at_ms"] = index % 7 + 1;
    const rt::KvKey key = paidFallbackRequestV3Key("many", id);
    const rt::KvValue value(row);
    co_await many->set(key, value);
  }
  rt::Task<std::vector<types::PaidFallbackRequestV3>> listing = listPaidFallbackRequestsV3("many", 5'000, many);
  const std::vector<types::PaidFallbackRequestV3> capped = co_await std::move(listing);
  const json::object& cap = results().at("cap").as_object();
  EXPECT_EQ(static_cast<double>(capped.size()), *rt::getNumber(cap.at("count")));
  json::array first;
  json::array last;
  for (std::size_t index = 0; index < 3 && index < capped.size(); index += 1) first.push_back(json::value(capped[index].request_id));
  for (std::size_t index = capped.size() >= 3 ? capped.size() - 3 : 0; index < capped.size(); index += 1) last.push_back(json::value(capped[index].request_id));
  EXPECT(same(first, cap.at("first"), "cap first"));
  EXPECT(same(last, cap.at("last"), "cap last"));
  uos::kv::setKvForTest(nullptr);
  rt::globalDateNow() = realNow;
}

ASYNC_TEST("provider usage, window projections and outstanding counts match the oracle") {
  const double now = *rt::getNumber(vectors().at("NOW"));
  const auto realNow = rt::globalDateNow();
  rt::globalDateNow() = [now] { return now; };
  rt::Task<std::shared_ptr<rt::MemoryKv>> seeding = seededKv();
  const std::shared_ptr<rt::Kv> kv = co_await std::move(seeding);
  const std::shared_ptr<rt::Kv> none;
  uos::kv::setKvForTest(kv);

  const std::function<json::value(const PaidFallbackProviderUsageByProviderV3&)> usageJson = [](const PaidFallbackProviderUsageByProviderV3& value) { return jsonOf(value); };
  rt::Task<json::value> describing = describe<PaidFallbackProviderUsageByProviderV3>([kv] { return getPaidFallbackProviderUsageV3("usage-a", 10'000, kv); }, usageJson);
  json::value outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("usage"), "usage"));
  describing = describe<PaidFallbackProviderUsageByProviderV3>([] { return getPaidFallbackProviderUsageV3("usage-a", 20'000); }, usageJson);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("usageOtherWindow"), "usage other window"));
  describing = describe<PaidFallbackProviderUsageByProviderV3>([none] { return getPaidFallbackProviderUsageV3("usage-a", 10'000, none); }, usageJson);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("usageNullKv"), "usage null kv"));

  using Projection = std::optional<PaidFallbackWindowProjectionV3>;
  const std::function<json::value(const Projection&)> projectionJson = [](const Projection& value) { return jsonOf(value); };
  const auto project = [&](std::string keyId, double limit, std::optional<std::shared_ptr<rt::Kv>> store) {
    return describe<Projection>(
      [keyId, limit, store] { return store ? getPaidFallbackWindowProjectionV3(keyId, 10'000, limit, *store) : getPaidFallbackWindowProjectionV3(keyId, 10'000, limit); },
      projectionJson);
  };
  describing = project("proj-a", 1'234, kv);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("projectionStored"), "projection stored"));
  describing = project("proj-b", -1, kv);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("projectionScan"), "projection scan"));
  describing = project("proj-c", 5, std::nullopt);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("projectionFalsyZero"), "projection falsy zero"));
  describing = project("proj-d", 5, kv);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("projectionFalsyEmpty"), "projection falsy empty"));
  describing = project("nobody", 5, kv);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("projectionEmpty"), "projection empty"));
  describing = project("proj-a", 5, none);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("projectionNullKv"), "projection null kv"));

  using Outstanding = std::optional<PaidFallbackOutstandingV3>;
  const std::function<json::value(const Outstanding&)> outstandingJson = [](const Outstanding& value) { return jsonOf(value); };
  const auto outstanding = [&](std::string keyId, std::optional<std::shared_ptr<rt::Kv>> store) {
    return describe<Outstanding>([keyId, store] { return store ? getPaidFallbackOutstandingV3(keyId, *store) : getPaidFallbackOutstandingV3(keyId); }, outstandingJson);
  };
  describing = outstanding("out-a", kv);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("outstanding"), "outstanding"));
  describing = outstanding("out-e", std::nullopt);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("outstandingMarkersOnly"), "outstanding markers only"));
  describing = outstanding("nobody", kv);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("outstandingNone"), "outstanding none"));
  describing = outstanding("out-null", kv);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("outstandingNullRow"), "outstanding null row"));
  describing = outstanding("out-undef", kv);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("outstandingNaNRow"), "outstanding NaN row"));
  describing = outstanding("out-a", none);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("outstandingNullKv"), "outstanding null kv"));
  uos::kv::setKvForTest(nullptr);
  rt::globalDateNow() = realNow;
}

ASYNC_TEST("the pending-marker scan and gate recompute match the oracle") {
  const double now = *rt::getNumber(vectors().at("NOW"));
  const auto realNow = rt::globalDateNow();
  rt::globalDateNow() = [now] { return now; };
  rt::Task<std::shared_ptr<rt::MemoryKv>> seeding = seededKv();
  const std::shared_ptr<rt::MemoryKv> store = co_await std::move(seeding);
  const std::shared_ptr<rt::Kv> kv = store;

  rt::Task<PendingMarkerScan> scanning = scanPaidFallbackPendingMarkers(kv);
  const PendingMarkerScan scan = co_await std::move(scanning);
  json::array keys;
  json::array values;
  for (const auto& entry : scan.entries) {
    keys.push_back(keyJson(entry.key));
    values.push_back(json::value_from(entry.value));
  }
  const json::object& expectedScan = results().at("scan").as_object();
  EXPECT(same(keys, expectedScan.at("keys"), "scan keys"));
  EXPECT(same(tag(values), expectedScan.at("values"), "scan values"));
  EXPECT(same(jsonOf(scan.earliest_due_at_ms), expectedScan.at("earliest_due_at_ms"), "scan earliest"));

  const std::function<json::value(const std::optional<double>&)> dueJson = [](const std::optional<double>& value) { return jsonOf(value); };
  rt::Task<json::value> describing = describe<std::optional<double>>([kv] { return recomputePaidFallbackReconciliationGateV3(kv); }, dueJson);
  json::value outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("recompute"), "recompute"));
  const rt::KvKey gateKey = paidFallbackReconciliationGateV3Key();
  const rt::KvEntryMaybe<> gate = co_await kv->get(gateKey);
  EXPECT(same(tag(gate.value.toJson()), results().at("gateRow"), "gate row"));

  const std::shared_ptr<rt::Kv> empty = emptyKv();
  describing = describe<std::optional<double>>([empty] { return recomputePaidFallbackReconciliationGateV3(empty); }, dueJson);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("recomputeEmpty"), "recompute empty"));
  const rt::KvEntryMaybe<> emptyGate = co_await empty->get(gateKey);
  EXPECT(same(emptyGate.value.toJson(), results().at("emptyGateRow"), "empty gate row"));
  scanning = scanPaidFallbackPendingMarkers(empty);
  const PendingMarkerScan emptyScan = co_await std::move(scanning);
  json::object emptyScanJson;
  emptyScanJson["entries"] = json::array();
  emptyScanJson["earliest_due_at_ms"] = jsonOf(emptyScan.earliest_due_at_ms);
  EXPECT_EQ(emptyScan.entries.size(), std::size_t{0});
  EXPECT(same(emptyScanJson, results().at("emptyScan"), "empty scan"));

  const auto conflictingStore = emptyKv();
  const rt::KvKey markerKey = paidFallbackPendingV3Key("k", "r");
  const rt::KvValue marker = rt::KvValue::object({{"created_at_ms", rt::KvValue(1)}, {"next_reconciliation_at_ms", rt::KvValue(42)}});
  co_await conflictingStore->set(markerKey, marker);
  const auto conflicting = std::make_shared<ConflictingKv>(conflictingStore);
  const std::shared_ptr<rt::Kv> conflictingKv = conflicting;
  describing = describe<std::optional<double>>([conflictingKv] { return recomputePaidFallbackReconciliationGateV3(conflictingKv); }, dueJson);
  outcome = co_await std::move(describing);
  EXPECT(same(outcome, results().at("recomputeConflict"), "recompute conflict"));
  EXPECT_EQ(static_cast<double>(*conflicting->commits), *rt::getNumber(results().at("recomputeConflictCommits")));
  rt::globalDateNow() = realNow;
}

ASYNC_TEST("the reconciliation queue and resolveKv match the oracle") {
  const std::shared_ptr<rt::Kv> plain = emptyKv();
  rt::Task<bool> enqueuing = enqueuePaidFallbackReconciliationJob(plain, "key-q");
  bool enqueued = co_await std::move(enqueuing);
  EXPECT_EQ(enqueued, results().at("enqueueWithoutQueue").as_bool());

  const auto withQueue = std::make_shared<QueueKv>(emptyKv(), QueueKv::Mode::record);
  enqueuing = enqueuePaidFallbackReconciliationJob(withQueue, "key-q");
  enqueued = co_await std::move(enqueuing);
  EXPECT_EQ(enqueued, results().at("enqueueDefault").as_bool());
  enqueuing = enqueuePaidFallbackReconciliationJob(withQueue, "key-r", 7'000);
  enqueued = co_await std::move(enqueuing);
  EXPECT_EQ(enqueued, results().at("enqueueDelayed").as_bool());
  EXPECT(same(withQueue->queued, results().at("queued"), "queued messages"));

  const auto failingQueue = std::make_shared<QueueKv>(emptyKv(), QueueKv::Mode::reject);
  enqueuing = enqueuePaidFallbackReconciliationJob(failingQueue, "key-q");
  enqueued = co_await std::move(enqueuing);
  EXPECT_EQ(enqueued, results().at("enqueueFailing").as_bool());
  const auto throwingQueue = std::make_shared<QueueKv>(emptyKv(), QueueKv::Mode::throwNow);
  enqueuing = enqueuePaidFallbackReconciliationJob(throwingQueue, "key-q");
  enqueued = co_await std::move(enqueuing);
  EXPECT_EQ(enqueued, results().at("enqueueThrowing").as_bool());

  const std::shared_ptr<rt::Kv> installed = emptyKv();
  uos::kv::setKvForTest(installed);
  rt::Task<std::shared_ptr<rt::Kv>> resolving = resolveKv(std::nullopt);
  std::shared_ptr<rt::Kv> resolved = co_await std::move(resolving);
  EXPECT_EQ(resolved == installed, results().at("resolveUndefined").as_bool());
  resolving = resolveKv(std::shared_ptr<rt::Kv>());
  resolved = co_await std::move(resolving);
  EXPECT(resolved == nullptr && results().at("resolveNull").is_null());
  resolving = resolveKv(plain);
  resolved = co_await std::move(resolving);
  EXPECT_EQ(resolved == plain, results().at("resolveOther").as_bool());
  uos::kv::setKvForTest(nullptr);
  resolving = resolveKv(std::nullopt);
  resolved = co_await std::move(resolving);
  EXPECT(resolved == nullptr && results().at("resolveUndefinedWithoutKv").is_null());
}
