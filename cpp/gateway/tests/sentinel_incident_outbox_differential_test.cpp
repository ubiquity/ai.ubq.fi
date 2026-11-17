// Differential cases for src/sentinel/incident-outbox.ts beyond the ported bun cases, recorded from
// the TypeScript under Bun 1.4.0: the canonical classification text and its fingerprint, the exact
// index rows (new group, merge with sanitized classification, out-of-order merge, second group),
// and the validators over hand-written values.
#include "uos/sentinel/incident_outbox.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "sentinel_incident_outbox_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::sentinel::incident_outbox;

namespace {
  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(kIncidentOutboxVectors);
    return parsed;
  }
  const json::value& at(const json::value& value, std::string_view key) {
    static const json::value null;
    const json::value* member = rt::get(value, key);
    return member != nullptr ? *member : null;
  }
  std::optional<std::string> textOrNull(const json::value& value) {
    if (!value.is_string()) return std::nullopt;
    return std::string(value.as_string());
  }

  SentinelIncidentIndexObservation observationOf(const json::value& value) {
    return {.status = *rt::getNumber(value, "status"), .stream = at(value, "stream").as_bool(), .completed = at(value, "completed").as_bool(),
            .framing_valid = at(value, "framing_valid").as_bool(), .terminal_type = textOrNull(at(value, "terminal_type"))};
  }

  SentinelIncidentIndexObservationInput recordInputOf(const json::value& value) {
    SentinelIncidentIndexObservationInput input{.endpoint = std::string(at(value, "endpoint").as_string()), .method = std::string(at(value, "method").as_string()),
                                                .gitSha = std::string(at(value, "gitSha").as_string()), .observedAtMs = *rt::getNumber(value, "observedAtMs"),
                                                .observation = observationOf(at(value, "observation"))};
    const json::value& classification = at(value, "classification");
    if (classification.is_object())
      input.classification = SentinelIncidentIndexClassification{.provider = textOrNull(at(classification, "provider")), .model = textOrNull(at(classification, "model")),
                                                                 .reasoning = textOrNull(at(classification, "reasoning")),
                                                                 .failure_kind = textOrNull(at(classification, "failure_kind"))};
    return input;
  }

  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    std::fprintf(stderr, "    %s\n      actual:   %s\n      expected: %s\n", label.c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(expected).c_str());
    return false;
  }
}

ASYNC_TEST("sentinelIncidentClassification and sentinelIncidentFingerprint match the oracle") {
  int cases = 0;
  for (const json::value& vector : at(vectors(), "classifications").as_array()) {
    const json::value& input = at(vector, "input");
    const SentinelIncidentClassificationInput classified{.endpoint = std::string(at(input, "endpoint").as_string()), .method = std::string(at(input, "method").as_string()),
                                                         .observation = observationOf(at(input, "observation"))};
    EXPECT_EQ(sentinelIncidentClassification(classified), std::string(at(vector, "classification").as_string()));
    const std::string fingerprint = co_await sentinelIncidentFingerprint(classified);
    EXPECT_EQ(fingerprint, std::string(at(vector, "fingerprint").as_string()));
    ++cases;
  }
  EXPECT_EQ(cases, 6);
}

ASYNC_TEST("recorded index rows match the oracle exactly") {
  rt::MemoryKv kv([] { return rt::dateNow(); });
  int cases = 0;
  for (const json::value& vector : at(vectors(), "records").as_array()) {
    SentinelIncidentDependencies dependencies;
    const std::string uuid(at(vector, "uuid").as_string());
    dependencies.randomUuid = [uuid] { return uuid; };
    const rt::KvEntry<SentinelIncidentIndexRow> entry = co_await recordSentinelIncidentIndexObservation(kv, recordInputOf(at(vector, "input")), dependencies);
    EXPECT(same(json::value_from(entry.value), at(vector, "value"), "record " + std::to_string(cases)));
    json::array key;
    for (const rt::KvKeyPart& part : entry.key) key.push_back(json::value(part.string()));
    EXPECT(same(key, at(vector, "key"), "key " + std::to_string(cases)));
    // The stored row is the returned row.
    const rt::KvEntryMaybe<> stored = co_await kv.get(entry.key);
    EXPECT(same(stored.value.toJson(), at(vector, "value"), "stored " + std::to_string(cases)));
    ++cases;
  }
  EXPECT_EQ(cases, 4);
  kv.close();
}

TEST("incident outbox validators match the oracle") {
  const json::value& validators = at(vectors(), "validators");
  const auto check = [&validators](std::string_view group, bool (*validator)(const rt::KvValue&)) {
    int index = 0;
    for (const json::value& pair : at(validators, group).as_array()) {
      const bool actual = validator(rt::KvValue(pair.as_array()[0]));
      if (actual != pair.as_array()[1].as_bool()) std::fprintf(stderr, "    %s[%d]: %s\n", std::string(group).c_str(), index, rt::jsonStringify(pair.as_array()[0]).c_str());
      EXPECT_EQ(actual, pair.as_array()[1].as_bool());
      ++index;
    }
    return index;
  };
  EXPECT_EQ(check("ids", isSentinelIncidentId), 5);
  EXPECT_EQ(check("terminals", isSentinelIncidentTerminalCategory), 5);
  EXPECT_EQ(check("endpoints", isSentinelIncidentIndexEndpoint), 5);
  EXPECT_EQ(check("refs", isSentinelIncidentIndexEvidenceRef), 5);
  EXPECT_EQ(check("rows", isSentinelIncidentIndexRow), 11);
  EXPECT_EQ(check("events", isSentinelIncidentFailureEvent), 5);
  EXPECT_EQ(check("references", isSentinelIncidentCaptureReference), 3);
}

// listSentinelIncidentIndexRows reads `entry.value.fingerprint` before it validates the row: a
// stored null or undefined fails with JavaScriptCore's TypeError instead of the record error.
ASYNC_TEST("a stored null or undefined index value throws the TypeError the TS throws") {
  rt::MemoryKv kv([] { return rt::dateNow(); });
  co_await kv.set(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, std::string(64, 'a')}, rt::KvValue(nullptr));
  EXPECT_REJECTS_MESSAGE(listSentinelIncidentIndexRows(kv), rt::TypeError, "null is not an object (evaluating 'entry.value.fingerprint')");
  co_await kv.set(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, std::string(64, 'a')}, rt::KvValue());
  EXPECT_REJECTS_MESSAGE(listSentinelIncidentIndexRows(kv), rt::TypeError, "undefined is not an object (evaluating 'entry.value.fingerprint')");
  co_await kv.set(rt::KvKey{SENTINEL_INCIDENT_INDEX_PREFIX, std::string(64, 'a')}, rt::KvValue(5));
  EXPECT_REJECTS_MESSAGE(listSentinelIncidentIndexRows(kv), rt::JsError, "Sentinel incident index record is invalid");
  kv.close();
}
