// Differential cases for src/sentinel/replay-model.ts, replay-retention-schema.ts, replay-observation.ts and
// replay-envelope.ts beyond what tests/sentinel-replay-envelope-coverage.test.ts covers: the retention schema has no
// direct test its ported modules can run (every case of the retention, bootstrap, metadata, deletion-budget and
// staging tests drives src/sentinel/replay-retention*.ts, replay-store.ts or replay-read.ts).
//
// The vectors were recorded from Bun 1.4.0 by a script that called the TS functions on fixed inputs. The KV-backed
// schema functions ran over an in-memory store with Deno KV key order (strings before numbers), per-commit
// versionstamps and atomic versionstamp checks; here the same calls run over rt::MemoryKv, and versionstamps are
// compared as present / null. Numbers JSON cannot carry (NaN, the infinities, -0) are recorded as strings.
// The gunzip vectors are Bun's DecompressionStream("gzip") outcomes over gzip streams from Bun's CompressionStream
// and corrupted copies of them (concatenated members, trailing bytes, bad checks, truncation, other formats).
// Links: tests/sentinel_replay_differential_test.cpp src/sentinel/replay_model.cpp src/sentinel/replay_retention_schema.cpp
//        src/sentinel/replay_observation.cpp src/sentinel/replay_envelope.cpp src/sentinel/upstream_capture.cpp
//        src/sentinel/replay_limits.cpp src/request.cpp src/utils.cpp src/config.cpp src/release.cpp
#include "uos/sentinel/replay_envelope.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tests/sentinel_replay_differential_vectors.inc"
#include "tests/sentinel_replay_gunzip_vectors.inc"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace model = uos::sentinel::replay_model;
  namespace schema = uos::sentinel::replay_retention_schema;
  namespace observation = uos::sentinel::replay_observation;
  namespace envelope = uos::sentinel::replay_envelope;
  namespace upstream = uos::sentinel::upstream_capture;

  const json::value& V() {
    static const json::value parsed = [] {
      std::string text;
      for (const char* part : SENTINEL_REPLAY_VECTOR_PARTS) text += part;
      return rt::jsonParse(text);
    }();
    return parsed;
  }
  const json::array& section(std::string_view name) { return rt::get(V(), name)->as_array(); }

  const json::array& gunzipVectors() {
    static const json::value parsed = [] {
      std::string text;
      for (const char* part : SENTINEL_REPLAY_GUNZIP_VECTOR_PARTS) text += part;
      return rt::jsonParse(text);
    }();
    return parsed.as_array();
  }

  // `{ ok, output }` or `{ ok, error, message }`: what the Bun script recorded for one gunzip call.
  json::value gunzipOutcome(const rt::Reason& failure, const rt::Bytes& output) {
    json::object outcome;
    outcome["ok"] = !failure;
    if (!failure) {
      outcome["output"] = rt::toHex(output);
      return outcome;
    }
    try {
      std::rethrow_exception(failure);
    } catch (const rt::JsError& error) {
      outcome["error"] = error.name;
      outcome["message"] = error.message;
    } catch (...) {
      outcome["error"] = "(not a JsError)";
      outcome["message"] = rt::reasonToString(failure);
    }
    return outcome;
  }

  double numberOf(const json::value& value) {
    if (const std::optional<std::string_view> text = rt::getString(value)) {
      if (*text == "NaN") return std::numeric_limits<double>::quiet_NaN();
      if (*text == "Infinity") return std::numeric_limits<double>::infinity();
      if (*text == "-Infinity") return -std::numeric_limits<double>::infinity();
      if (*text == "-0") return -0.0;
    }
    return *rt::getNumber(value);
  }
  json::value numberJson(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
    if (value == 0 && std::signbit(value)) return "-0";
    return rt::canonicalNumbers(json::value(value));
  }

  json::value keyJson(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) parts.emplace_back(part.string());
      else if (part.isNumber()) parts.push_back(rt::canonicalNumbers(json::value(part.number())));
      else if (part.isBool()) parts.emplace_back(part.boolean());
      else parts.emplace_back("<unsupported>");
    }
    return parts;
  }
  rt::KvKey keyFrom(const json::value& value) {
    rt::KvKey key;
    for (const json::value& part : value.as_array()) {
      if (part.is_string()) key.emplace_back(std::string(part.get_string()));
      else key.emplace_back(*rt::getNumber(part));
    }
    return key;
  }

  void expectSame(const json::value& actual, const json::value& expected, const std::string& label, const char* file, int line) {
    if (!rt::deepEqual(rt::canonicalNumbers(actual), rt::canonicalNumbers(expected))) {
      rt::test::report(file, line, label + ": " + rt::jsonStringify(actual) + " != " + rt::jsonStringify(expected), false);
    }
  }
#define EXPECT_SAME(actual, expected, label) expectSame((actual), (expected), (label), __FILE__, __LINE__)

  json::value valueOutcome(json::value value) {
    json::object outcome;
    outcome["value"] = std::move(value);
    return outcome;
  }
  json::value errorOutcome(const rt::Reason& error) {
    json::object outcome;
    outcome["error"] = rt::reasonToString(error);
    return outcome;
  }

  std::string stringOf(const json::value& value, std::string_view key) { return std::string(*rt::getString(value, key)); }

  rt::Bytes encode(std::string_view text) { return rt::encodeUtf8(text); }

  std::shared_ptr<rt::MemoryKv> memoryKv() { return std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); }); }

  // A KV whose reads fail (`{ get: () => Promise.reject(new Error("kv down")) }`).
  class BrokenKv final : public rt::ForwardingKv {
  public:
    explicit BrokenKv(rt::Kv& wrapped) : rt::ForwardingKv(wrapped) {}
    using rt::Kv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey, rt::KvGetOptions) override {
      throw rt::JsError("kv down");
      co_return rt::KvEntryMaybe<>{};
    }
  };

  json::value ledgerStateJson(const schema::LedgerState& state) {
    json::object out;
    out["kind"] = state.kind == schema::LedgerStateKind::absent ? "absent" : state.kind == schema::LedgerStateKind::ok ? "ok" : "corrupt";
    if (state.ledger) out["ledger"] = json::value_from(*state.ledger);
    out["versionstamp"] = state.versionstamp ? json::value("set") : json::value(nullptr);
    return out;
  }

  upstream::SentinelUpstreamTrace emptyTrace() { return upstream::emptySentinelUpstreamTrace(); }

  rt::Task<void> seedChunks(std::shared_ptr<rt::MemoryKv> kv, std::string captureId, double count) {
    for (double index = 0; index < count; index += 1) {
      const rt::KvKey key{model::SENTINEL_REPLAY_CHUNK_PREFIX, captureId, index};
      const rt::KvValue value("chunk-" + rt::numberToString(index));
      auto writing = kv->set(key, value);
      co_await std::move(writing);
    }
  }

  rt::Task<json::value> remainingKeys(std::shared_ptr<rt::MemoryKv> kv) {
    rt::KvListSelector selector;
    selector.prefix = rt::KvKey{"uos_ai"};
    std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector);
    json::array keys;
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) keys.push_back(keyJson(entry->key));
    co_return json::value(std::move(keys));
  }
}

TEST("replay model: stable header text and request status key") {
  for (const json::value& item : section("stableHeaderText")) {
    EXPECT_SAME(json::value(model::stableHeaderText(rt::get(item, "headers")->as_object())), *rt::get(item, "text"), "stableHeaderText");
  }
  EXPECT_SAME(keyJson(model::requestStatusKey("req-1")), *rt::get(V(), "requestStatusKey"), "requestStatusKey");
}

TEST("retention schema: budgets, reserves, keys, charges and counters") {
  for (const json::value& item : section("budget")) {
    const json::value& bytes = *rt::get(item, "bytes");
    schema::setSentinelReplayBudgetForTest(bytes.is_null() ? std::nullopt : std::optional<double>(numberOf(bytes)));
    const double value = schema::sentinelReplayBudgetBytes();
    schema::setSentinelReplayBudgetForTest(std::nullopt);
    EXPECT_SAME(numberJson(value), *rt::get(item, "value"), "budget " + rt::jsonStringify(bytes));
  }
  for (const json::value& item : section("reserve")) {
    const double budget = numberOf(*rt::get(item, "budget"));
    EXPECT_SAME(numberJson(schema::sentinelReplayMetadataReserve(budget)), *rt::get(item, "reserve"), "reserve " + rt::jsonStringify(*rt::get(item, "budget")));
    EXPECT_SAME(numberJson(schema::sentinelReplayPayloadBudgetBytes(budget)), *rt::get(item, "payload"), "payload " + rt::jsonStringify(*rt::get(item, "budget")));
  }
  const json::value& keys = *rt::get(V(), "keys");
  EXPECT_SAME(keyJson(schema::sentinelReplayAccountingKey(1'800'000'000'000, "capture-1")), *rt::get(keys, "accounting"), "accounting key");
  model::SentinelReplayManifest keyManifest{};
  keyManifest.captured_at_ms = 5;
  keyManifest.fingerprint = std::string(64, 'f');
  keyManifest.capture_id = "c";
  EXPECT_SAME(keyJson(schema::sentinelReplayManifestKey(keyManifest)), *rt::get(keys, "manifest"), "manifest key");
  EXPECT_SAME(keyJson(schema::sentinelReplayEvictionKey(std::string(64, 'a'))), *rt::get(keys, "eviction"), "eviction key");
  EXPECT_SAME(keyJson(schema::sentinelReplayRequestStatusKey("req-2")), *rt::get(keys, "status"), "status key");

  model::SentinelReplayManifest manifest{
    .version = 1,
    .capture_id = "cap",
    .fingerprint = std::string(64, 'f'),
    .case_group_digest = std::string(64, 'e'),
    .captured_at_ms = 1000,
    .expires_at_ms = 2000,
    .algorithm = "AES-256-GCM",
    .compression = "gzip",
    .iv = "iv",
    .chunk_count = 1,
    .ciphertext_bytes = 1234,
  };
  for (const json::value& item : section("manifestCharge")) {
    const json::value& stored = *rt::get(item, "stored");
    manifest.stored_bytes = stored.is_null() ? std::nullopt : std::optional<double>(numberOf(stored));
    EXPECT_SAME(numberJson(schema::sentinelReplayManifestCharge(manifest)), *rt::get(item, "charge"), "charge " + rt::jsonStringify(stored));
  }
  manifest.stored_bytes = std::nullopt;
  manifest.ciphertext_bytes = 50'000'000;
  EXPECT_SAME(numberJson(schema::sentinelReplayManifestCharge(manifest)), *rt::get(V(), "manifestChargeLarge"), "large charge");

  for (const json::value& item : section("counter")) {
    const std::string kind = stringOf(item, "kind");
    const json::value& raw = *rt::get(item, "value");
    const json::value value = kind == "number" ? rt::canonicalNumbers(json::value(numberOf(raw))) : raw;
    // `counter(value: unknown)` on the JSON value, the stored value and (numbers) the double.
    const bool expected = rt::get(item, "counter")->as_bool();
    EXPECT_EQ(schema::counter(value), expected);
    EXPECT_EQ(schema::counter(rt::KvValue(value)), expected);
    if (kind == "number") EXPECT_EQ(schema::counter(numberOf(raw)), expected);
  }
}

TEST("retention schema: row validators, key matches and status metadata bytes") {
  for (const json::value& item : section("isAccountingRow")) {
    const json::value& value = *rt::get(item, "value");
    if (schema::isAccountingRow(rt::KvValue(value)) != rt::get(item, "result")->as_bool()) {
      rt::test::report(__FILE__, __LINE__, "isAccountingRow " + stringOf(item, "name"), false);
    }
  }
  for (const json::value& item : section("isLegacyReservation")) {
    if (schema::isLegacyReservation(rt::KvValue(*rt::get(item, "value"))) != rt::get(item, "result")->as_bool()) {
      rt::test::report(__FILE__, __LINE__, "isLegacyReservation " + stringOf(item, "name"), false);
    }
  }
  const schema::SentinelReplayAccountingRow row{
    .version = 1,
    .capture_id = "cap-1",
    .request_id = "req-1",
    .fingerprint = std::string(64, 'a'),
    .bytes = 10,
    .state = "stored",
    .fence = 1,
    .created_at_ms = 100,
    .expires_at_ms = 200,
    .status_expires_at_ms = 300,
    .stage = 0,
  };
  for (const json::value& item : section("accountingKeyMatches")) {
    rt::KvKey key;
    for (const json::value& part : rt::get(item, "key")->as_array()) {
      if (part.is_string()) key.emplace_back(std::string(part.get_string()));
      else key.emplace_back(*rt::getNumber(part));
    }
    EXPECT_EQ(schema::accountingKeyMatches(key, row), rt::get(item, "result")->as_bool());
  }
  model::SentinelReplayManifest manifest{};
  manifest.captured_at_ms = 1000;
  manifest.fingerprint = std::string(64, 'f');
  manifest.capture_id = "cap";
  for (const json::value& item : section("manifestKeyMatches")) {
    EXPECT_EQ(schema::manifestKeyMatches(keyFrom(*rt::get(item, "key")), manifest), rt::get(item, "result")->as_bool());
  }
  for (const json::value& item : section("statusMetadataBytes")) {
    const json::value& value = *rt::get(item, "value");
    const double bytes = *rt::getNumber(item, "bytes");
    const double stored = *rt::getNumber(item, "stored");
    EXPECT_EQ(schema::sentinelReplayStatusMetadataBytes(value), bytes);
    EXPECT_EQ(schema::sentinelReplayStatusMetadataBytes(rt::KvValue(value)), bytes);
    EXPECT_EQ(schema::storedStatusMetadataBytes(value), stored);
    EXPECT_EQ(schema::storedStatusMetadataBytes(rt::KvValue(value)), stored);
  }
  const json::value& undefinedBytes = *rt::get(V(), "statusMetadataBytesUndefined");
  EXPECT_EQ(schema::sentinelReplayStatusMetadataBytes(rt::KvValue()), *rt::getNumber(undefinedBytes, "bytes"));
  EXPECT_EQ(schema::storedStatusMetadataBytes(rt::KvValue()), *rt::getNumber(undefinedBytes, "stored"));
  // JSON.stringify throws for a BigInt anywhere: only the row overhead is charged.
  rt::KvObject withBigInt;
  withBigInt.set("value", rt::KvValue(rt::BigInt(1)));
  EXPECT_EQ(schema::sentinelReplayStatusMetadataBytes(rt::KvValue(withBigInt)), uos::sentinel::replay_limits::SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES);
}

ASYNC_TEST("retention schema: ledger reads, snapshots and retention status") {
  const json::array& budgets = section("ledgerBudgets");
  for (const json::value& item : section("ledger")) {
    const std::string name = stringOf(item, "name");
    const std::shared_ptr<rt::MemoryKv> kv = memoryKv();
    if (rt::get(item, "stored")->as_bool()) {
      const rt::KvValue stored(*rt::get(item, "value"));
      auto writing = kv->set(schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY, stored);
      co_await std::move(writing);
    }
    auto reading = schema::readLedger(*kv, 4242);
    const schema::LedgerState state = co_await std::move(reading);
    EXPECT_SAME(valueOutcome(ledgerStateJson(state)), *rt::get(item, "read"), name + " read");
    for (std::size_t index = 0; index < budgets.size(); ++index) {
      const json::value& budget = budgets[index];
      auto snapshotting = budget.is_null() ? schema::readSentinelReplayLedgerSnapshot(*kv) : schema::readSentinelReplayLedgerSnapshot(*kv, *rt::getNumber(budget));
      const std::optional<schema::SentinelReplayLedgerSnapshot> snapshot = co_await std::move(snapshotting);
      json::value snapshotJson(nullptr);
      if (snapshot) {
        json::object value;
        value["ledger"] = json::value_from(snapshot->ledger);
        value["versionstamp"] = snapshot->versionstamp ? json::value("set") : json::value(nullptr);
        snapshotJson = std::move(value);
      }
      EXPECT_SAME(valueOutcome(snapshotJson), rt::get(item, "snapshots")->as_array().at(index), name + " snapshot " + rt::jsonStringify(budget));
      auto statusing = budget.is_null() ? schema::readSentinelReplayRetentionStatus(*kv) : schema::readSentinelReplayRetentionStatus(*kv, *rt::getNumber(budget));
      const schema::SentinelReplayRetentionStatus status = co_await std::move(statusing);
      EXPECT_SAME(valueOutcome(json::value_from(status)), rt::get(item, "statuses")->as_array().at(index), name + " status " + rt::jsonStringify(budget));
    }
  }
  const std::shared_ptr<rt::MemoryKv> inner = memoryKv();
  BrokenKv broken(*inner);
  auto statusing = schema::readSentinelReplayRetentionStatus(broken);
  const schema::SentinelReplayRetentionStatus unavailable = co_await std::move(statusing);
  EXPECT_SAME(json::value_from(unavailable), *rt::get(V(), "statusUnavailable"), "unavailable status");
}

ASYNC_TEST("retention schema: chunk batches, fault injection and accounting commits") {
  for (const json::value& item : section("chunks")) {
    const double count = *rt::getNumber(item, "count");
    const double limit = *rt::getNumber(item, "limit");
    const bool stray = rt::get(item, "stray")->as_bool();
    const std::shared_ptr<rt::MemoryKv> kv = memoryKv();
    auto seedingA = seedChunks(kv, "cap-a", count);
    co_await std::move(seedingA);
    auto seedingB = seedChunks(kv, "cap-b", 2);
    co_await std::move(seedingB);
    if (stray) {
      const rt::KvKey strayKey{model::SENTINEL_REPLAY_CHUNK_PREFIX, "cap-a", "stray", 1};
      const rt::KvValue strayValue("stray");
      auto writing = kv->set(strayKey, strayValue);
      co_await std::move(writing);
    }
    const std::string label = "chunks " + rt::jsonStringify(item);
    auto counting = schema::countChunks(*kv, "cap-a");
    const double counted = co_await std::move(counting);
    EXPECT_SAME(rt::canonicalNumbers(json::value(counted)), *rt::get(item, "counted"), label + " counted");
    json::value deleted;
    try {
      auto deleting = schema::deleteChunkBatch(*kv, "cap-a", limit);
      const schema::DeleteChunkBatchResult result = co_await std::move(deleting);
      json::object value;
      value["deleted"] = result.deleted;
      value["remaining"] = result.remaining;
      deleted = valueOutcome(json::value(std::move(value)));
    } catch (...) {
      deleted = errorOutcome(std::current_exception());
    }
    EXPECT_SAME(deleted, *rt::get(item, "deleted"), label + " deleted");
    auto listing = remainingKeys(kv);
    const json::value remaining = co_await std::move(listing);
    EXPECT_SAME(remaining, *rt::get(item, "remaining"), label + " remaining");
  }

  {
    const std::shared_ptr<rt::MemoryKv> kv = memoryKv();
    auto seeding = seedChunks(kv, "cap-a", 3);
    co_await std::move(seeding);
    auto deletes = std::make_shared<int>(0);
    schema::setSentinelReplayRetentionFaultsForTest([deletes](const std::string& kind) { return kind == "delete" && ++*deletes == 2; });
    json::value result;
    try {
      auto deleting = schema::deleteChunkBatch(*kv, "cap-a", 3);
      (void)co_await std::move(deleting);
      result = valueOutcome(json::value("unexpected"));
    } catch (...) {
      result = errorOutcome(std::current_exception());
    }
    schema::setSentinelReplayRetentionFaultsForTest({});
    const json::value& expected = *rt::get(V(), "chunkFault");
    EXPECT_SAME(result, *rt::get(expected, "result"), "chunk fault result");
    auto listing = remainingKeys(kv);
    const json::value remaining = co_await std::move(listing);
    EXPECT_SAME(remaining, *rt::get(expected, "remaining"), "chunk fault remaining");
  }

  const rt::KvKey accountingKey = schema::sentinelReplayAccountingKey(100, "cap-1");
  const schema::SentinelReplayAccountingRow accountingRow{
    .version = 1,
    .capture_id = "cap-1",
    .request_id = "req-1",
    .fingerprint = std::string(64, 'a'),
    .bytes = 10,
    .state = "stored",
    .fence = 1,
    .created_at_ms = 100,
    .expires_at_ms = 200,
    .status_expires_at_ms = 300,
    .stage = 0,
  };
  const json::array& mutations = section("mutations");
  const std::vector<std::pair<std::string, std::string>> plans = {
    {"absent ledger", "row"}, {"valid ledger", "row"}, {"corrupt ledger", "row"}, {"stale row", "stale"}, {"absent row expected", "absent"}, {"injected commit", "row"},
  };
  json::value validLedger;
  for (const json::value& ledgerCase : section("ledger")) {
    if (stringOf(ledgerCase, "name") == "valid") validLedger = *rt::get(ledgerCase, "value");
  }
  for (std::size_t index = 0; index < plans.size(); ++index) {
    const auto& [name, rowStamp] = plans[index];
    const json::value& expected = mutations.at(index);
    EXPECT_EQ(stringOf(expected, "name"), name);
    const std::shared_ptr<rt::MemoryKv> kv = memoryKv();
    if (name != "absent ledger") {
      const rt::KvValue ledgerValue = name == "corrupt ledger" ? rt::KvValue(rt::jsonParse(R"({"version":9})")) : rt::KvValue(validLedger);
      auto writing = kv->set(schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue);
      co_await std::move(writing);
    }
    if (rowStamp != "absent") {
      const rt::KvValue rowValue(json::value_from(accountingRow));
      auto writing = kv->set(accountingKey, rowValue);
      co_await std::move(writing);
    }
    auto reading = kv->get(accountingKey);
    const rt::KvEntryMaybe<> current = co_await std::move(reading);
    const std::optional<std::string> versionstamp = rowStamp == "stale" ? std::optional<std::string>("00000000000000ff0000") : current.versionstamp;
    if (name == "injected commit") schema::setSentinelReplayRetentionFaultsForTest([](const std::string& kind) { return kind == "commit"; });
    json::value result;
    try {
      const schema::LedgerMutation mutate = [](const schema::SentinelReplayBudgetLedger& ledger) {
        schema::SentinelReplayBudgetLedger next = ledger;
        next.records = ledger.records + 1;
        next.stored_bytes = ledger.stored_bytes + 10;
        return next;
      };
      schema::SentinelReplayAccountingRow staged = accountingRow;
      staged.stage = 1;
      const rt::KvValue stagedValue(json::value_from(staged));
      const schema::AtomicRowApplication applyRow = [accountingKey, stagedValue](rt::AtomicOperation& operation) -> rt::AtomicOperation& {
        return operation.set(accountingKey, stagedValue);
      };
      auto committing = schema::commitAccountingMutation(*kv, accountingKey, versionstamp, mutate, applyRow, 5000);
      const bool committed = co_await std::move(committing);
      result = valueOutcome(json::value(committed));
    } catch (...) {
      result = errorOutcome(std::current_exception());
    }
    schema::setSentinelReplayRetentionFaultsForTest({});
    EXPECT_SAME(result, *rt::get(expected, "result"), name + " result");
    auto ledgerReading = kv->get(schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
    const rt::KvEntryMaybe<> ledgerAfter = co_await std::move(ledgerReading);
    auto rowReading = kv->get(accountingKey);
    const rt::KvEntryMaybe<> rowAfter = co_await std::move(rowReading);
    EXPECT_SAME(ledgerAfter.value.toJson(), *rt::get(expected, "ledgerAfter"), name + " ledger");
    EXPECT_SAME(rowAfter.value.toJson(), *rt::get(expected, "rowAfter"), name + " row");
  }
}

TEST("replay observation: status rows, request ids and bounded kinds") {
  for (const json::value& item : section("statusRows")) {
    if (observation::isSentinelReplayCaptureStatusRow(rt::KvValue(*rt::get(item, "value"))) != rt::get(item, "result")->as_bool()) {
      rt::test::report(__FILE__, __LINE__, "isSentinelReplayCaptureStatusRow " + stringOf(item, "name"), false);
    }
  }
  for (const json::value& item : section("requestIds")) {
    EXPECT_EQ(observation::isSentinelReplayRequestId(rt::KvValue(*rt::get(item, "value"))), rt::get(item, "result")->as_bool());
  }
  for (const json::value& item : section("boundedFailureKind")) {
    const json::value& value = *rt::get(item, "value");
    const std::optional<std::string> kind = observation::boundedFailureKind(value);
    const std::optional<std::string> param = observation::boundedErrorParam(value);
    EXPECT_SAME(kind ? json::value(*kind) : json::value(nullptr), *rt::get(item, "kind"), "kind " + rt::jsonStringify(value));
    EXPECT_SAME(param ? json::value(*param) : json::value(nullptr), *rt::get(item, "param"), "param " + rt::jsonStringify(value));
  }
}

TEST("replay observation: SSE inspector streams") {
  for (const json::value& item : section("sse")) {
    const observation::SentinelSseInspector inspector = observation::createSentinelSseInspector();
    for (const json::value& chunk : rt::get(item, "chunks")->as_array()) inspector.push(encode(std::string(chunk.get_string())));
    const model::SentinelClientBodyObservation result = inspector.finish(stringOf(item, "termination"));
    EXPECT_SAME(json::value_from(result), *rt::get(item, "result"), "sse " + rt::jsonStringify(item));
  }
}

TEST("replay observation: buffered responses") {
  for (const json::value& item : section("buffered")) {
    const model::SentinelClientBodyObservation result =
      observation::inspectSentinelBufferedResponse(*rt::getNumber(item, "status"), stringOf(item, "contentType"), encode(stringOf(item, "body")));
    EXPECT_SAME(json::value_from(result), *rt::get(item, "result"), "buffered " + rt::jsonStringify(item));
  }
}

TEST("replay observation: client observation resolution and persistence decisions") {
  for (const json::value& item : section("resolve")) {
    const model::SentinelFailureObservation internal = json::value_to<model::SentinelFailureObservation>(*rt::get(item, "internal"));
    const json::value& body = *rt::get(item, "body");
    model::SentinelClientFailureObservation client;
    if (body.is_string()) client = observation::resolveSentinelClientFailureObservation(internal);
    else if (body.is_null()) client = observation::resolveSentinelClientFailureObservation(internal, static_cast<const model::SentinelClientBodyObservation*>(nullptr));
    else client = observation::resolveSentinelClientFailureObservation(internal, json::value_to<model::SentinelClientBodyObservation>(body));
    const std::string label = rt::jsonStringify(item);
    EXPECT_SAME(json::value_from(client), *rt::get(item, "client"), "client " + label);
    EXPECT_SAME(json::value(observation::shouldPersistSentinelReplay(internal)), *rt::get(item, "persist"), "persist " + label);
    EXPECT_SAME(json::value(observation::shouldPersistSentinelReplay(internal, client)), *rt::get(item, "persistWith"), "persistWith " + label);
    EXPECT_SAME(json::value(observation::shouldSignalSentinelIncident(internal, client)), *rt::get(item, "signal"), "signal " + label);
    EXPECT_SAME(json::value(observation::sentinelFailureSignature(client)), *rt::get(item, "signature"), "signature " + label);
  }
}

TEST("replay observation: normalized compatibility headers") {
  const rt::Headers headers{{"Accept", " text/event-stream "}, {"User-Agent", "codex/1"}, {"OpenAI-Beta", "  "},
                            {"Originator", "x"},             {"X-Forwarded-For", "1.2.3.4"}, {"x-stainless-os", "Linux"}};
  EXPECT_SAME(json::value(observation::normalizeSentinelCompatibilityHeaders(headers)), *rt::get(V(), "normalizedHeaders"), "normalized headers");
}

ASYNC_TEST("replay envelope: fingerprints, digests, reasons, settings and keys") {
  const json::value& expected = *rt::get(V(), "envelope");
  const rt::Bytes key(32, 0x2a);
  const model::AcceptedSentinelReplayInput input{
    .endpoint = "/v1/chat/completions?x=1",
    .method = "POST",
    .body = std::make_shared<rt::Bytes>(encode(R"({"model":"gpt","messages":[]})")),
    .content_type = std::nullopt,
    .compatibility_headers = rt::jsonParse(R"({"originator":"o","accept":"application/json"})").as_object(),
    .request_id = "r",
    .git_sha = "g",
    .deno_revision = "d",
  };
  // fingerprintParts canonicalizes the trace it is given without validating it (the recording passed this
  // object as is), so it is built directly.
  const upstream::SentinelUpstreamTrace trace{
    .version = 2,
    .attempts = {upstream::SentinelUpstreamAttempt{
      .provider = "openrouter",
      .status = 200,
      .content_type = "text/event-stream",
      .chunks_base64 = {"ZGF0YQ=="},
      .terminal = "eof",
      .headers = rt::jsonParse(R"({"content-type":"text/event-stream"})").as_object(),
      .headers_truncated = false,
      .chunk_times_ms = std::vector<double>{3},
      .started_at_ms = 0,
      .headers_at_ms = 1.0,
      .ended_at_ms = 4.0,
    }},
    .attempts_truncated = false,
    .bytes_truncated = false,
    .chunks_truncated = false,
  };
  const std::string fingerprintPurpose = "fingerprint";
  const std::string caseGroupPurpose = "case-group";
  auto fingerprinting = envelope::hmacHex(key, fingerprintPurpose, envelope::fingerprintParts(input, "sig", "fingerprint", trace));
  const std::string fingerprint = co_await std::move(fingerprinting);
  EXPECT_EQ(fingerprint, stringOf(expected, "fingerprint"));
  auto emptyFingerprinting = envelope::hmacHex(key, fingerprintPurpose, envelope::fingerprintParts(input, "sig", "fingerprint", emptyTrace()));
  const std::string fingerprintEmpty = co_await std::move(emptyFingerprinting);
  EXPECT_EQ(fingerprintEmpty, stringOf(expected, "fingerprintEmpty"));
  auto noneFingerprinting = envelope::hmacHex(key, fingerprintPurpose, envelope::fingerprintParts(input, "sig", "fingerprint"));
  const std::string fingerprintNone = co_await std::move(noneFingerprinting);
  EXPECT_EQ(fingerprintNone, stringOf(expected, "fingerprintNone"));
  auto grouping = envelope::hmacHex(key, caseGroupPurpose, envelope::fingerprintParts(input, "sig", "case-group"));
  const std::string caseGroup = co_await std::move(grouping);
  EXPECT_EQ(caseGroup, stringOf(expected, "caseGroup"));
  auto digesting = envelope::ciphertextDigest(encode("ciphertext"));
  const std::string digest = co_await std::move(digesting);
  EXPECT_EQ(digest, stringOf(expected, "digest"));
  json::array aad;
  for (const std::uint8_t byte : envelope::encryptionAdditionalData(std::string(64, 'f'))) aad.emplace_back(byte);
  EXPECT_SAME(json::value(aad), *rt::get(expected, "aad"), "aad");
  model::SentinelClientFailureObservation client{};
  client.status = 502;
  client.terminal_body_base64 = "eA==";
  json::array reasons;
  for (const std::string& reason : envelope::replayUnavailableReasons(trace, client)) reasons.emplace_back(reason);
  EXPECT_SAME(json::value(reasons), *rt::get(expected, "reasons"), "reasons");

  for (const json::value& item : section("settings")) {
    const model::SentinelReplaySettings observed = envelope::requestSettingsFromBody(encode(stringOf(item, "body")), "route", std::nullopt);
    EXPECT_SAME(json::value_from(observed), *rt::get(item, "observed"), "settings " + rt::jsonStringify(*rt::get(item, "body")));
  }
  for (const json::value& item : section("keyDecode")) {
    const std::optional<rt::Bytes> decoded = envelope::decodeSentinelReplayKey(stringOf(item, "raw"));
    json::value actual(nullptr);
    if (decoded) {
      json::array bytes;
      for (const std::uint8_t byte : *decoded) bytes.emplace_back(byte);
      actual = std::move(bytes);
    }
    EXPECT_SAME(actual, *rt::get(item, "decoded"), "key " + stringOf(item, "raw"));
  }
}

ASYNC_TEST("replay envelope: gunzip inflates and rejects exactly what Bun's DecompressionStream does") {
  EXPECT_EQ(gunzipVectors().size(), std::size_t{20});
  for (const json::value& item : gunzipVectors()) {
    const std::string name = stringOf(item, "name");
    rt::Bytes input = rt::fromHex(stringOf(item, "input"));
    auto inflating = envelope::gunzip(std::move(input));
    rt::Reason failure;
    rt::Bytes output;
    try {
      output = co_await std::move(inflating);
    } catch (...) {
      failure = std::current_exception();
    }
    json::object expected = item.as_object();
    expected.erase("name");
    expected.erase("input");
    EXPECT_SAME(gunzipOutcome(failure, output), json::value(std::move(expected)), "gunzip " + name);
  }
}
