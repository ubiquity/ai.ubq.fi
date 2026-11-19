// Links: tests/sentinel_upstream_capture_test.cpp src/sentinel/replay_capture.cpp src/sentinel/replay_store.cpp
//        src/sentinel/replay_retention.cpp src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp
//        src/sentinel/replay_read.cpp src/sentinel/replay_retention_schema.cpp src/sentinel/replay_model.cpp
//        src/sentinel/replay_envelope.cpp src/sentinel/replay_observation.cpp src/sentinel/upstream_capture.cpp
//        src/sentinel/replay_limits.cpp src/sentinel/incident_outbox.cpp src/request.cpp src/utils.cpp src/config.cpp
//        src/release.cpp src/kv.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
//        src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
// Port of tests/sentinel-upstream-capture.test.ts, in full (all 15 cases). Storage is `openKv(":memory:")`:
// rt::MemoryKv.
#include "uos/kv.hpp"
#include "uos/sentinel/replay_capture.hpp"
#include "uos/utils.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::sentinel::upstream_capture;

  // assert.throws(fn, /pattern/): node:assert matches the pattern against String(error).

  using uos::utils::base64UrlDecode;
  using uos::utils::base64UrlEncode;

  // { ...base, key: value, ... }
  json::object spread(json::object base, std::initializer_list<std::pair<std::string_view, json::value>> members) {
    for (const auto& [key, value] : members) rt::objectDefine(base, key, value);
    return base;
  }

  rt::QueuingStrategy zeroHighWaterMark() {
    rt::QueuingStrategy strategy;
    strategy.highWaterMark = 0;
    return strategy;
  }

  rt::ResponseInit responseInit(double status, std::initializer_list<rt::Headers::Entry> headers) {
    rt::ResponseInit init;
    init.status = status;
    init.headers = rt::Headers(headers);
    return init;
  }

  /** The wrapped response body, which every wrapped response in this file must expose. */
  rt::ByteStream responseBody(const rt::Response& response) {
    std::optional<rt::ByteStream> body = response.body();
    ASSERT(body.has_value());
    return *body;
  }

  /** The single captured attempt of a trace that must contain one. */
  const SentinelUpstreamAttempt& firstAttempt(const SentinelUpstreamTrace& trace) {
    ASSERT(!trace.attempts.empty());
    return trace.attempts[0];
  }

  json::value invalidTrace(json::object attempt) {
    return json::object{{"version", 1}, {"attempts", json::array{json::value(std::move(attempt))}}, {"attempts_truncated", false}, {"bytes_truncated", false}, {"chunks_truncated", false}};
  }

  json::object baseAttempt() {
    return json::object{{"provider", "chatgpt_codex"}, {"status", 200}, {"content_type", "application/json"}, {"chunks_base64", json::array()}, {"terminal", "eof"}};
  }

  /** A minimal valid version-2 attempt: one canonical chunk, timing and headers. */
  json::object v2Attempt() {
    return json::object{
      {"provider", "chatgpt_codex"},
      {"status", 200},
      {"content_type", "application/json"},
      {"chunks_base64", json::array{"YQ=="}},
      {"terminal", "eof"},
      {"headers", json::object{{"content-type", "application/json"}}},
      {"headers_truncated", false},
      {"chunk_times_ms", json::array{0}},
      {"started_at_ms", 1'700'000'000'000},
      {"headers_at_ms", 1'700'000'000'001},
      {"ended_at_ms", 1'700'000'000'002},
    };
  }

  json::value v2Trace(json::object attempt) {
    return json::object{{"version", 2}, {"attempts", json::array{json::value(std::move(attempt))}}, {"attempts_truncated", false}, {"bytes_truncated", false}, {"chunks_truncated", false}};
  }

  std::vector<std::string> decodedChunks(const SentinelUpstreamAttempt& attempt) {
    std::vector<std::string> texts;
    for (const std::string& chunk : attempt.chunks_base64) texts.push_back(rt::decodeUtf8(base64UrlDecode(chunk)));
    return texts;
  }

  std::size_t decodedByteLength(const SentinelUpstreamAttempt& attempt) {
    std::size_t sum = 0;
    for (const std::string& chunk : attempt.chunks_base64) sum += base64UrlDecode(chunk).size();
    return sum;
  }
}

TEST("frozen trace relationship: status and content_type are null together") {
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(invalidTrace(spread(baseAttempt(), {{"status", nullptr}, {"content_type", "application/json"}}))), "content type");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(invalidTrace(spread(baseAttempt(), {{"status", 204}, {"content_type", nullptr}}))), "content type");
  // A pending pre-header attempt is exactly null/null with no chunks.
  const SentinelUpstreamTrace preHeader = parseSentinelUpstreamTrace(
    invalidTrace(json::object{{"provider", "surplus"}, {"status", nullptr}, {"content_type", nullptr}, {"chunks_base64", json::array()}, {"terminal", "pending"}}));
  EXPECT_EQ(preHeader.attempts.size(), std::size_t{1});
  EXPECT_EQ(preHeader.attempts[0].status, std::nullopt);
  EXPECT_EQ(preHeader.attempts[0].content_type, std::nullopt);
  EXPECT_EQ(preHeader.attempts[0].terminal, "pending");
}

TEST("frozen trace relationship: fetch_error and header-bearing terminals") {
  const json::value fetchError =
    invalidTrace(json::object{{"provider", "metered"}, {"status", nullptr}, {"content_type", nullptr}, {"chunks_base64", json::array()}, {"terminal", "fetch_error"}});
  EXPECT_EQ(parseSentinelUpstreamTrace(fetchError).attempts[0].terminal, "fetch_error");
  EXPECT_THROWS_MATCHING(
    parseSentinelUpstreamTrace(invalidTrace(json::object{{"provider", "metered"}, {"status", 502}, {"content_type", "application/json"}, {"chunks_base64", json::array()}, {"terminal", "fetch_error"}})),
    "fetch_error");
  EXPECT_THROWS_MATCHING(
    parseSentinelUpstreamTrace(invalidTrace(json::object{{"provider", "metered"}, {"status", nullptr}, {"content_type", nullptr}, {"chunks_base64", json::array()}, {"terminal", "eof"}})),
    "requires a status");
  EXPECT_THROWS_MATCHING(
    parseSentinelUpstreamTrace(invalidTrace(json::object{{"provider", "metered"}, {"status", nullptr}, {"content_type", nullptr}, {"chunks_base64", json::array()}, {"terminal", "read_error"}})),
    "requires a status");
  EXPECT_THROWS_MATCHING(
    parseSentinelUpstreamTrace(invalidTrace(json::object{{"provider", "metered"}, {"status", nullptr}, {"content_type", nullptr}, {"chunks_base64", json::array()}, {"terminal", "cancelled"}})),
    "requires a status");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(invalidTrace(spread(baseAttempt(), {{"status", nullptr}, {"content_type", nullptr}, {"chunks_base64", json::array{"YQ=="}}}))),
                         "cannot have chunks");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(invalidTrace(spread(baseAttempt(), {{"status", 99}}))), "status is invalid");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(invalidTrace(spread(baseAttempt(), {{"status", 600}}))), "status is invalid");
}

TEST("strict parser rejects provider enum, keys, version, canonical base64, and attempt bounds") {
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(invalidTrace(spread(baseAttempt(), {{"provider", "codex"}}))), "provider");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(invalidTrace(spread(baseAttempt(), {{"content_type", "text/plain"}}))), "content type");
  // A version-2 trace must carry the version-2 attempt key set; a version-1
  // attempt inside it is a key-set violation, and an unknown version is rejected.
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(json::value(spread(invalidTrace(baseAttempt()).as_object(), {{"version", 2}}))), "keys are invalid");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(json::value(spread(invalidTrace(baseAttempt()).as_object(), {{"version", 3}}))), "version");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(v2Trace(spread(v2Attempt(), {{"headers", json::object{{"authorization", "Bearer secret"}}}}))), "not allowlisted");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(v2Trace(spread(v2Attempt(), {{"headers", json::object{{"content-type", rt::repeat("x", 513)}}}}))), "header value is invalid");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(v2Trace(spread(v2Attempt(), {{"chunk_times_ms", json::array()}}))), "chunk timing does not match");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(v2Trace(spread(v2Attempt(), {{"chunk_times_ms", json::array{5}}}))), "timing exceeds its bounds");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(v2Trace(spread(v2Attempt(), {{"ended_at_ms", nullptr}}))), "end time disagrees");
  const SentinelUpstreamTrace parsedV2 = parseSentinelUpstreamTrace(v2Trace(v2Attempt()));
  EXPECT_EQ(parsedV2.version, std::int64_t{2});
  EXPECT_EQ(firstAttempt(parsedV2).chunk_times_ms, std::optional<std::vector<double>>(std::vector<double>{0}));
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(json::value(spread(invalidTrace(baseAttempt()).as_object(), {{"extra", true}}))), "keys are invalid");
  // Non-canonical padded base64: YR== decodes to the same byte as YQ== but
  // leaves non-zero trailing bits, so it must be rejected.
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(invalidTrace(spread(baseAttempt(), {{"chunks_base64", json::array{"YR=="}}}))), "base64");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(invalidTrace(spread(baseAttempt(), {{"chunks_base64", json::array{"a"}}}))), "base64");
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(invalidTrace(spread(baseAttempt(), {{"chunks_base64", json::array{"YQ"}}}))), "base64");
  json::array attempts;
  for (std::int64_t index = 0; index < SENTINEL_UPSTREAM_MAX_ATTEMPTS + 1; ++index) attempts.emplace_back(baseAttempt());
  const json::value tooManyAttempts =
    json::object{{"version", 1}, {"attempts", std::move(attempts)}, {"attempts_truncated", true}, {"bytes_truncated", false}, {"chunks_truncated", false}};
  EXPECT_THROWS_MATCHING(parseSentinelUpstreamTrace(tooManyAttempts), "too many attempts");
  EXPECT_EQ(isSentinelUpstreamTrace(tooManyAttempts), false);
  EXPECT_EQ(isSentinelUpstreamTrace(emptySentinelUpstreamTrace()), true);
}

TEST("normalizeSentinelUpstreamContentType maps MIME before semicolon to fixed literals") {
  EXPECT_EQ(normalizeSentinelUpstreamContentType("text/event-stream; charset=utf-8"), std::optional<std::string>("text/event-stream"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType("TEXT/EVENT-STREAM"), std::optional<std::string>("text/event-stream"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType("application/json; charset=utf-8"), std::optional<std::string>("application/json"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType("text/plain"), std::optional<std::string>("other"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType(""), std::optional<std::string>("other"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType(std::nullopt), std::nullopt);
}

ASYNC_TEST("recorder wrapper is lazy, forwards bytes, and records a complete EOF trace") {
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  struct SourceState {
    std::deque<std::string> sourceParts{"data: one\n\n", "data: two\n\n"};
    int sourcePulls = 0;
  };
  auto state = std::make_shared<SourceState>();
  rt::UnderlyingSource<rt::ByteChunk> underlying;
  underlying.pull = [state](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
    state->sourcePulls += 1;
    if (state->sourceParts.empty()) {
      controller.close();
    } else {
      const std::string next = state->sourceParts.front();
      state->sourceParts.pop_front();
      controller.enqueue(rt::encodeUtf8(next));
    }
  };
  rt::ByteStream source(std::move(underlying), zeroHighWaterMark());
  rt::ResponseInit init = responseInit(200, {{"Content-Type", "text/event-stream; charset=utf-8"}, {"X-Upstream-Secret", "must-never-be-retained"}, {"X-Request-Id", "secret-request-id"}});
  init.statusText = "OK";
  const rt::Response response(rt::BodyInit(std::move(source)), std::move(init));
  const rt::Response wrapped = recorder.startAttempt("surplus").wrap(response);
  EXPECT_EQ(state->sourcePulls, 0);  // wrap must not read ahead
  EXPECT_EQ(wrapped.status(), 200);
  EXPECT_EQ(wrapped.statusText(), "OK");
  EXPECT_EQ(wrapped.headers().get("Content-Type"), std::optional<std::string>("text/event-stream; charset=utf-8"));
  const auto reader = responseBody(wrapped).getReader();
  EXPECT_EQ(state->sourcePulls, 0);  // getReader must not pull
  std::vector<std::string> collected;
  for (;;) {
    auto step = co_await reader.read();
    if (step.done) break;
    collected.push_back(rt::decodeUtf8(*step.value));
  }
  EXPECT_EQ(collected, std::vector<std::string>({"data: one\n\n", "data: two\n\n"}));
  EXPECT_EQ(state->sourcePulls, 3);  // one pull per read plus the terminal close read
  const SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
  EXPECT_EQ(rt::includes(rt::jsonStringify(json::value_from(trace)), "must-never-be-retained"), false);
  EXPECT_EQ(rt::includes(rt::jsonStringify(json::value_from(trace)), "secret-request-id"), false);
  std::vector<std::string> providers;
  for (const SentinelUpstreamAttempt& attempt : trace.attempts) providers.push_back(attempt.provider);
  EXPECT_EQ(providers, std::vector<std::string>({"surplus"}));
  const SentinelUpstreamAttempt& attempt = firstAttempt(trace);
  EXPECT_EQ(attempt.status, std::optional<std::int64_t>(200));
  EXPECT_EQ(attempt.content_type, std::optional<std::string>("text/event-stream"));
  EXPECT_EQ(attempt.terminal, "eof");
  EXPECT_EQ(decodedChunks(attempt), std::vector<std::string>({"data: one\n\n", "data: two\n\n"}));
  EXPECT_EQ(trace.attempts_truncated, false);
  EXPECT_EQ(trace.bytes_truncated, false);
  EXPECT_EQ(trace.chunks_truncated, false);
  recorder.dispose();
}

TEST("recorder maps missing Content-Type on a real response to other and bodyless to EOF") {
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  rt::ResponseInit init;
  init.status = 204;
  const rt::Response bodyless = recorder.startAttempt("chatgpt_codex").wrap(rt::Response(nullptr, std::move(init)));
  EXPECT_EQ(bodyless.status(), 204);
  const SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
  ASSERT_EQ(trace.attempts.size(), std::size_t{1});
  EXPECT_EQ(trace.attempts[0].status, std::optional<std::int64_t>(204));
  EXPECT_EQ(trace.attempts[0].content_type, std::optional<std::string>("other"));
  EXPECT_EQ(trace.attempts[0].terminal, "eof");
  recorder.dispose();
}

ASYNC_TEST("recorder preserves the original read error object and marks the trace read_error") {
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const rt::Reason readError = rt::makeReason(rt::JsError("original upstream read error"));
  rt::UnderlyingSource<rt::ByteChunk> underlying;
  underlying.pull = [readError](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
    controller.enqueue(rt::encodeUtf8("partial-async"));
    controller.error(readError);
  };
  rt::ByteStream source(std::move(underlying), zeroHighWaterMark());
  const rt::Response wrapped = recorder.startAttempt("metered").wrap(rt::Response(rt::BodyInit(std::move(source)), responseInit(200, {{"Content-Type", "application/json"}})));
  const auto reader = responseBody(wrapped).getReader();
  const auto first = co_await reader.read();
  EXPECT_EQ(rt::decodeUtf8(first.value.value()), "partial-async");
  EXPECT_REJECTS_REASON(reader.read(), readError);
  const SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
  const SentinelUpstreamAttempt& attempt = firstAttempt(trace);
  EXPECT_EQ(attempt.terminal, "read_error");
  EXPECT_EQ(attempt.status, std::optional<std::int64_t>(200));
  EXPECT_EQ(decodedChunks(attempt), std::vector<std::string>({"partial-async"}));
  recorder.dispose();
}

ASYNC_TEST("recorder forwards cancellation with the exact reason and surfaces reader cancel rejections") {
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  auto sourceCancelReason = std::make_shared<rt::Reason>();
  rt::UnderlyingSource<rt::ByteChunk> blockingSource;
  blockingSource.start = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { controller.enqueue(rt::encodeUtf8("pending")); };
  blockingSource.cancel = [sourceCancelReason](rt::Reason cancelReason) -> rt::Task<void> {
    *sourceCancelReason = cancelReason;
    return rt::Promise<void>::resolve().wait();
  };
  rt::ByteStream blocking(std::move(blockingSource), zeroHighWaterMark());
  const rt::Reason reason = rt::makeReason(rt::DOMException("client cancelled the stream", "AbortError"));
  const rt::Response wrapped = recorder.startAttempt("cerebras").wrap(rt::Response(rt::BodyInit(std::move(blocking)), responseInit(200, {{"Content-Type", "text/event-stream"}})));
  const rt::ByteStream wrappedBody = responseBody(wrapped);
  co_await wrappedBody.cancel(reason);
  EXPECT(*sourceCancelReason == reason);  // the exact cancellation reason must reach the original reader
  const SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
  const SentinelUpstreamAttempt& attempt = firstAttempt(trace);
  EXPECT_EQ(attempt.terminal, "cancelled");
  EXPECT_EQ(attempt.status, std::optional<std::int64_t>(200));
  recorder.dispose();

  // A rejection from the original reader must reach the wrapper caller.
  SentinelUpstreamRecorder recorderReject = createSentinelUpstreamRecorder();
  const rt::Reason cancelFailure = rt::makeReason(rt::JsError("original cancel failure"));
  rt::UnderlyingSource<rt::ByteChunk> rejectingSource;
  rejectingSource.cancel = [cancelFailure](rt::Reason) -> rt::Task<void> { return rt::Promise<void>::reject(cancelFailure).wait(); };
  rt::ByteStream rejecting(std::move(rejectingSource), zeroHighWaterMark());
  const rt::Response wrappedReject =
    recorderReject.startAttempt("chatgpt_codex").wrap(rt::Response(rt::BodyInit(std::move(rejecting)), responseInit(200, {{"Content-Type", "application/json"}})));
  const rt::Reason abortReason = rt::makeReason(rt::DOMException("abort", "AbortError"));
  const rt::ByteStream rejectBody = responseBody(wrappedReject);
  EXPECT_REJECTS_REASON(rejectBody.cancel(abortReason), cancelFailure);
  EXPECT_EQ(recorderReject.snapshotAndSeal().attempts.at(0).terminal, "cancelled");
  recorderReject.dispose();
}

ASYNC_TEST("recorder bounds: attempts, chunks, and bytes with permanent truncation flags") {
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  std::vector<SentinelUpstreamAttemptHandle> handles;
  for (std::int64_t index = 0; index < SENTINEL_UPSTREAM_MAX_ATTEMPTS + 1; ++index) handles.push_back(recorder.startAttempt("surplus"));
  rt::ResponseInit init;
  init.status = 200;
  const rt::Response response(nullptr, std::move(init));
  ASSERT(!handles.empty());  // the recorder must return a handle for every attempt
  const SentinelUpstreamAttemptHandle& lastHandle = handles.back();
  EXPECT(lastHandle.wrap(response) == response);  // over-bound attempts must be no-ops
  const SentinelUpstreamTrace traced = recorder.snapshotAndSeal();
  EXPECT_EQ(static_cast<std::int64_t>(traced.attempts.size()), SENTINEL_UPSTREAM_MAX_ATTEMPTS);
  EXPECT_EQ(traced.attempts_truncated, true);
  recorder.dispose();

  SentinelUpstreamRecorder byteRecorder = createSentinelUpstreamRecorder();
  // Two chunks whose total exceeds the byte bound by exactly one byte, so the
  // retained prefix is the bound and the omission is reported.
  const auto halfBound = static_cast<std::size_t>(rt::Math::ceil(static_cast<double>(SENTINEL_UPSTREAM_MAX_BYTES) / 2));
  auto bigChunks = std::make_shared<std::deque<rt::ByteChunk>>();
  bigChunks->emplace_back(halfBound, std::uint8_t{0x61});
  bigChunks->emplace_back(static_cast<std::size_t>(SENTINEL_UPSTREAM_MAX_BYTES) - halfBound + 1, std::uint8_t{0x62});
  rt::UnderlyingSource<rt::ByteChunk> bigUnderlying;
  bigUnderlying.pull = [bigChunks](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
    if (bigChunks->empty()) {
      controller.close();
    } else {
      rt::ByteChunk next = std::move(bigChunks->front());
      bigChunks->pop_front();
      controller.enqueue(std::move(next));
    }
  };
  rt::ByteStream bigSource(std::move(bigUnderlying), zeroHighWaterMark());
  const rt::Response bigWrapped =
    byteRecorder.startAttempt("chatgpt_codex").wrap(rt::Response(rt::BodyInit(std::move(bigSource)), responseInit(200, {{"Content-Type", "application/json"}})));
  const auto bigReader = responseBody(bigWrapped).getReader();
  for (;;) {
    const auto step = co_await bigReader.read();
    if (step.done) break;
  }
  const SentinelUpstreamTrace byteTrace = byteRecorder.snapshotAndSeal();
  EXPECT_EQ(byteTrace.bytes_truncated, true);
  EXPECT_EQ(decodedByteLength(firstAttempt(byteTrace)), static_cast<std::size_t>(SENTINEL_UPSTREAM_MAX_BYTES));
  EXPECT_EQ(firstAttempt(byteTrace).chunks_base64.size(), std::size_t{2});
  EXPECT_EQ(byteTrace.chunks_truncated, false);
  byteRecorder.dispose();

  SentinelUpstreamRecorder chunkRecorder = createSentinelUpstreamRecorder();
  rt::UnderlyingSource<rt::ByteChunk> manyUnderlying;
  manyUnderlying.pull = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { controller.enqueue(rt::ByteChunk{0x61}); };
  manyUnderlying.cancel = [](rt::Reason) {};
  rt::ByteStream manyChunks(std::move(manyUnderlying), zeroHighWaterMark());
  const rt::Response manyWrapped =
    chunkRecorder.startAttempt("metered").wrap(rt::Response(rt::BodyInit(std::move(manyChunks)), responseInit(200, {{"Content-Type", "text/event-stream"}})));
  const auto manyReader = responseBody(manyWrapped).getReader();
  std::int64_t readCount = 0;
  while (readCount < SENTINEL_UPSTREAM_MAX_CHUNKS + 1) {
    const auto step = co_await manyReader.read();
    if (step.done) break;
    readCount += 1;
  }
  const rt::Reason testComplete = rt::makeReason(rt::DOMException("test complete", "AbortError"));
  co_await manyReader.cancel(testComplete);
  const SentinelUpstreamTrace chunkTrace = chunkRecorder.snapshotAndSeal();
  EXPECT_EQ(static_cast<std::int64_t>(firstAttempt(chunkTrace).chunks_base64.size()), SENTINEL_UPSTREAM_MAX_CHUNKS);
  EXPECT_EQ(chunkTrace.chunks_truncated, true);
  chunkRecorder.dispose();
  // The sealed trace survives disposal (the base64 snapshot is immutable).
  EXPECT_EQ(firstAttempt(byteTrace).chunks_base64.size(), std::size_t{2});
}

ASYNC_TEST("recorder bounds: simultaneous chunk and byte exhaustion discloses both limits") {
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  auto chunkIndex = std::make_shared<std::int64_t>(0);
  rt::UnderlyingSource<rt::ByteChunk> underlying;
  underlying.pull = [chunkIndex](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
    // An exact even split: MAX_CHUNKS retained chunks exhaust the byte
    // bound exactly, so the next omitted chunk trips both permanent flags.
    const auto chunkBytes = static_cast<std::size_t>(SENTINEL_UPSTREAM_MAX_BYTES / SENTINEL_UPSTREAM_MAX_CHUNKS);
    if (*chunkIndex < SENTINEL_UPSTREAM_MAX_CHUNKS) controller.enqueue(rt::ByteChunk(chunkBytes, std::uint8_t{0x41}));
    else if (*chunkIndex == SENTINEL_UPSTREAM_MAX_CHUNKS) controller.enqueue(rt::ByteChunk{0x42});
    else controller.close();
    *chunkIndex += 1;
  };
  rt::ByteStream source(std::move(underlying), zeroHighWaterMark());
  const rt::Response wrapped = recorder.startAttempt("metered").wrap(rt::Response(rt::BodyInit(std::move(source)), responseInit(200, {{"Content-Type", "application/json"}})));
  const rt::Bytes delivered = co_await wrapped.arrayBuffer();
  EXPECT_EQ(delivered.size(), static_cast<std::size_t>(SENTINEL_UPSTREAM_MAX_BYTES + 1));  // capture bounds must not truncate the original response
  const SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
  const SentinelUpstreamAttempt& attempt = firstAttempt(trace);
  EXPECT_EQ(static_cast<std::int64_t>(attempt.chunks_base64.size()), SENTINEL_UPSTREAM_MAX_CHUNKS);
  EXPECT_EQ(decodedByteLength(attempt), static_cast<std::size_t>(SENTINEL_UPSTREAM_MAX_BYTES));
  EXPECT_EQ(trace.chunks_truncated, true);  // chunk limit is exhausted
  EXPECT_EQ(trace.bytes_truncated, true);   // the byte limit is exhausted on the same omitted chunk too
  EXPECT_EQ(attempt.terminal, "eof");
  EXPECT(parseSentinelUpstreamTrace(trace) == trace);
  recorder.dispose();
}

// Object.freeze makes `attempts.push` and `bytes_truncated = true` throw TypeError in
// JS. A sealed C++ trace is returned by value: mutating the caller's copy cannot
// reach the recorder, whose later snapshots stay the sealed one.
ASYNC_TEST("sealed snapshot is immutable and repeated snapshots return the same trace") {
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  recorder.startAttempt("chatgpt_codex").wrap(rt::Response(std::string("ok"), responseInit(200, {{"Content-Type", "application/json"}})));
  const SentinelUpstreamTrace first = recorder.snapshotAndSeal();
  const SentinelUpstreamTrace second = recorder.snapshotAndSeal();
  EXPECT(first == second);
  SentinelUpstreamTrace mutated = first;
  mutated.attempts.push_back(SentinelUpstreamAttempt());
  mutated.bytes_truncated = true;
  EXPECT(recorder.snapshotAndSeal() == second);
  // After sealing, the wrapper still forwards but retains nothing.
  const rt::Response postSeal = recorder.startAttempt("surplus").wrap(rt::Response(std::string("later"), responseInit(200, {{"Content-Type", "application/json"}})));
  EXPECT_EQ(postSeal.status(), 200);
  recorder.dispose();
  EXPECT(recorder.snapshotAndSeal() == first);
  co_return;
}

ASYNC_TEST("canonical upstream JSON is deterministic and matches the v2 frame encoding") {
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  rt::UnderlyingSource<rt::ByteChunk> underlying;
  underlying.pull = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
    controller.enqueue(rt::encodeUtf8("a"));
    controller.close();
  };
  rt::ByteStream source(std::move(underlying), zeroHighWaterMark());
  const rt::Response wrapped = recorder.startAttempt("chatgpt_codex").wrap(rt::Response(rt::BodyInit(std::move(source)), responseInit(200, {{"Content-Type", "application/json"}})));
  const auto reader = responseBody(wrapped).getReader();
  for (;;) {
    const auto step = co_await reader.read();
    if (step.done) break;
  }
  const SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
  const std::string canonical = canonicalSentinelUpstreamJson(trace);
  EXPECT_EQ(canonicalSentinelUpstreamJson(trace), canonical);
  // The recorder now seals a version-2 attempt: the frozen keys keep their
  // exact frame encoding and the new timing/header evidence follows the same
  // recursive key ordering.
  const std::string frozenV1 =
    R"({"attempts":[{"chunks_base64":["YQ=="],"content_type":"application/json","provider":"chatgpt_codex","status":200,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":1})";
  EXPECT_EQ(canonicalSentinelUpstreamJson(parseSentinelUpstreamTrace(rt::jsonParse(frozenV1))), frozenV1);
  EXPECT_EQ(rt::startsWith(canonical, R"({"attempts":[{"chunk_times_ms":[)"), true);
  EXPECT_EQ(rt::includes(canonical, R"("headers":{"content-type":"application/json"})"), true);
  EXPECT_EQ(rt::endsWith(canonical, R"("version":2})"), true);
  // A different partial trace canonicalizes differently.
  SentinelUpstreamRecorder partial = createSentinelUpstreamRecorder();
  partial.startAttempt("chatgpt_codex").wrap(rt::Response(std::string("ab"), responseInit(200, {{"Content-Type", "application/json"}})));
  const SentinelUpstreamTrace partialTrace = partial.snapshotAndSeal();
  EXPECT(canonicalSentinelUpstreamJson(partialTrace) != canonical);
  recorder.dispose();
  partial.dispose();
}

namespace {
  using uos::sentinel::replay_capture::AcceptedSentinelReplayInput;
  using uos::sentinel::replay_capture::decryptExportedSentinelReplay;
  using uos::sentinel::replay_capture::ExportedSentinelReplayCapture;
  using uos::sentinel::replay_capture::persistEncryptedSentinelReplay;
  using uos::sentinel::replay_capture::SENTINEL_REPLAY_CHUNK_PREFIX;
  using uos::sentinel::replay_capture::SentinelFailureObservation;
  using uos::sentinel::replay_model::SentinelReplayPlaintext;
  using uos::sentinel::replay_observation::PersistDependencies;
  using uos::sentinel::replay_observation::SentinelReplayPersistResult;
  using uos::sentinel::replay_observation::SentinelReplayPersistResultStored;

  // `const kv = await openKv(":memory:"); setKvForTest(kv); try { ... } finally { kv.close(); setKvForTest(null); }`
  struct InstalledStore {
    std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
    InstalledStore() { uos::kv::setKvForTest(kv); }
    ~InstalledStore() {
      kv->close();
      uos::kv::setKvForTest(nullptr);
    }
    InstalledStore(const InstalledStore&) = delete;
    InstalledStore& operator=(const InstalledStore&) = delete;
  };

  // crypto.getRandomValues(new Uint8Array(32)).slice()
  rt::Bytes randomKeyBytes() {
    rt::Bytes keyBytes(32);
    rt::crypto::getRandomValues(std::span<std::uint8_t>(keyBytes));
    return keyBytes;
  }

  // encoder.encode(text)
  std::shared_ptr<rt::Bytes> encoded(std::string_view text) { return std::make_shared<rt::Bytes>(rt::encodeUtf8(text)); }

  SentinelFailureObservation failureObservation() {
    return {
      .status = 502,
      .stream = false,
      .completed = false,
      .terminal_type = "http.error",
      .failure_kind = "upstream_timeout",
      .synthetic_terminal_type = std::nullopt,
      .provider_route = "test-provider",
    };
  }

  AcceptedSentinelReplayInput syntheticInput(std::shared_ptr<rt::Bytes> bytes, const std::string& requestId) {
    return {
      .endpoint = "/v1/responses",
      .method = "POST",
      .body = std::move(bytes),
      .content_type = "application/json",
      .compatibility_headers = {},
      .request_id = requestId,
      .git_sha = "synthetic-git-sha",
      .deno_revision = "synthetic-revision",
    };
  }

  // `{ kv, keyBytes, now: () => 1_700_000_000_000, randomUuid: () => captureId }`
  PersistDependencies dependenciesFor(std::shared_ptr<rt::Kv> kv, const rt::Bytes& keyBytes, std::string captureId) {
    return {
      .kv = std::move(kv),
      .keyBytes = keyBytes,
      .now = [] { return 1'700'000'000'000.0; },
      .randomUuid = [captureId] { return captureId; },
    };
  }

  /** The stored arm of a persist result, which the test asserted (`assert.equal(stored.status, "stored")`). */
  const SentinelReplayPersistResultStored& storedOf(const SentinelReplayPersistResult& result) {
    ASSERT(std::holds_alternative<SentinelReplayPersistResultStored>(result));
    return std::get<SentinelReplayPersistResultStored>(result);
  }

  rt::Task<ExportedSentinelReplayCapture> exportRoundTrip(SentinelReplayPersistResultStored stored, std::shared_ptr<rt::Kv> kv) {
    std::vector<rt::Bytes> rows;
    for (double index = 0; index < stored.manifest.chunk_count; index += 1) {
      auto reading = kv->get(rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, stored.manifest.capture_id, index});
      const rt::KvEntryMaybe<> row = co_await std::move(reading);
      if (row.value.isNull()) throw rt::JsError("missing replay chunk");
      rows.push_back(row.value.asBytes());
    }
    std::vector<std::string> chunks;
    for (const rt::Bytes& row : rows) chunks.push_back(base64UrlEncode(row));
    co_return ExportedSentinelReplayCapture{.manifest = stored.manifest, .chunks = std::move(chunks)};
  }

  // `terminal`: "eof" | "cancelled". The recorder is left undisposed, as the TS leaves it.
  rt::Task<SentinelUpstreamTrace> buildRecordedTrace(std::string terminal) {
    SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
    rt::UnderlyingSource<rt::ByteChunk> underlying;
    underlying.pull = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      controller.enqueue(rt::encodeUtf8("data: {\"type\":\"response.created\"}\n\n"));
      controller.enqueue(rt::encodeUtf8("data: {\"type\":\"response.completed\"}\n\n"));
      controller.close();
    };
    rt::ByteStream source(std::move(underlying), zeroHighWaterMark());
    const rt::Response wrapped =
      recorder.startAttempt("chatgpt_codex").wrap(rt::Response(rt::BodyInit(std::move(source)), responseInit(200, {{"Content-Type", "text/event-stream"}})));
    const auto reader = responseBody(wrapped).getReader();
    if (terminal == "eof") {
      for (;;) {
        const auto step = co_await reader.read();
        if (step.done) break;
      }
    } else {
      const auto first = co_await reader.read();
      (void)first;
      const rt::Reason reason = rt::makeReason(rt::DOMException("client cancelled", "AbortError"));
      co_await reader.cancel(reason);
    }
    co_return recorder.snapshotAndSeal();
  }

  // `assert.rejects(promise)`: any rejection.
  template <class T> rt::Task<bool> rejects(rt::Task<T> task) {
    bool rejected = false;
    try {
      co_await std::move(task);
    } catch (...) {
      rejected = true;
    }
    co_return rejected;
  }
}

ASYNC_TEST("v2 fingerprint binds the exact upstream trace: partial vs complete differ, case-group stays v1-identical") {
  const InstalledStore installed;
  const std::shared_ptr<rt::Kv> kv = installed.kv;
  const rt::Bytes keyBytes = randomKeyBytes();
  const AcceptedSentinelReplayInput input =
    syntheticInput(encoded(R"({"model":"gpt-5.6-sol","stream":false,"input":"same-request"})"), "v2-fingerprint-binding");
  auto recordingComplete = buildRecordedTrace("eof");
  const SentinelUpstreamTrace complete = co_await std::move(recordingComplete);
  auto recordingPartial = buildRecordedTrace("cancelled");
  const SentinelUpstreamTrace partial = co_await std::move(recordingPartial);
  EXPECT_EQ(firstAttempt(complete).terminal, "eof");
  EXPECT_EQ(firstAttempt(partial).terminal, "cancelled");
  EXPECT(canonicalSentinelUpstreamJson(complete) != canonicalSentinelUpstreamJson(partial));
  AcceptedSentinelReplayInput completeInput = input;
  completeInput.upstream = complete;
  auto persistingComplete = persistEncryptedSentinelReplay(completeInput, failureObservation(), dependenciesFor(kv, keyBytes, "capture-complete"));
  const SentinelReplayPersistResult completeResult = co_await std::move(persistingComplete);
  AcceptedSentinelReplayInput partialInput = input;
  partialInput.upstream = partial;
  auto persistingPartial = persistEncryptedSentinelReplay(partialInput, failureObservation(), dependenciesFor(kv, keyBytes, "capture-partial"));
  const SentinelReplayPersistResult partialResult = co_await std::move(persistingPartial);
  const SentinelReplayPersistResultStored& storedComplete = storedOf(completeResult);
  const SentinelReplayPersistResultStored& storedPartial = storedOf(partialResult);  // partial and complete must not suppress one another
  EXPECT(storedComplete.manifest.fingerprint != storedPartial.manifest.fingerprint);
  // case-group identity must remain the request-only v1 HMAC
  EXPECT_EQ(storedComplete.manifest.case_group_digest, storedPartial.manifest.case_group_digest);
  auto exportingComplete = exportRoundTrip(storedComplete, kv);
  const ExportedSentinelReplayCapture completeExport = co_await std::move(exportingComplete);
  auto decryptingComplete = decryptExportedSentinelReplay(completeExport, keyBytes);
  const SentinelReplayPlaintext completePlaintext = co_await std::move(decryptingComplete);
  EXPECT_EQ(completePlaintext.version, 3.0);
  // This synthetic observation carries no observed downstream body, so the
  // capture reports that omission instead of claiming full coverage.
  EXPECT_EQ(completePlaintext.capture_status, std::optional<std::string>("incomplete"));
  EXPECT_EQ(completePlaintext.replay_coverage, std::optional<std::string>("partial"));
  EXPECT_EQ(completePlaintext.unavailable.has_value() && rt::includes(*completePlaintext.unavailable, std::string("downstream_terminal_body_unavailable")), true);
  EXPECT(completePlaintext.upstream == complete);
  auto exportingPartial = exportRoundTrip(storedPartial, kv);
  const ExportedSentinelReplayCapture partialExport = co_await std::move(exportingPartial);
  auto decryptingPartial = decryptExportedSentinelReplay(partialExport, keyBytes);
  const SentinelReplayPlaintext partialPlaintext = co_await std::move(decryptingPartial);
  EXPECT(partialPlaintext.upstream == partial);
}

ASYNC_TEST("strict v2 metadata: invalid upstream traces are rejected and tampered ciphertext fails closed") {
  const InstalledStore installed;
  const std::shared_ptr<rt::Kv> kv = installed.kv;
  const rt::Bytes keyBytes = randomKeyBytes();
  const AcceptedSentinelReplayInput input = syntheticInput(encoded(R"({"model":"gpt-5.6-sol","stream":false,"input":"tamper"})"), "v2-tamper");
  const auto encoderTrace = [] {
    SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
    recorder.startAttempt("chatgpt_codex").wrap(rt::Response(std::string("upstream-500"), responseInit(500, {{"Content-Type", "application/json"}})));
    const SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
    recorder.dispose();
    return trace;
  };
  const SentinelUpstreamTrace trace = encoderTrace();
  // A tainted upstream trace must fail closed at persistence.
  SentinelUpstreamTrace tainted = trace;
  ASSERT(!tainted.attempts.empty());
  tainted.attempts[0].status = std::nullopt;
  tainted.attempts[0].content_type = "application/json";
  AcceptedSentinelReplayInput taintedInput = input;
  taintedInput.upstream = tainted;
  auto tainting = persistEncryptedSentinelReplay(taintedInput, failureObservation(), dependenciesFor(kv, keyBytes, "capture-taint"));
  auto taintRejection = rejects(std::move(tainting));
  const bool taintRejected = co_await std::move(taintRejection);
  EXPECT(taintRejected);
  // A truthful capture round-trips, then an authenticated tamper fails closed.
  PersistDependencies tamperDependencies = dependenciesFor(kv, keyBytes, "capture-tamper");
  tamperDependencies.randomBytes = [](double) { return rt::Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}; };
  AcceptedSentinelReplayInput truthfulInput = input;
  truthfulInput.upstream = trace;
  auto persisting = persistEncryptedSentinelReplay(truthfulInput, failureObservation(), tamperDependencies);
  const SentinelReplayPersistResult result = co_await std::move(persisting);
  const SentinelReplayPersistResultStored& stored = storedOf(result);
  auto exporting = exportRoundTrip(stored, kv);
  const ExportedSentinelReplayCapture good = co_await std::move(exporting);
  ExportedSentinelReplayCapture tampered{.manifest = good.manifest, .chunks = {}};
  for (std::size_t index = 0; index < good.chunks.size(); ++index) {
    if (index != 0) {
      tampered.chunks.push_back(good.chunks[index]);
      continue;
    }
    std::vector<std::uint8_t> bytes = base64UrlDecode(good.chunks[index]);
    ASSERT(!bytes.empty());
    bytes[0] = static_cast<std::uint8_t>(bytes[0] ^ 0xffU);
    tampered.chunks.push_back(base64UrlEncode(bytes));
  }
  auto decrypting = decryptExportedSentinelReplay(tampered, keyBytes);
  auto tamperRejection = rejects(std::move(decrypting));
  const bool tamperRejected = co_await std::move(tamperRejection);
  EXPECT(tamperRejected);
}

ASYNC_TEST("request-only inputs emit the required empty upstream trace and never claim coverage") {
  const SentinelUpstreamTrace empty = emptySentinelUpstreamTrace();
  EXPECT(empty.attempts.empty());
  EXPECT_EQ(empty.attempts_truncated, false);
  EXPECT_EQ(empty.bytes_truncated, false);
  EXPECT_EQ(empty.chunks_truncated, false);
  EXPECT_EQ(rt::includes(rt::jsonStringify(json::value_from(empty)), "captured"), false);
  const InstalledStore installed;
  const std::shared_ptr<rt::Kv> kv = installed.kv;
  const rt::Bytes keyBytes = randomKeyBytes();
  const AcceptedSentinelReplayInput input = syntheticInput(encoded(R"({"input":"request-only"})"), "request-only-empty-trace");
  auto persisting = persistEncryptedSentinelReplay(input, failureObservation(), dependenciesFor(kv, keyBytes, "request-only-capture"));
  const SentinelReplayPersistResult result = co_await std::move(persisting);
  auto exporting = exportRoundTrip(storedOf(result), kv);
  const ExportedSentinelReplayCapture exported = co_await std::move(exporting);
  auto decrypting = decryptExportedSentinelReplay(exported, keyBytes);
  const SentinelReplayPlaintext plaintext = co_await std::move(decrypting);
  EXPECT(plaintext.upstream == empty);
}
