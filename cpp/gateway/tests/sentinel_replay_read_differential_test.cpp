// Links: tests/sentinel_replay_read_differential_test.cpp src/sentinel/replay_read.cpp src/sentinel/incident_outbox.cpp
//        src/sentinel/replay_envelope.cpp src/sentinel/replay_observation.cpp src/sentinel/replay_retention_schema.cpp
//        src/sentinel/replay_model.cpp src/sentinel/upstream_capture.cpp src/sentinel/replay_limits.cpp src/request.cpp
//        src/config.cpp src/release.cpp src/kv.cpp src/utils.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/value_compatibility.cpp
// Differential cases for src/sentinel/replay-read.ts over rows written directly. The TS tests that read stored
// captures (tests/sentinel-replay-read-coverage.test.ts: "chunk reads are paged ...", "the time-range export ...",
// "the incident export ...", "a request id resolves ..."; tests/sentinel-replay-coverage.test.ts: "a request id with no
// row reads as unknown ...") write them through src/sentinel/replay-store.ts, not ported yet; this file runs their
// replay-read steps, and more, over the same rows written with kv.set.
//
// sentinel_replay_read_differential_vectors.inc was recorded from Bun 1.4.0 running the TS module on the real native
// :memory: KV: the envelopes it sealed (Bun's gzip, so the C++ reader decrypts and inflates TS-written captures) and
// the transcript of every step, `{ step, ok }` or `{ step, error: String(error) }`. Here the same steps run in the same
// order over rt::MemoryKv, and each transcript entry must serialize to the same JSON text. List cursors are the SDK's
// and compared literally; versionstamps never appear.
//
// Known divergence, asserted here: a stored status row's undeclared members (`extra`) are not carried into the C++
// SentinelReplayCaptureStatusRow, while the TS returns the stored object itself.
#include "uos/sentinel/incident_outbox.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"
#include "uos/utils.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace model = uos::sentinel::replay_model;
  namespace outbox = uos::sentinel::incident_outbox;
  namespace read = uos::sentinel::replay_read;
  namespace schema = uos::sentinel::replay_retention_schema;
  namespace utils = uos::utils;

#include "sentinel_replay_read_differential_vectors.inc"

  constexpr double NOW = 1'800'000'000'000;
  const rt::Bytes KEY_BYTES(32, 0x33);
  const std::string INCIDENT_UUID = "12345678-1234-4abc-8def-1234567890ab";

  double kvClock() { return rt::dateNow(); }

  const json::object& oracle() {
    static const json::object parsed = rt::jsonParse(READ_ORACLE_JSON).as_object();
    return parsed;
  }

  model::ExportedSentinelReplayCapture sealed(std::string_view name) {
    return json::value_to<model::ExportedSentinelReplayCapture>(oracle().at("sealed").at(name));
  }

  rt::KvKey manifestKey(const model::SentinelReplayManifest& manifest) {
    return rt::KvKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, manifest.captured_at_ms, manifest.fingerprint, manifest.capture_id};
  }
  rt::KvKey chunkKey(const std::string& captureId, double index) { return rt::KvKey{model::SENTINEL_REPLAY_CHUNK_PREFIX, captureId, index}; }
  rt::KvValue value(const json::value& json) { return rt::KvValue(json); }
  rt::KvValue manifestValue(const model::SentinelReplayManifest& manifest) { return rt::KvValue(json::value_from(manifest)); }
  // `[...key]` as a stored array.
  rt::KvValue keyValue(const rt::KvKey& key) {
    rt::KvArray parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(std::visit([](const auto& item) { return rt::KvValue(item); }, part.value));
    return rt::KvValue(std::move(parts));
  }

  // `store(kv, sealed, extra)`: the manifest row (extra members after the manifest's own) and its chunks.
  rt::Task<void> storeTask(rt::Kv& kv, model::ExportedSentinelReplayCapture capture, json::object extra) {
    json::object manifest = json::value_from(capture.manifest).as_object();
    for (const auto& member : extra) rt::objectDefine(manifest, member.key(), member.value());
    const rt::KvKey key = manifestKey(capture.manifest);
    const rt::KvValue stored(manifest);
    co_await kv.set(key, stored);
    for (std::size_t index = 0; index < capture.chunks.size(); ++index) {
      const rt::KvKey chunk = chunkKey(capture.manifest.capture_id, static_cast<double>(index));
      const rt::KvValue bytes(rt::KvBytes(utils::base64UrlDecode(capture.chunks[index])));
      co_await kv.set(chunk, bytes);
    }
  }
  rt::Task<void> store(rt::Kv& kv, const model::ExportedSentinelReplayCapture& capture, const json::object& extra = {}) {
    return storeTask(kv, capture, extra);
  }

  json::value keyJson(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) parts.emplace_back(json::string(part.string()));
      else if (part.isNumber()) parts.push_back(rt::makeNumber(part.number()));
      else if (part.isBool()) parts.emplace_back(part.boolean());
      else throw rt::JsError("unexpected key part in a transcript");
    }
    return parts;
  }

  json::value captureJson(const model::ExportedSentinelReplayCapture& capture) {
    json::object out;
    out["manifest"] = json::value_from(capture.manifest);
    json::array chunks;
    for (const std::string& chunk : capture.chunks) chunks.emplace_back(json::string(chunk));
    out["chunks"] = std::move(chunks);
    return out;
  }

  json::value pageJson(const read::EncryptedSentinelReplayPage& page) {
    json::array captures;
    for (const model::ExportedSentinelReplayCapture& capture : page.captures) captures.push_back(captureJson(capture));
    json::object out;
    out["captures"] = std::move(captures);
    out["cursor"] = json::string(page.cursor);
    return out;
  }

  json::value textOrNull(const std::optional<std::string>& text) { return text ? json::value(json::string(*text)) : json::value(nullptr); }
  json::value numberOrNull(const std::optional<double>& number) { return number ? rt::makeNumber(*number) : json::value(nullptr); }

  json::value statusJson(const model::SentinelReplayCaptureStatusRow& row) {
    json::object out;
    out["version"] = rt::makeNumber(row.version);
    out["request_id"] = json::string(row.request_id);
    out["status"] = json::string(row.status);
    out["reason"] = textOrNull(row.reason);
    out["captured_at_ms"] = rt::makeNumber(row.captured_at_ms);
    out["manifest_key"] = row.manifest_key ? keyJson(*row.manifest_key) : json::value(nullptr);
    out["fingerprint"] = textOrNull(row.fingerprint);
    out["expires_at_ms"] = numberOrNull(row.expires_at_ms);
    return out;
  }

  json::value requestExportJson(const read::EncryptedSentinelRequestReplays& result) {
    json::array captures;
    for (const model::ExportedSentinelReplayCapture& capture : result.captures) captures.push_back(captureJson(capture));
    json::object out;
    out["captures"] = std::move(captures);
    out["status"] = statusJson(result.status);
    return out;
  }

  rt::Task<json::value> plainJson(model::ExportedSentinelReplayCapture capture) {
    auto decrypting = read::decryptExportedSentinelReplay(std::move(capture), KEY_BYTES);
    const model::SentinelReplayPlaintext plaintext = co_await std::move(decrypting);
    json::object out;
    out["request_id"] = json::string(plaintext.request_id);
    out["endpoint"] = json::string(plaintext.endpoint);
    out["method"] = json::string(plaintext.method);
    out["captured_at_ms"] = rt::makeNumber(plaintext.captured_at_ms);
    out["body"] = json::string(rt::decodeUtf8(*plaintext.body));
    co_return json::value(std::move(out));
  }

  json::value chunkShape(const std::vector<rt::Bytes>& chunks) {
    json::array shapes;
    for (const rt::Bytes& chunk : chunks) {
      json::array shape;
      shape.push_back(rt::makeNumber(static_cast<double>(chunk.size())));
      shape.push_back(chunk.empty() ? json::value(nullptr) : rt::makeNumber(static_cast<double>(chunk.front())));
      shape.push_back(chunk.empty() ? json::value(nullptr) : rt::makeNumber(static_cast<double>(chunk.back())));
      shapes.push_back(std::move(shape));
    }
    return shapes;
  }

  // The transcript, compared entry by entry with the recorded one as it is built.
  class Transcript {
  public:
    rt::Task<void> record(std::string step, rt::Task<json::value> task) {
      json::object entry;
      entry["step"] = json::string(step);
      try {
        json::value result = co_await std::move(task);
        entry["ok"] = std::move(result);
      } catch (const rt::JsError& error) {
        entry["error"] = json::string(error.toString());
      }
      compare(json::value(std::move(entry)));
    }
    void finish() {
      if (index_ != expected().size()) rt::test::report(__FILE__, __LINE__, "ran " + std::to_string(index_) + " of " + std::to_string(expected().size()) + " recorded steps", false);
    }

  private:
    static const json::array& expected() { return oracle().at("transcript").as_array(); }
    void compare(const json::value& actual) {
      if (index_ >= expected().size()) {
        rt::test::report(__FILE__, __LINE__, "unrecorded step " + rt::jsonStringify(actual), false);
        return;
      }
      json::value wanted = expected().at(index_++);
      const std::string step(wanted.at("step").as_string());
      // Known divergence: the C++ status row carries only its declared members.
      if (step == "status live" || step == "status expired") {
        json::object& ok = wanted.at("ok").as_object();
        if (!ok.contains("extra")) rt::test::report(__FILE__, __LINE__, "the TS row was expected to keep its extra member", false);
        rt::objectDelete(ok, "extra");
      }
      const std::string actualText = rt::jsonStringify(actual);
      const std::string wantedText = rt::jsonStringify(wanted);
      if (actualText != wantedText) {
        rt::test::report(__FILE__, __LINE__, "step " + step + "\n      actual:   " + actualText + "\n      expected: " + wantedText, false);
      }
    }
    std::size_t index_ = 0;
  };

#define RECORD(step, task)                                                \
  do {                                                                    \
    auto recorded_ = (task);                                              \
    auto recording_ = transcript.record((step), std::move(recorded_));    \
    co_await std::move(recording_);                                       \
  } while (false)

  rt::Task<json::value> rangeStep(rt::Kv& kv, read::ListEncryptedSentinelReplaysOptions options) {
    auto listing = read::listEncryptedSentinelReplays(kv, std::move(options));
    const read::EncryptedSentinelReplayPage page = co_await std::move(listing);
    co_return pageJson(page);
  }
  rt::Task<json::value> range(rt::Kv& kv, const read::ListEncryptedSentinelReplaysOptions& options) { return rangeStep(kv, options); }

  rt::Task<json::value> incidentStep(rt::Kv& kv, read::ListEncryptedSentinelIncidentReplaysOptions options) {
    auto listing = read::listEncryptedSentinelIncidentReplays(kv, std::move(options));
    const read::EncryptedSentinelReplayPage page = co_await std::move(listing);
    co_return pageJson(page);
  }
  rt::Task<json::value> incidentPage(rt::Kv& kv, const read::ListEncryptedSentinelIncidentReplaysOptions& options) { return incidentStep(kv, options); }

  rt::Task<json::value> status(rt::Kv& kv, std::string requestId, double nowMs) {
    auto reading = read::readSentinelReplayCaptureStatus(kv, std::move(requestId), nowMs);
    const model::SentinelReplayCaptureStatusRow row = co_await std::move(reading);
    co_return statusJson(row);
  }

  rt::Task<json::value> requestExport(rt::Kv& kv, std::string requestId, double nowMs) {
    auto listing = read::listEncryptedSentinelReplaysByRequestId(kv, std::move(requestId), nowMs);
    const read::EncryptedSentinelRequestReplays result = co_await std::move(listing);
    co_return requestExportJson(result);
  }

  rt::Task<json::value> chunksStep(rt::Kv& kv, model::SentinelReplayManifest manifest) {
    auto reading = read::getChunks(kv, std::move(manifest));
    const std::vector<rt::Bytes> chunks = co_await std::move(reading);
    co_return chunkShape(chunks);
  }
  rt::Task<json::value> chunks(rt::Kv& kv, const model::SentinelReplayManifest& manifest) { return chunksStep(kv, manifest); }

  rt::Task<json::value> resultOf(json::value result) { co_return result; }

  json::object object(std::initializer_list<std::pair<std::string_view, json::value>> members) {
    json::object out;
    for (const auto& [key, member] : members) rt::objectDefine(out, key, member);
    return out;
  }
  json::value text(std::string value) { return json::value(json::string(value)); }
}

ASYNC_TEST("replay-read over directly written rows matches the Bun transcript") {
  Transcript transcript;
  const model::ExportedSentinelReplayCapture first = sealed("first");
  const model::ExportedSentinelReplayCapture second = sealed("second");
  const model::ExportedSentinelReplayCapture incident = sealed("incident");
  const model::ExportedSentinelReplayCapture byRequest = sealed("byRequest");

  // ---- the time-range export ----
  {
    rt::MemoryKv kv(kvClock);
    const read::ListEncryptedSentinelReplaysOptions window{.afterMs = 0, .beforeMs = NOW + 10'000};
    const std::vector<double> afterValues{-1.0, 1.5};
    for (const double afterMs : afterValues) {
      read::ListEncryptedSentinelReplaysOptions options = window;
      options.afterMs = afterMs;
      RECORD("range start " + rt::numberToString(afterMs), range(kv, options));
    }
    const std::vector<double> beforeValues{-1.0, 1.5, rt::Number::MAX_SAFE_INTEGER};
    for (const double beforeMs : beforeValues) {
      read::ListEncryptedSentinelReplaysOptions options = window;
      options.beforeMs = beforeMs;
      RECORD("range end " + rt::numberToString(beforeMs), range(kv, options));
    }
    read::ListEncryptedSentinelReplaysOptions limitTwo = window;
    limitTwo.limit = 2;
    RECORD("range limit 2", range(kv, limitTwo));
    read::ListEncryptedSentinelReplaysOptions limitOne = window;
    limitOne.limit = 1;
    RECORD("range limit 1", range(kv, limitOne));
    const std::vector<std::string> badCursors{std::string(), std::string(2'049, 'a'), std::string("has spaces")};
    for (const std::string& cursor : badCursors) {
      read::ListEncryptedSentinelReplaysOptions options = window;
      options.cursor = cursor;
      RECORD("range cursor " + std::to_string(cursor.size()), range(kv, options));
    }
    read::ListEncryptedSentinelReplaysOptions notACursor = window;
    notACursor.cursor = "abc";
    RECORD("range cursor not a cursor", range(kv, notACursor));
    RECORD("range empty", range(kv, window));
    co_await store(kv, first);
    const json::object secondExtra = object({{"stored_bytes", 4'096}, {"request_id", "range-request-2"}});
    co_await store(kv, second, secondExtra);
    std::string cursor;
    RECORD("range page one", rt::async([&]() -> rt::Task<json::value> {
             auto listing = read::listEncryptedSentinelReplays(kv, window);
             const read::EncryptedSentinelReplayPage page = co_await std::move(listing);
             cursor = page.cursor;
             auto decrypting = plainJson(page.captures.at(0));
             json::value plaintext = co_await std::move(decrypting);
             co_return json::value(object({{"page", pageJson(page)}, {"plaintext", plaintext}}));
           }));
    RECORD("range page two", rt::async([&]() -> rt::Task<json::value> {
             read::ListEncryptedSentinelReplaysOptions options = window;
             options.cursor = cursor;
             auto listing = read::listEncryptedSentinelReplays(kv, options);
             const read::EncryptedSentinelReplayPage page = co_await std::move(listing);
             cursor = page.cursor;
             auto decrypting = plainJson(page.captures.at(0));
             json::value plaintext = co_await std::move(decrypting);
             co_return json::value(object({{"page", pageJson(page)}, {"plaintext", plaintext}}));
           }));
    read::ListEncryptedSentinelReplaysOptions third = window;
    third.cursor = cursor;
    RECORD("range page three", range(kv, third));
    RECORD("range before the first capture", range(kv, read::ListEncryptedSentinelReplaysOptions{.afterMs = 0, .beforeMs = NOW - 1}));
    RECORD("range window between captures", range(kv, read::ListEncryptedSentinelReplaysOptions{.afterMs = NOW + 1, .beforeMs = NOW + 999}));
    RECORD("range window at the second capture", range(kv, read::ListEncryptedSentinelReplaysOptions{.afterMs = NOW + 1'000, .beforeMs = NOW + 1'000}));
    const rt::KvKey misplaced{model::SENTINEL_REPLAY_MANIFEST_PREFIX, NOW + 7, second.manifest.fingerprint, "capture-range-2"};
    co_await kv.set(misplaced, manifestValue(second.manifest));
    RECORD("range misplaced manifest", range(kv, read::ListEncryptedSentinelReplaysOptions{.afterMs = NOW + 5, .beforeMs = NOW + 10'000}));
    co_await kv.delete_(misplaced);
    const rt::KvValue versionTwo = value(object({{"version", 2}}));
    co_await kv.set(manifestKey(second.manifest), versionTwo);
    RECORD("range corrupt manifest", range(kv, read::ListEncryptedSentinelReplaysOptions{.afterMs = NOW + 500, .beforeMs = NOW + 10'000}));
    co_await kv.set(manifestKey(second.manifest), manifestValue(second.manifest));
    const rt::KvKey shortKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, NOW + 2'000, "x"};
    const rt::KvValue notAManifest = value(object({{"not", "a manifest"}}));
    co_await kv.set(shortKey, notAManifest);
    RECORD("range short key", range(kv, read::ListEncryptedSentinelReplaysOptions{.afterMs = NOW + 1'500, .beforeMs = NOW + 10'000}));
    co_await kv.delete_(shortKey);
    const rt::KvKey secondChunk = chunkKey("capture-range-2", 0);
    co_await kv.delete_(secondChunk);
    RECORD("range missing chunk", range(kv, read::ListEncryptedSentinelReplaysOptions{.afterMs = NOW + 500, .beforeMs = NOW + 10'000}));
    const rt::KvValue fourBytes(rt::KvBytes(4, 0));
    co_await kv.set(secondChunk, fourBytes);
    RECORD("range short chunk", range(kv, read::ListEncryptedSentinelReplaysOptions{.afterMs = NOW + 500, .beforeMs = NOW + 10'000}));
    kv.close();
  }

  // ---- getChunks and manifestMatchesKey ----
  {
    rt::MemoryKv kv(kvClock);
    co_await store(kv, first);
    RECORD("chunks stored", chunks(kv, first.manifest));
    model::SentinelReplayManifest paged = first.manifest;
    paged.capture_id = "capture-paged";
    paged.chunk_count = 11;
    paged.ciphertext_bytes = 10 * model::SENTINEL_REPLAY_CHUNK_BYTES + 1;
    for (int index = 0; index < 10; index += 1) {
      rt::KvBytes bytes(static_cast<std::size_t>(model::SENTINEL_REPLAY_CHUNK_BYTES), static_cast<std::uint8_t>(index));
      bytes.back() = static_cast<std::uint8_t>(200 + index);
      const rt::KvValue chunk(std::move(bytes));
      const rt::KvKey pagedKey = chunkKey("capture-paged", index);
      co_await kv.set(pagedKey, chunk);
    }
    const rt::KvValue tail(rt::KvBytes(1, 0xab));
    const rt::KvKey tailKey = chunkKey("capture-paged", 10);
    co_await kv.set(tailKey, tail);
    RECORD("chunks paged", chunks(kv, paged));
    const rt::KvValue longTail(rt::KvBytes(2, 0));
    co_await kv.set(tailKey, longTail);
    RECORD("chunks long tail", chunks(kv, paged));
    const rt::KvValue wideTail(rt::KvArrayBufferView{.kind = rt::KvViewKind::Uint16Array, .buffer = rt::KvBytes(2, 0), .byteOffset = 0, .byteLength = 2});
    co_await kv.set(tailKey, wideTail);
    RECORD("chunks wrong type", chunks(kv, paged));
    const rt::KvValue textTail("not bytes");
    co_await kv.set(tailKey, textTail);
    RECORD("chunks text", chunks(kv, paged));
    co_await kv.delete_(tailKey);
    RECORD("chunks missing tail", chunks(kv, paged));
    const rt::KvKey middleKey = chunkKey("capture-paged", 3);
    co_await kv.delete_(middleKey);
    RECORD("chunks missing middle", chunks(kv, paged));
    const model::SentinelReplayManifest& m = first.manifest;
    const std::vector<std::pair<std::string, rt::KvKey>> keys{
      {"exact", rt::KvKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, m.captured_at_ms, m.fingerprint, m.capture_id}},
      {"wrong prefix", rt::KvKey{"wrong", "prefix"}},
      {"short", rt::KvKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, m.captured_at_ms}},
      {"long", rt::KvKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, m.captured_at_ms, m.fingerprint, m.capture_id, "extra"}},
      {"other time", rt::KvKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, m.captured_at_ms + 1, m.fingerprint, m.capture_id}},
      {"other fingerprint", rt::KvKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, m.captured_at_ms, std::string(64, '0'), m.capture_id}},
      {"other capture", rt::KvKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, m.captured_at_ms, m.fingerprint, "other"}},
      {"bigint time", rt::KvKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, rt::BigInt::fromNumber(m.captured_at_ms), m.fingerprint, m.capture_id}},
      {"text time", rt::KvKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, rt::numberToString(m.captured_at_ms), m.fingerprint, m.capture_id}},
      {"other namespace", rt::KvKey{"uos_ai", "sentinel_replay", "v2", "manifest", m.captured_at_ms, m.fingerprint, m.capture_id}},
    };
    for (const auto& [label, key] : keys) RECORD("matches " + label, resultOf(json::value(read::manifestMatchesKey(key, m))));
    kv.close();
  }

  // ---- the incident export ----
  {
    rt::MemoryKv kv(kvClock);
    const std::string incidentId = "provider-" + INCIDENT_UUID;
    RECORD("incident bad id", incidentPage(kv, read::ListEncryptedSentinelIncidentReplaysOptions{.incidentId = "provider-nope"}));
    RECORD("incident limit 2", incidentPage(kv, read::ListEncryptedSentinelIncidentReplaysOptions{.incidentId = incidentId, .limit = 2}));
    const std::vector<std::string> badCursors{std::string(), std::string(2'049, 'a'), std::string("has spaces")};
    for (const std::string& cursor : badCursors) {
      RECORD("incident cursor " + std::to_string(cursor.size()),
             incidentPage(kv, read::ListEncryptedSentinelIncidentReplaysOptions{.incidentId = incidentId, .cursor = cursor}));
    }
    const read::ListEncryptedSentinelIncidentReplaysOptions query{.incidentId = incidentId};
    RECORD("incident empty", incidentPage(kv, query));
    co_await store(kv, incident);
    const rt::KvKey key = manifestKey(incident.manifest);
    const rt::KvValue keyArray = keyValue(key);
    rt::KvArray sixParts(keyArray.asArray().begin(), keyArray.asArray().begin() + 6);
    rt::KvArray withObject = sixParts;
    withObject.push_back(rt::KvValue(rt::KvObject{{"part", rt::KvValue(1)}}));
    rt::KvArray withNull{rt::KvValue(nullptr)};
    withNull.insert(withNull.end(), keyArray.asArray().begin() + 1, keyArray.asArray().end());
    rt::KvArray withArray = sixParts;
    withArray.push_back(rt::KvValue(rt::KvArray{rt::KvValue(1), rt::KvValue(nullptr), rt::KvValue("x")}));
    const auto reference = [](const rt::KvValue& manifestKeyValue, double version) {
      return rt::KvValue(rt::KvObject{{"version", rt::KvValue(version)}, {"manifest_key", manifestKeyValue}});
    };
    const std::vector<std::tuple<std::string, rt::KvKey, rt::KvValue>> invalid{
      {"wrong version", rt::KvKey{outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, std::string(64, '2')}, reference(keyArray, 2)},
      {"short manifest key", rt::KvKey{outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, std::string(64, '2')},
       reference(rt::KvValue(rt::KvArray{rt::KvValue(NOW)}), 1)},
      {"over-long reference key", rt::KvKey{outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, std::string(64, '3'), "extra"}, reference(keyArray, 1)},
      {"non-hex reference key", rt::KvKey{outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, "not-hex"}, reference(keyArray, 1)},
      {"numeric reference key", rt::KvKey{outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, 7}, reference(keyArray, 1)},
      {"object in manifest key", rt::KvKey{outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, std::string(64, '4')}, reference(rt::KvValue(withObject), 1)},
      {"null in manifest key", rt::KvKey{outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, std::string(64, '4')}, reference(rt::KvValue(withNull), 1)},
      {"array in manifest key", rt::KvKey{outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, std::string(64, '4')}, reference(rt::KvValue(withArray), 1)},
    };
    for (const auto& [label, refKey, refValue] : invalid) {
      co_await kv.set(refKey, refValue);
      RECORD("incident " + label, incidentPage(kv, query));
      co_await kv.delete_(refKey);
    }
    auto linking = outbox::linkSentinelReplayToIncident(kv, incidentId, incident.manifest.fingerprint, key);
    co_await std::move(linking);
    RECORD("incident listed", rt::async([&]() -> rt::Task<json::value> {
             auto listing = read::listEncryptedSentinelIncidentReplays(kv, query);
             const read::EncryptedSentinelReplayPage page = co_await std::move(listing);
             auto decrypting = plainJson(page.captures.at(0));
             json::value plaintext = co_await std::move(decrypting);
             co_return json::value(object({{"page", pageJson(page)}, {"plaintext", plaintext}}));
           }));
    const std::string danglingFingerprint(64, '0');
    const rt::KvKey danglingRefKey{outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, danglingFingerprint};
    const rt::KvValue danglingReference = reference(keyValue(rt::KvKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, danglingFingerprint, "capture-dangling"}), 1);
    co_await kv.set(danglingRefKey, danglingReference);
    std::string cursor;
    RECORD("incident skips a dangling reference", rt::async([&]() -> rt::Task<json::value> {
             auto listing = read::listEncryptedSentinelIncidentReplays(kv, query);
             const read::EncryptedSentinelReplayPage page = co_await std::move(listing);
             cursor = page.cursor;
             const rt::KvEntryMaybe<> dangling = co_await kv.get(danglingRefKey);
             co_return json::value(object({{"page", pageJson(page)}, {"dangling", dangling.value.toJson()}}));
           }));
    RECORD("incident after the last capture", incidentPage(kv, read::ListEncryptedSentinelIncidentReplaysOptions{.incidentId = incidentId, .cursor = cursor}));
    const rt::KvKey linkedRefKey{outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, incident.manifest.fingerprint};
    co_await kv.delete_(linkedRefKey);
    const rt::KvValue foreignReference = reference(keyValue(rt::KvKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, incident.manifest.fingerprint, "capture-incident"}), 1);
    co_await kv.set(danglingRefKey, foreignReference);
    RECORD("incident foreign manifest", incidentPage(kv, query));
    const rt::KvKey renamedKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, danglingFingerprint, "capture-incident"};
    const rt::KvValue renamedReference = reference(keyValue(renamedKey), 1);
    co_await kv.set(danglingRefKey, renamedReference);
    model::SentinelReplayManifest renamed = incident.manifest;
    renamed.fingerprint = danglingFingerprint;
    co_await kv.set(renamedKey, manifestValue(renamed));
    RECORD("incident manifest under another key", incidentPage(kv, query));
    const rt::KvValue versionTwo = value(object({{"version", 2}}));
    co_await kv.set(renamedKey, versionTwo);
    RECORD("incident corrupt manifest", incidentPage(kv, query));
    co_await kv.delete_(danglingRefKey);
    kv.close();
  }

  // ---- request id status and export ----
  {
    rt::MemoryKv kv(kvClock);
    const auto statusKey = [](std::string id) { return rt::KvKey{model::SENTINEL_REPLAY_REQUEST_PREFIX, std::move(id)}; };
    RECORD("status unknown", status(kv, "never-stored-request", NOW));
    RECORD("status bad id", status(kv, "bad request id", NOW));
    RECORD("export unknown", requestExport(kv, "never-captured-request", NOW));
    RECORD("export bad id", requestExport(kv, "bad request id", NOW));
    const rt::KvValue disabled = value(object({{"version", 1},
                                               {"request_id", "disabled-request"},
                                               {"status", "disabled"},
                                               {"reason", "key_missing"},
                                               {"captured_at_ms", rt::makeNumber(NOW)},
                                               {"manifest_key", nullptr},
                                               {"fingerprint", nullptr},
                                               {"expires_at_ms", nullptr}}));
    const rt::KvKey key_disabled_request = statusKey("disabled-request");
    co_await kv.set(key_disabled_request, disabled);
    RECORD("export disabled", requestExport(kv, "disabled-request", NOW));
    json::array expiringKey{"uos_ai", "sentinel_replay", "v1", "manifest", rt::makeNumber(NOW), text(std::string(64, 'c')), "capture-x"};
    const rt::KvValue expiring = value(object({{"version", 1},
                                               {"request_id", "expiring-request"},
                                               {"status", "ready"},
                                               {"reason", nullptr},
                                               {"captured_at_ms", rt::makeNumber(NOW)},
                                               {"manifest_key", expiringKey},
                                               {"fingerprint", text(std::string(64, 'c'))},
                                               {"expires_at_ms", rt::makeNumber(NOW + 1'000)},
                                               {"extra", "kept"}}));
    const rt::KvKey key_expiring_request = statusKey("expiring-request");
    co_await kv.set(key_expiring_request, expiring);
    RECORD("status live", status(kv, "expiring-request", NOW + 999));
    RECORD("status expired", status(kv, "expiring-request", NOW + 1'000));
    const rt::KvValue corrupt = value(object({{"version", 1}, {"request_id", "corrupt-request"}}));
    const rt::KvKey key_corrupt_request = statusKey("corrupt-request");
    co_await kv.set(key_corrupt_request, corrupt);
    RECORD("status corrupt", status(kv, "corrupt-request", NOW));
    co_await store(kv, byRequest);
    const rt::KvKey key = manifestKey(byRequest.manifest);
    const json::object row = object({{"version", 1},
                                     {"request_id", "by-request-id"},
                                     {"status", "incomplete"},
                                     {"reason", "upstream_trace_empty"},
                                     {"captured_at_ms", rt::makeNumber(NOW)},
                                     {"manifest_key", keyJson(key)},
                                     {"fingerprint", text(byRequest.manifest.fingerprint)},
                                     {"expires_at_ms", rt::makeNumber(NOW + model::SENTINEL_REPLAY_TTL_MS)}});
    const rt::KvKey key_by_request_id = statusKey("by-request-id");
    co_await kv.set(key_by_request_id, value(row));
    RECORD("export resolved", rt::async([&]() -> rt::Task<json::value> {
             auto listing = read::listEncryptedSentinelReplaysByRequestId(kv, "by-request-id", NOW);
             const read::EncryptedSentinelRequestReplays result = co_await std::move(listing);
             auto decrypting = plainJson(result.captures.at(0));
             json::value plaintext = co_await std::move(decrypting);
             co_return json::value(object({{"export", requestExportJson(result)}, {"plaintext", plaintext}}));
           }));
    RECORD("export after expiry", requestExport(kv, "by-request-id", NOW + model::SENTINEL_REPLAY_TTL_MS));
    co_await kv.delete_(key);
    RECORD("export manifest gone", requestExport(kv, "by-request-id", NOW));
    const rt::KvKey tombstoneKey = schema::sentinelReplayEvictionKey(byRequest.manifest.fingerprint);
    const rt::KvValue evicted = value(object({{"reason", text(std::string(schema::SENTINEL_REPLAY_EVICTION_REASON))}}));
    co_await kv.set(tombstoneKey, evicted);
    RECORD("export evicted", requestExport(kv, "by-request-id", NOW));
    const rt::KvValue expired = value(object({{"reason", text(std::string(schema::SENTINEL_REPLAY_EXPIRED_REASON))}}));
    co_await kv.set(tombstoneKey, expired);
    RECORD("export payload expired", requestExport(kv, "by-request-id", NOW));
    const rt::KvValue oddTombstone(rt::KvArray{rt::KvValue("storage_budget")});
    co_await kv.set(tombstoneKey, oddTombstone);
    RECORD("export odd tombstone", requestExport(kv, "by-request-id", NOW));
    co_await kv.delete_(tombstoneKey);
    co_await kv.set(key, manifestValue(byRequest.manifest));
    json::object mismatched = row;
    rt::objectSet(mismatched, "request_id", "mismatched-request");
    rt::objectSet(mismatched, "status", "ready");
    rt::objectSet(mismatched, "reason", nullptr);
    rt::objectSet(mismatched, "fingerprint", text(std::string(64, '5')));
    const rt::KvKey key_mismatched_request = statusKey("mismatched-request");
    co_await kv.set(key_mismatched_request, value(mismatched));
    RECORD("export mismatched", requestExport(kv, "mismatched-request", NOW));
    const rt::KvKey falsyKey{model::SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, std::string(64, 'f'), "falsy"};
    json::object falsy = row;
    rt::objectSet(falsy, "request_id", "falsy-request");
    rt::objectSet(falsy, "manifest_key", keyJson(falsyKey));
    rt::objectSet(falsy, "fingerprint", text(std::string(64, 'f')));
    const rt::KvKey key_falsy_request = statusKey("falsy-request");
    co_await kv.set(key_falsy_request, value(falsy));
    const rt::KvValue zero(0);
    co_await kv.set(falsyKey, zero);
    RECORD("export falsy manifest", requestExport(kv, "falsy-request", NOW));
    const rt::KvValue byRequestKeyArray = keyValue(key);
    rt::KvArray objectKeyParts(byRequestKeyArray.asArray().begin(), byRequestKeyArray.asArray().begin() + 6);
    objectKeyParts.push_back(rt::KvValue(rt::KvObject()));
    rt::KvValue objectKeyRow = value(row);
    objectKeyRow.asObject().set("request_id", rt::KvValue("object-key-request"));
    objectKeyRow.asObject().set("manifest_key", rt::KvValue(objectKeyParts));
    const rt::KvKey key_object_key_request = statusKey("object-key-request");
    co_await kv.set(key_object_key_request, objectKeyRow);
    // `export object key part`: the TS returns the stored row from readSentinelReplayCaptureStatus and fails in the
    // kv.get of its manifest_key; the C++ row cannot hold the part, so the status read fails first, with the same error.
    RECORD("export object key part", requestExport(kv, "object-key-request", NOW));
    json::object chunkless = row;
    rt::objectSet(chunkless, "request_id", "chunkless-request");
    const rt::KvKey key_chunkless_request = statusKey("chunkless-request");
    co_await kv.set(key_chunkless_request, value(chunkless));
    co_await kv.delete_(chunkKey(byRequest.manifest.capture_id, 0));
    RECORD("export chunk missing", requestExport(kv, "chunkless-request", NOW));
    // A ledger that records pruned status rows turns an unknown request into status_not_retained.
    const std::optional<schema::SentinelReplayLedgerSnapshot> snapshot = co_await schema::readSentinelReplayLedgerSnapshot(kv);
    schema::SentinelReplayBudgetLedger pruned = snapshot->ledger;
    pruned.status_pruned_records = 1;
    co_await kv.set(schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY, rt::KvValue(json::value_from(pruned)));
    RECORD("status not retained", status(kv, "pruned-request", NOW));
    RECORD("export not retained", requestExport(kv, "pruned-request", NOW));
    json::object corruptLedger = json::value_from(snapshot->ledger).as_object();
    rt::objectSet(corruptLedger, "status_records", -1);
    co_await kv.set(schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY, value(corruptLedger));
    RECORD("status with a corrupt ledger", status(kv, "pruned-request", NOW));
    kv.close();
  }
  transcript.finish();
}
