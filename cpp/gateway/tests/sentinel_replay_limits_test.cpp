// Links: tests/sentinel_replay_limits_test.cpp src/sentinel/replay_store.cpp src/sentinel/replay_retention.cpp
//        src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_read.cpp
//        src/sentinel/replay_retention_schema.cpp src/sentinel/replay_model.cpp src/sentinel/replay_envelope.cpp
//        src/sentinel/replay_observation.cpp src/sentinel/upstream_capture.cpp src/sentinel/replay_limits.cpp
//        src/sentinel/incident_outbox.cpp src/request.cpp src/utils.cpp src/config.cpp src/release.cpp src/kv.cpp
//        src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
// Port of tests/sentinel-replay-limits.test.ts, in full (all 3 cases). Storage is `openKv(":memory:")`: rt::MemoryKv.
#include "uos/kv.hpp"
#include "uos/sentinel/replay_limits.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "uos/sentinel/replay_store.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace rt = uos::rt;
using namespace uos::sentinel::replay_limits;
using namespace uos::sentinel::upstream_capture;

namespace {
  /** A trace at the chunk-count and byte ceilings: 4,096 chunks of 1,023 raw bytes. */
  SentinelUpstreamTrace maximalTrace() {
    const std::string chunk = rt::repeat("b", 1'023);
    const std::vector<std::string> chunks(static_cast<std::size_t>(SENTINEL_REPLAY_MAX_UPSTREAM_CHUNKS), chunk);
    std::vector<std::string> chunksBase64;
    for (const std::string& text : chunks) chunksBase64.push_back(rt::btoa(text));
    SentinelUpstreamAttempt attempt{
      .provider = "chatgpt_codex",
      .status = 200,
      .content_type = "text/event-stream",
      .chunks_base64 = chunksBase64,
      .terminal = "eof",
      .headers = std::nullopt,
      .headers_truncated = std::nullopt,
      .chunk_times_ms = std::nullopt,
      .started_at_ms = std::nullopt,
      .headers_at_ms = rt::undefined,
      .ended_at_ms = rt::undefined,
    };
    SentinelUpstreamTrace trace{.version = 1, .attempts = {attempt}, .attempts_truncated = false, .bytes_truncated = false, .chunks_truncated = false};
    const double total = static_cast<double>(chunks.size()) * static_cast<double>(chunk.size());
    EXPECT_EQ(total <= SENTINEL_REPLAY_MAX_UPSTREAM_BYTES, true);
    return trace;
  }
}

TEST("derived metadata allowance covers the full allowed trace and escaping covers the request envelope") {
  EXPECT_EQ(SENTINEL_REPLAY_METADATA_DERIVED_BYTES < SENTINEL_REPLAY_MAX_METADATA_BYTES, true);
  EXPECT_EQ(SENTINEL_REPLAY_REQUEST_FILE_MAX_BYTES >= SENTINEL_REPLAY_JSON_ESCAPE_FACTOR * SENTINEL_REPLAY_MAX_REQUEST_BYTES, true);
  EXPECT_EQ(SENTINEL_REPLAY_UPSTREAM_FILE_MAX_BYTES >= SENTINEL_REPLAY_MAX_METADATA_BYTES, true);
}

TEST("a trace one chunk over the bound and a body one byte over the bound are rejected") {
  const SentinelUpstreamTrace overChunk = maximalTrace();
  const SentinelUpstreamAttempt& attempts = overChunk.attempts.at(0);
  SentinelUpstreamAttempt longer = attempts;
  longer.chunks_base64.push_back(rt::btoa("b"));
  SentinelUpstreamTrace over = overChunk;
  over.attempts = {longer};
  static const rt::Regex TOO_MANY_CHUNKS("too many chunks", "");
  bool threw = false;
  try {
    parseSentinelUpstreamTrace(over);
  } catch (const rt::JsError& error) {
    threw = true;
    EXPECT(TOO_MANY_CHUNKS.test(error.message));
  }
  EXPECT(threw);
}

namespace {
  namespace json = uos::json;
  using namespace uos::sentinel::replay_model;
  using uos::sentinel::replay_observation::PersistDependencies;
  using uos::sentinel::replay_observation::resolveSentinelClientFailureObservation;
  using uos::sentinel::replay_observation::SentinelReplayPersistResult;
  using uos::sentinel::replay_observation::SentinelReplayPersistResultStored;
  using uos::sentinel::replay_read::decryptExportedSentinelReplay;
  using uos::sentinel::replay_read::EncryptedSentinelRequestReplays;
  using uos::sentinel::replay_read::listEncryptedSentinelReplaysByRequestId;
  using uos::sentinel::replay_store::persistEncryptedSentinelReplay;

  constexpr double BUDGET_HEADROOM_BYTES = 512 * 1'024 * 1'024;

  SentinelFailureObservation failureObservation() {
    return {
      .status = 502,
      .stream = false,
      .completed = false,
      .terminal_type = "http.error",
      .failure_kind = "upstream_timeout",
      .synthetic_terminal_type = std::nullopt,
      .provider_route = "chatgpt_codex",
    };
  }

  AcceptedSentinelReplayInput input(std::shared_ptr<rt::Bytes> body, const SentinelUpstreamTrace& upstream, const std::string& requestId) {
    return {
      .endpoint = "/v1/responses",
      .method = "POST",
      .body = std::move(body),
      .content_type = "application/json",
      .compatibility_headers = {},
      .request_id = requestId,
      .git_sha = std::string(40, 'a'),
      .deno_revision = "limits-fixture",
      .upstream = upstream,
    };
  }

  /** A JSON request body of exactly SENTINEL_REPLAY_MAX_REQUEST_BYTES bytes using control-char escapes. */
  std::shared_ptr<rt::Bytes> maximalEscapedBody() {
    const std::string control = "\x01";
    const double target = SENTINEL_REPLAY_MAX_REQUEST_BYTES - 64;
    const double repeats = rt::Math::floor(target / 6);
    json::object fields;
    fields["model"] = "gpt-5.6-sol";
    fields["input"] = rt::repeat(control, repeats);
    auto body = std::make_shared<rt::Bytes>(rt::encodeUtf8(rt::jsonStringify(json::value(std::move(fields)))));
    EXPECT(static_cast<double>(body->size()) <= SENTINEL_REPLAY_MAX_REQUEST_BYTES);
    EXPECT(static_cast<double>(body->size()) > SENTINEL_REPLAY_MAX_REQUEST_BYTES - 128);  // the body must sit at the cap
    return body;
  }

  PersistDependencies dependenciesFor(std::shared_ptr<rt::Kv> kv, const rt::Bytes& keyBytes, double nowMs, std::string captureId) {
    return {
      .kv = std::move(kv),
      .keyBytes = keyBytes,
      .now = [nowMs] { return nowMs; },
      .randomUuid = [captureId] { return captureId; },
      .budgetBytes = BUDGET_HEADROOM_BYTES,
    };
  }

  // `assert.rejects(task, /pattern/)`
  rt::Task<std::string> rejectionMessage(rt::Task<SentinelReplayPersistResult> task) {
    std::string message = "<resolved>";
    try {
      co_await std::move(task);
    } catch (const rt::JsError& error) {
      message = error.message;
    }
    co_return message;
  }
}

ASYNC_TEST("a maximum-size capture round-trips through store, export and decrypt") {
  const std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  uos::kv::setKvForTest(kv);
  rt::Bytes keyBytes(32);
  rt::crypto::getRandomValues(std::span<std::uint8_t>(keyBytes));
  const std::shared_ptr<rt::Bytes> body = maximalEscapedBody();
  const SentinelUpstreamTrace trace = maximalTrace();
  const AcceptedSentinelReplayInput maximal = input(body, trace, "limits-maximal");
  const SentinelFailureObservation failure = failureObservation();
  const PersistDependencies maximalDependencies = dependenciesFor(kv, keyBytes, 1'700'000'000'000, "capture-limits-maximal");
  auto storing = persistEncryptedSentinelReplay(maximal, failure, maximalDependencies);
  const SentinelReplayPersistResult stored = co_await std::move(storing);
  EXPECT(std::holds_alternative<SentinelReplayPersistResultStored>(stored));
  auto exporting = listEncryptedSentinelReplaysByRequestId(*kv, "limits-maximal");
  const EncryptedSentinelRequestReplays exported = co_await std::move(exporting);
  ASSERT_EQ(exported.captures.size(), std::size_t{1});
  auto decrypting = decryptExportedSentinelReplay(exported.captures[0], keyBytes);
  const SentinelReplayPlaintext plaintext = co_await std::move(decrypting);
  EXPECT_EQ(plaintext.body->size(), body->size());
  ASSERT_EQ(plaintext.upstream.attempts.size(), std::size_t{1});
  EXPECT_EQ(static_cast<double>(plaintext.upstream.attempts[0].chunks_base64.size()), SENTINEL_REPLAY_MAX_UPSTREAM_CHUNKS);
  EXPECT_EQ(plaintext.version, 3.0);

  // A capture with a complete recorded upstream attempt is fully replayable,
  // so its persisted status row must be `ready`, never `incomplete`.
  SentinelUpstreamAttempt readyAttempt{
    .provider = "chatgpt_codex",
    .status = 200,
    .content_type = "text/event-stream",
    .chunks_base64 = {rt::btoa("data: {}\n\n")},
    .terminal = "eof",
    .headers = json::object{{"content-type", "text/event-stream"}},
    .headers_truncated = false,
    .chunk_times_ms = std::vector<double>{5},
    .started_at_ms = 100,
    .headers_at_ms = 101,
    .ended_at_ms = 110,
  };
  SentinelUpstreamTrace readyTrace{.version = 2, .attempts = {}, .attempts_truncated = false, .bytes_truncated = false, .chunks_truncated = false};
  readyTrace.attempts.push_back(std::move(readyAttempt));
  json::object readyFields;
  readyFields["model"] = "gpt-5.6-sol";
  readyFields["input"] = "ready-coverage";
  const auto readyBody = std::make_shared<rt::Bytes>(rt::encodeUtf8(rt::jsonStringify(json::value(std::move(readyFields)))));
  // `ready` requires every coverage reason absent: a complete upstream
  // attempt AND the recorded downstream terminal body.
  SentinelClientFailureObservation readyObservation = resolveSentinelClientFailureObservation(failureObservation());
  json::object errorBody;
  json::object errorMessage;
  errorMessage["message"] = "upstream timeout";
  errorBody["error"] = std::move(errorMessage);
  readyObservation.terminal_body_base64 = rt::btoa(rt::jsonStringify(json::value(std::move(errorBody))));
  const AcceptedSentinelReplayInput readyInput = input(readyBody, readyTrace, "limits-ready-coverage");
  const PersistDependencies readyDependencies = dependenciesFor(kv, keyBytes, 1'700'000'000'500, "capture-limits-ready");
  auto storingReady = persistEncryptedSentinelReplay(readyInput, failure, readyDependencies, readyObservation);
  const SentinelReplayPersistResult ready = co_await std::move(storingReady);
  EXPECT(std::holds_alternative<SentinelReplayPersistResultStored>(ready));
  // Read at the fixture clock, not the wall clock: the stored expiry is synthetic.
  auto exportingReady = listEncryptedSentinelReplaysByRequestId(*kv, "limits-ready-coverage", 1'700'000'000'500);
  const EncryptedSentinelRequestReplays readyExport = co_await std::move(exportingReady);
  EXPECT_EQ(readyExport.captures.size(), std::size_t{1});
  EXPECT_EQ(readyExport.status.status, std::string("ready"));
  EXPECT(!readyExport.status.reason.has_value());

  // One byte over the accepted request cap is rejected before publication.
  const auto overBody = std::make_shared<rt::Bytes>(static_cast<std::size_t>(SENTINEL_REPLAY_MAX_REQUEST_BYTES + 1), 0x61);
  const AcceptedSentinelReplayInput over = input(overBody, trace, "limits-over-body");
  const PersistDependencies overDependencies = dependenciesFor(kv, keyBytes, 1'700'000'001'000, "capture-limits-over-body");
  auto refusing = persistEncryptedSentinelReplay(over, failure, overDependencies);
  const std::string message = co_await rejectionMessage(std::move(refusing));
  static const rt::Regex TOO_LARGE("too large", "");
  EXPECT(TOO_LARGE.test(message));
  kv->close();
  uos::kv::setKvForTest(nullptr);
}
