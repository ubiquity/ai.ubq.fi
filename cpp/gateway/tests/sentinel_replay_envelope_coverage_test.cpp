// Port of tests/sentinel-replay-envelope-coverage.test.ts (every case).
// Links: tests/sentinel_replay_envelope_coverage_test.cpp src/sentinel/replay_model.cpp src/sentinel/replay_envelope.cpp
//        src/sentinel/replay_observation.cpp src/sentinel/upstream_capture.cpp src/sentinel/replay_limits.cpp src/request.cpp
//        src/utils.cpp src/config.cpp src/release.cpp
// `JSON.stringify(metadata)` of the TS fixture is the literal BASE_METADATA_JSON, recorded from Bun.
#include "uos/request.hpp"
#include "uos/sentinel/replay_envelope.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"
#include "uos/sentinel/upstream_capture.hpp"
#include "uos/utils.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace request = uos::request;
  namespace utils = uos::utils;
  using namespace uos::sentinel::replay_model;
  using namespace uos::sentinel::replay_envelope;
  using namespace uos::sentinel::replay_observation;
  using namespace uos::sentinel::upstream_capture;

  // assert.rejects(fn, /pattern/): node:assert matches String(error).
  template <class T> rt::Task<void> expectRejectsMatching(rt::Task<T> task, std::string pattern, std::string flags, const char* text, const char* file, int line) {
    std::optional<std::string> thrown;
    try {
      (void)co_await std::move(task);
    } catch (const rt::JsError& error) {
      thrown = error.toString();
    }
    if (!thrown) {
      rt::test::report(file, line, std::string("expected ") + text + " to reject", false);
    } else if (!rt::Regex(pattern, flags).test(*thrown)) {
      rt::test::report(file, line, std::string(text) + " rejected with " + rt::test::show(*thrown) + ", which does not match /" + pattern + "/", false);
    }
  }

  rt::Bytes encode(std::string_view text) { return rt::encodeUtf8(text); }
  std::shared_ptr<rt::Bytes> sharedBytes(std::string_view text) { return std::make_shared<rt::Bytes>(rt::encodeUtf8(text)); }
  bool allZero(const rt::Bytes& bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; });
  }
  std::uint64_t bigUint64(const rt::Bytes& bytes) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) value = (value << 8) | bytes.at(index);
    return value;
  }

  constexpr double NOW = 1'800'000'000'000;
  const std::string GIT_SHA(40, 'c');
  const rt::Bytes KEY_BYTES(32, 0x11);
  /** Documentation-range client address (RFC 5737 TEST-NET-2), exactly as tests/images.test.ts fixtures use it. */
  constexpr std::string_view CLIENT_ADDRESS = "198.51.100.7";

  const std::string BASE_METADATA_JSON =
    R"({"version":3,"captured_at_ms":1800000000000,"endpoint":"/v1/responses","method":"POST","content_type":"application/json","compatibility_headers":{},)"
    R"("failure_signature":"{\"status\":502}","observation":{"status":502,"stream":true,"completed":false,"terminal_type":"http.error",)"
    R"("failure_kind":"upstream_timeout","synthetic_terminal_type":null,"provider_route":"test-provider"},"client_observation":{"status":502,)"
    R"("stream":true,"completed":false,"terminal_type":"http.error","failure_kind":"upstream_timeout","framing_valid":true,"provider_route":"test-provider",)"
    R"("error_code":"upstream_error","error_param":null,"terminal_body_base64":"dXBzdHJlYW0gZmFpbGVk","terminal_body_truncated":false},)"
    R"("request_id":"envelope-request","git_sha":"cccccccccccccccccccccccccccccccccccccccc","deno_revision":"envelope-revision",)"
    R"("upstream":{"version":2,"attempts":[],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false},)"
    R"("settings":{"source":"recorded_request_body","provider_route":"test-provider","model_requested":"gpt-5","reasoning_requested":null,)"
    R"("stream_requested":true,"stream_observed":true},"capture_status":"ready","replay_coverage":"full","unavailable":[],)"
    R"("body_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","body_bytes":4,"downstream":{"terminal_type":"http.error",)"
    R"("failure_kind":"upstream_timeout","error_code":"upstream_error","error_param":null,"body_base64":"dXBzdHJlYW0gZmFpbGVk","body_truncated":false}})";

  SentinelFailureObservation internalObservation(const std::function<void(SentinelFailureObservation&)>& overrides = {}) {
    SentinelFailureObservation observation{
      .status = 502,
      .stream = true,
      .completed = false,
      .terminal_type = "http.error",
      .failure_kind = "upstream_timeout",
      .synthetic_terminal_type = std::nullopt,
      .provider_route = "test-provider",
    };
    if (overrides) overrides(observation);
    return observation;
  }

  SentinelClientFailureObservation clientObservation(const std::function<void(SentinelClientFailureObservation&)>& overrides = {}) {
    SentinelClientFailureObservation observation{
      .status = 502,
      .stream = true,
      .completed = false,
      .terminal_type = "http.error",
      .failure_kind = "upstream_timeout",
      .framing_valid = true,
      .provider_route = "test-provider",
      .error_code = "upstream_error",
      .error_param = std::nullopt,
      .terminal_body_base64 = rt::btoa("upstream failed"),
      .terminal_body_truncated = false,
    };
    if (overrides) overrides(observation);
    return observation;
  }

  AcceptedSentinelReplayInput acceptedInput(std::string_view body, const std::string& requestId) {
    return {
      .endpoint = "/v1/responses",
      .method = "POST",
      .body = sharedBytes(body),
      .content_type = "application/json",
      .compatibility_headers = json::object(),
      .request_id = requestId,
      .git_sha = GIT_SHA,
      .deno_revision = "envelope-revision",
    };
  }

  ReplayMetadata baseMetadata() {
    const SentinelClientFailureObservation client = clientObservation();
    return {
      .version = 3,
      .captured_at_ms = NOW,
      .endpoint = "/v1/responses",
      .method = "POST",
      .content_type = "application/json",
      .compatibility_headers = json::object(),
      .failure_signature = R"({"status":502})",
      .observation = internalObservation(),
      .client_observation = client,
      .request_id = "envelope-request",
      .git_sha = GIT_SHA,
      .deno_revision = "envelope-revision",
      .upstream = emptySentinelUpstreamTrace(),
      .settings =
        SentinelReplaySettings{
          .source = "recorded_request_body",
          .provider_route = "test-provider",
          .model_requested = "gpt-5",
          .reasoning_requested = std::nullopt,
          .stream_requested = true,
          .stream_observed = true,
        },
      .capture_status = "ready",
      .replay_coverage = "full",
      .unavailable = std::vector<std::string>{},
      .body_sha256 = std::string(64, 'a'),
      .body_bytes = 4,
      .downstream = downstreamObservation(client),
    };
  }

  json::object baseJson() { return json::value_from(baseMetadata()).as_object(); }

  /** The thirteen keys every plaintext version shares, in the order the envelope writes them. */
  constexpr std::string_view REPLAY_SHARED_KEYS[] = {
    "version",     "captured_at_ms",     "endpoint",   "method",  "content_type",  "compatibility_headers", "failure_signature",
    "observation", "client_observation", "request_id", "git_sha", "deno_revision", "upstream",
  };

  /** A version-2 plaintext body: the 13 shared keys, no version-3 additions. */
  json::object baseV2Metadata() {
    const json::object sealed = baseJson();
    json::object shared;
    for (const std::string_view key : REPLAY_SHARED_KEYS) rt::objectDefine(shared, key, *rt::get(sealed, key));
    rt::objectDefine(shared, "version", json::value(REPLAY_PLAINTEXT_VERSION_V2));
    json::object client;
    client["status"] = 502;
    client["stream"] = true;
    client["completed"] = false;
    client["terminal_type"] = "http.error";
    client["failure_kind"] = "upstream_timeout";
    client["framing_valid"] = true;
    client["provider_route"] = "test-provider";
    rt::objectDefine(shared, "client_observation", json::value(client));
    return rt::canonicalNumbers(json::value(shared)).as_object();
  }

  // { ...base, key: value }; std::nullopt is `key: undefined` (dropped by JSON.stringify).
  json::object with(json::object base, std::string_view key, std::optional<json::value> value) {
    if (value) rt::objectDefine(base, key, *value);
    else rt::objectDelete(base, key);
    return base;
  }
  // { ...base, key: { ...base[key], member: value } }
  json::object withMember(const json::object& base, std::string_view key, std::string_view member, std::optional<json::value> value) {
    return with(base, key, json::value(with(rt::get(base, key)->as_object(), member, std::move(value))));
  }

  rt::Bytes defaultCandidateBody() { return rt::Bytes{1, 2, 3, 4}; }

  SentinelReplayPlaintext decodeCandidate(const json::value& metadata, const rt::Bytes& body = defaultCandidateBody()) {
    const json::value untrusted = rt::jsonParse(rt::jsonStringify(metadata));
    return decodePlaintext(encodePlaintext(untrusted, body));
  }

  SentinelUpstreamTrace v1Trace(const SentinelUpstreamAttempt& attempt) {
    return {.version = 1, .attempts = {attempt}, .attempts_truncated = false, .bytes_truncated = false, .chunks_truncated = false};
  }

  SentinelUpstreamAttempt cerebrasAttempt(std::vector<std::string> chunks) {
    return {
      .provider = "cerebras",
      .status = 200,
      .content_type = "application/json",
      .chunks_base64 = std::move(chunks),
      .terminal = "eof",
      .headers = std::nullopt,
      .headers_truncated = std::nullopt,
      .chunk_times_ms = std::nullopt,
      .started_at_ms = std::nullopt,
      .headers_at_ms = rt::undefined,
      .ended_at_ms = rt::undefined,
    };
  }

  std::vector<std::size_t> sizes(const std::vector<rt::Bytes>& chunks) {
    std::vector<std::size_t> out;
    for (const rt::Bytes& chunk : chunks) out.push_back(chunk.size());
    return out;
  }

  rt::Bytes jsonBytes(const json::value& value) { return encode(rt::jsonStringify(value)); }

  rt::Request newRequest(std::string_view url, std::string method, rt::Headers headers = {}) {
    rt::RequestInit init;
    init.method = std::move(method);
    init.headers = std::move(headers);
    return rt::Request(url, std::move(init));
  }

  rt::Response response(rt::BodyInit body, double status, rt::Headers headers = {}) {
    rt::ResponseInit init;
    init.status = status;
    init.headers = std::move(headers);
    return rt::Response(std::move(body), std::move(init));
  }

  SentinelClientBodyObservation bodyObservation(bool stream, bool completed, std::optional<std::string> terminalType, std::optional<std::string> failureKind,
                                                bool framingValid) {
    return {.stream = stream, .completed = completed, .terminal_type = std::move(terminalType), .failure_kind = std::move(failureKind), .framing_valid = framingValid};
  }
}

TEST("the replay key decoder accepts only one canonical 32-byte key") {
  const std::optional<rt::Bytes> decoded = decodeSentinelReplayKey(rt::repeat("A", 43));
  ASSERT(decoded.has_value());
  EXPECT_EQ(static_cast<double>(decoded->size()), REPLAY_KEY_BYTES);
  EXPECT_EQ(*decoded, rt::Bytes(32, 0));
  const std::optional<rt::Bytes> padded = decodeSentinelReplayKey(rt::repeat("A", 43) + "=");
  EXPECT(padded.has_value() && static_cast<double>(padded->size()) == REPLAY_KEY_BYTES);
  EXPECT(!decodeSentinelReplayKey(""));
  EXPECT(!decodeSentinelReplayKey(rt::repeat("A", 42)));
  EXPECT(!decodeSentinelReplayKey(rt::repeat("A", 44)));
  EXPECT(!decodeSentinelReplayKey(rt::repeat("A", 43) + "=="));
  EXPECT(!decodeSentinelReplayKey(rt::repeat("A", 43) + "+"));
  EXPECT(!decodeSentinelReplayKey(rt::repeat("A", 42) + "/A"));
}

TEST("the encoded envelope is a length-prefixed metadata frame followed by the exact body") {
  const ReplayMetadata metadata = baseMetadata();
  const rt::Bytes body = encode("body-bytes");
  const rt::Bytes encoded = encodePlaintext(metadata, body);
  const rt::Bytes metadataBytes = encode(BASE_METADATA_JSON);
  EXPECT_EQ(encoded.size(), 4 + metadataBytes.size() + body.size());
  const std::uint32_t declared = (std::uint32_t{encoded[0]} << 24) | (std::uint32_t{encoded[1]} << 16) | (std::uint32_t{encoded[2]} << 8) | encoded[3];
  EXPECT_EQ(declared, metadataBytes.size());
  EXPECT_EQ(rt::decodeUtf8(std::span<const std::uint8_t>(encoded).subspan(4, metadataBytes.size())), BASE_METADATA_JSON);

  const SentinelReplayPlaintext plaintext = decodePlaintext(encoded);
  EXPECT_EQ(plaintext.captured_at_ms, NOW);
  EXPECT_EQ(plaintext.request_id, "envelope-request");
  EXPECT_EQ(*plaintext.body, body);
  EXPECT_EQ(plaintext.body_bytes, std::optional<double>(4));
  EXPECT(plaintext.upstream == emptySentinelUpstreamTrace());
  EXPECT_EQ(plaintext.settings ? std::optional<std::string>(plaintext.settings->source) : std::nullopt, std::optional<std::string>("recorded_request_body"));
  EXPECT_EQ(plaintext.downstream ? plaintext.downstream->terminal_type : std::nullopt, std::optional<std::string>("http.error"));

  // A version-2 envelope still decodes: it carries the shared keys only.
  const SentinelReplayPlaintext v2 = decodePlaintext(encodePlaintext(json::value(baseV2Metadata()), body));
  EXPECT_EQ(v2.version, REPLAY_PLAINTEXT_VERSION_V2);
  EXPECT_EQ(*v2.body, body);
}

TEST("envelope size faults are rejected before any metadata is interpreted") {
  EXPECT_THROWS_MATCHING(decodePlaintext((rt::Bytes{0, 0, 0})), "Sentinel replay envelope size is invalid");
  EXPECT_THROWS_MATCHING(decodePlaintext(rt::Bytes(static_cast<std::size_t>(MAX_REPLAY_PLAINTEXT_BYTES) + 1)), "Sentinel replay envelope size is invalid");

  const auto bigEndian = [](std::uint32_t value) {
    return rt::Bytes{static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8),
                     static_cast<std::uint8_t>(value)};
  };
  const rt::Bytes oversizedMetadata = bigEndian(static_cast<std::uint32_t>(MAX_REPLAY_METADATA_BYTES) + 1);
  EXPECT_THROWS_MATCHING(decodePlaintext(oversizedMetadata), "Sentinel replay metadata is too large");

  const rt::Bytes truncatedFrame = bigEndian(10);
  EXPECT_THROWS_MATCHING(decodePlaintext(truncatedFrame), "Sentinel replay metadata length is invalid");

  rt::Bytes oversizedBody(4 + 2 + static_cast<std::size_t>(SENTINEL_REPLAY_MAX_BODY_BYTES) + 1);
  oversizedBody[3] = 2;
  EXPECT_THROWS_MATCHING(decodePlaintext(oversizedBody), "Sentinel replay body is too large");

  rt::Bytes notJson{0, 0, 0, 8};
  for (const std::uint8_t byte : encode("not json")) notJson.push_back(byte);
  EXPECT_THROWS(decodePlaintext(notJson), rt::SyntaxError);

  rt::Bytes emptyObject{0, 0, 0, 2};
  for (const std::uint8_t byte : encode("{}")) emptyObject.push_back(byte);
  EXPECT_THROWS_MATCHING(decodePlaintext(emptyObject), "Sentinel replay metadata is invalid");

  // JSON that is not an object at all is refused before any field is read.
  rt::Bytes scalarMetadata{0, 0, 0, 6};
  for (const std::uint8_t byte : encode("\"text\"")) scalarMetadata.push_back(byte);
  EXPECT_THROWS_MATCHING(decodePlaintext(scalarMetadata), "Sentinel replay metadata is invalid");
  rt::Bytes arrayMetadata{0, 0, 0, 2};
  for (const std::uint8_t byte : encode("[]")) arrayMetadata.push_back(byte);
  EXPECT_THROWS_MATCHING(decodePlaintext(arrayMetadata), "Sentinel replay metadata is invalid");

  // Encoding refuses what decoding would refuse.
  ReplayMetadata longEndpoint = baseMetadata();
  longEndpoint.endpoint = std::string(static_cast<std::size_t>(MAX_REPLAY_METADATA_BYTES), 'e');
  EXPECT_THROWS_MATCHING(encodePlaintext(longEndpoint, rt::Bytes(1)), "Sentinel replay metadata is too large");
  EXPECT_THROWS_MATCHING(encodePlaintext(baseMetadata(), rt::Bytes(static_cast<std::size_t>(SENTINEL_REPLAY_MAX_BODY_BYTES) + 1)), "Sentinel replay body is too large");
}

TEST("plaintext metadata is accepted exactly when every version-3 field is in contract") {
  const SentinelReplayPlaintext valid = decodeCandidate(json::value(baseJson()));
  EXPECT_EQ(valid.version, 3);
  EXPECT_EQ(decodeCandidate(json::value(baseV2Metadata())).version, REPLAY_PLAINTEXT_VERSION_V2);

  const json::object base = baseJson();
  const auto withVersion = [&base](json::value version) { return with(base, "version", std::move(version)); };
  json::object client = json::value_from(clientObservation()).as_object();
  json::object disallowedHeader;
  disallowedHeader["x-forwarded-for"] = std::string(CLIENT_ADDRESS);
  json::object paddedHeader;
  paddedHeader["accept"] = " application/json";
  json::object emptyHeader;
  emptyHeader["accept"] = "";
  json::object invalidObservation;
  invalidObservation["status"] = "502";
  json::object badTrace;
  badTrace["bad"] = true;
  json::array unknownReason{"not_a_reason"};
  const std::vector<std::pair<std::string, json::object>> variants = {
    {"unknown version", withVersion(4)},
    {"missing settings", with(base, "settings", std::nullopt)},
    {"unavailable settings source", withMember(base, "settings", "source", json::value("guessed"))},
    {"unbounded model", withMember(base, "settings", "model_requested", json::value("has spaces"))},
    {"non-boolean stream request", withMember(base, "settings", "stream_requested", json::value("yes"))},
    {"unknown capture status", with(base, "capture_status", json::value("maybe"))},
    {"unknown coverage", with(base, "replay_coverage", json::value("mostly"))},
    {"unknown unavailable reason", with(base, "unavailable", json::value(unknownReason))},
    {"unavailable reasons not an array", with(base, "unavailable", json::value("upstream_trace_empty"))},
    {"short body digest", with(base, "body_sha256", json::value("abc"))},
    {"negative body bytes", with(base, "body_bytes", json::value(-1))},
    {"fractional body bytes", with(base, "body_bytes", json::value(1.5))},
    {"downstream body flag missing", withMember(base, "downstream", "body_truncated", std::nullopt)},
    {"negative capture time", with(base, "captured_at_ms", json::value(-1))},
    {"non-string endpoint", with(base, "endpoint", json::value(7))},
    {"non-string method", with(base, "method", json::value(nullptr))},
    {"non-string content type", with(base, "content_type", json::value(7))},
    {"disallowed compatibility header", with(base, "compatibility_headers", json::value(disallowedHeader))},
    {"padded compatibility header value", with(base, "compatibility_headers", json::value(paddedHeader))},
    {"empty compatibility header value", with(base, "compatibility_headers", json::value(emptyHeader))},
    {"non-string failure signature", with(base, "failure_signature", json::value(3))},
    {"invalid observation", with(base, "observation", json::value(invalidObservation))},
    {"observation missing provider route", withMember(base, "observation", "provider_route", json::value(3))},
    {"client observation missing framing", with(base, "client_observation", json::value(with(client, "framing_valid", std::nullopt)))},
    {"client observation with unbounded error code", with(base, "client_observation", json::value(with(client, "error_code", json::value("has spaces"))))},
    {"client observation with unbounded error param", with(base, "client_observation", json::value(with(client, "error_param", json::value("has spaces"))))},
    {"client observation with non-string body", with(base, "client_observation", json::value(with(client, "terminal_body_base64", json::value(7))))},
    {"client observation without truncation flag", with(base, "client_observation", json::value(with(client, "terminal_body_truncated", std::nullopt)))},
    {"non-string request id", with(base, "request_id", json::value(7))},
    {"non-string git sha", with(base, "git_sha", json::value(7))},
    {"non-string deno revision", with(base, "deno_revision", json::value(7))},
    {"invalid upstream trace", with(base, "upstream", json::value(badTrace))},
    {"extra field", with(base, "extra", json::value(1))},
    {"missing version-3 field", with(base, "body_bytes", std::nullopt)},
  };
  for (const auto& [label, metadata] : variants) {
    bool matched = false;
    try {
      (void)decodeCandidate(json::value(metadata));
    } catch (const rt::JsError& error) {
      matched = rt::Regex("Sentinel replay metadata is invalid").test(error.toString());
    }
    if (!matched) rt::test::report(__FILE__, __LINE__, "variant was not rejected as invalid metadata: " + label, false);
  }

  // The version-2 key set is exact as well.
  EXPECT_THROWS_MATCHING(decodeCandidate(json::value(with(baseV2Metadata(), "body_bytes", json::value(4)))), "Sentinel replay metadata is invalid");
  EXPECT_THROWS_MATCHING(decodeCandidate(json::value(withVersion(1))), "Sentinel replay metadata is invalid");
}

TEST("request settings are read back from the recorded body, or declared unavailable") {
  const SentinelReplaySettings recorded = requestSettingsFromBody(encode(R"({"model":"gpt-5","stream":true})"), "test-provider", true);
  EXPECT_EQ(recorded.source, "recorded_request_body");
  EXPECT_EQ(recorded.model_requested, std::optional<std::string>("gpt-5"));
  EXPECT_EQ(recorded.stream_requested, std::optional<bool>(true));
  EXPECT_EQ(recorded.stream_observed, std::optional<bool>(true));
  EXPECT_EQ(recorded.reasoning_requested, std::nullopt);

  const SentinelReplaySettings nested = requestSettingsFromBody(encode(R"({"reasoning":{"effort":"high"}})"), "test-provider", std::nullopt);
  EXPECT_EQ(nested.reasoning_requested, std::optional<std::string>("high"));
  EXPECT_EQ(nested.stream_requested, std::nullopt);

  const SentinelReplaySettings flat = requestSettingsFromBody(encode(R"({"reasoning_effort":"low"})"), "test-provider", std::nullopt);
  EXPECT_EQ(flat.reasoning_requested, std::optional<std::string>("low"));

  const SentinelReplaySettings unbounded = requestSettingsFromBody(encode(R"({"model":"has spaces","stream":"yes"})"), "test-provider", std::nullopt);
  EXPECT_EQ(unbounded.source, "recorded_request_body");
  EXPECT_EQ(unbounded.model_requested, std::nullopt);
  EXPECT_EQ(unbounded.stream_requested, std::nullopt);

  for (const std::string_view body : {"not json", "[]", "\"text\"", "null", "7"}) {
    const SentinelReplaySettings unavailable = requestSettingsFromBody(encode(body), "test-provider", false);
    EXPECT_EQ(unavailable.source, "unavailable");
    EXPECT_EQ(unavailable.model_requested, std::nullopt);
    EXPECT_EQ(unavailable.stream_requested, std::nullopt);
    EXPECT_EQ(unavailable.stream_observed, std::optional<bool>(false));
  }
}

TEST("unavailable reasons and coverage follow the recorded trace and the observed downstream") {
  using Reasons = std::vector<std::string>;
  const SentinelClientFailureObservation complete = clientObservation();
  EXPECT_EQ(replayUnavailableReasons(emptySentinelUpstreamTrace(), complete), Reasons{"upstream_trace_empty"});
  EXPECT_EQ(replayCoverageFor(emptySentinelUpstreamTrace(), complete, Reasons{"upstream_trace_empty"}), "unavailable");
  const SentinelClientFailureObservation rejection = clientObservation([](SentinelClientFailureObservation& value) {
    value.status = 400;
    value.terminal_body_base64 = rt::btoa("bad request");
  });
  EXPECT_EQ(replayUnavailableReasons(emptySentinelUpstreamTrace(), rejection), Reasons{});
  EXPECT_EQ(replayCoverageFor(emptySentinelUpstreamTrace(), rejection, Reasons{}), "full");

  const SentinelClientFailureObservation withoutTerminalBody =
    clientObservation([](SentinelClientFailureObservation& value) { value.terminal_body_base64 = std::nullopt; });
  EXPECT_EQ(replayUnavailableReasons(emptySentinelUpstreamTrace(), withoutTerminalBody), (Reasons{"upstream_trace_empty", "downstream_terminal_body_unavailable"}));

  const SentinelUpstreamAttempt attempt = cerebrasAttempt({"e30="});
  const SentinelUpstreamTrace v1 = v1Trace(attempt);
  const Reasons v1Reasons = replayUnavailableReasons(v1, complete);
  EXPECT_EQ(v1Reasons, (Reasons{"upstream_timing_unavailable", "upstream_response_headers_unavailable"}));
  EXPECT_EQ(replayCoverageFor(v1, complete, v1Reasons), "partial");

  SentinelUpstreamAttempt headedAttempt = attempt;
  headedAttempt.started_at_ms = 1;
  headedAttempt.headers = json::object();
  headedAttempt.chunk_times_ms = std::vector<double>{1};
  const SentinelUpstreamTrace headed = v1Trace(headedAttempt);
  EXPECT_EQ(replayUnavailableReasons(headed, complete), Reasons{});
  EXPECT_EQ(replayCoverageFor(headed, complete, Reasons{}), "full");

  SentinelUpstreamAttempt truncatedHeadersAttempt = headedAttempt;
  truncatedHeadersAttempt.headers_truncated = true;
  const SentinelUpstreamTrace truncatedHeaders = v1Trace(truncatedHeadersAttempt);
  EXPECT_EQ(replayUnavailableReasons(truncatedHeaders, complete), Reasons{"upstream_response_headers_unavailable"});
  EXPECT_EQ(replayCoverageFor(truncatedHeaders, complete, Reasons{"upstream_response_headers_unavailable"}), "partial");

  SentinelUpstreamTrace pending = v1;
  SentinelUpstreamAttempt pendingAttempt = headedAttempt;
  pendingAttempt.terminal = "pending";
  pending.attempts = {pendingAttempt};
  const Reasons pendingReasons = replayUnavailableReasons(pending, complete);
  EXPECT_EQ(pendingReasons, Reasons{"upstream_attempt_pending"});
  EXPECT_EQ(replayCoverageFor(pending, complete, pendingReasons), "partial");

  SentinelUpstreamTrace truncated = v1;
  truncated.attempts_truncated = true;
  truncated.bytes_truncated = true;
  truncated.chunks_truncated = true;
  truncated.attempts = {headed.attempts[0]};
  const Reasons truncatedReasons = replayUnavailableReasons(truncated, complete);
  EXPECT_EQ(truncatedReasons, Reasons{"upstream_trace_truncated"});
  EXPECT_EQ(replayCoverageFor(truncated, complete, truncatedReasons), "partial");
}

ASYNC_TEST("the crypto and framing helpers agree with their declared contracts") {
  // splitChunks keeps the recorded chunk size and never returns zero chunks.
  const auto chunkBytes = static_cast<std::size_t>(SENTINEL_REPLAY_CHUNK_BYTES);
  EXPECT_EQ(sizes(splitChunks(rt::Bytes())), std::vector<std::size_t>{0});
  EXPECT_EQ(sizes(splitChunks(rt::Bytes(4))), std::vector<std::size_t>{4});
  EXPECT_EQ(sizes(splitChunks(rt::Bytes(chunkBytes))), std::vector<std::size_t>{chunkBytes});
  EXPECT_EQ(sizes(splitChunks(rt::Bytes(chunkBytes + 1))), (std::vector<std::size_t>{chunkBytes, 1}));

  // fingerprintParts frames every part with an 8-byte big-endian length, and
  // the case-group frame is upstream-independent while the fingerprint is not.
  const AcceptedSentinelReplayInput input = acceptedInput(R"({"model":"gpt-5"})", "frame-request");
  const SentinelUpstreamTrace trace = emptySentinelUpstreamTrace();
  const std::vector<rt::Bytes> fingerprintFrames = fingerprintParts(input, "signature", "fingerprint", trace);
  const std::vector<rt::Bytes> framesWithoutUpstream = fingerprintParts(input, "signature", "fingerprint");
  const std::vector<rt::Bytes> caseGroupFrames = fingerprintParts(input, "signature", "case-group");
  // Five shared frames (namespace, method, endpoint, headers, body) plus the
  // signature and canonical upstream JSON frames for the fingerprint purpose;
  // the case-group identity stays the four-part request frame plus the body.
  EXPECT_EQ(fingerprintFrames.size(), 14u);
  EXPECT_EQ(caseGroupFrames.size(), 10u);
  EXPECT_EQ(framesWithoutUpstream.size(), 14u);
  EXPECT_EQ(bigUint64(fingerprintFrames[0]), fingerprintFrames[1].size());
  EXPECT_EQ(fingerprintFrames[1], encode("uos-sentinel-replay-v2:fingerprint"));
  EXPECT_EQ(caseGroupFrames[1], encode("uos-sentinel-replay-v1:case-group"));
  // Everything after the purpose-specific namespace frame is shared: the
  // method, endpoint, header, body and signature frames are identical.
  for (std::size_t index = 2; index <= 9; index += 1) EXPECT(fingerprintFrames[index] == caseGroupFrames[index]);
  // The trailing frame is the canonical upstream JSON, whose byte length is
  // what the preceding 8-byte frame declares.
  const rt::Bytes canonicalUpstream = encode(canonicalSentinelUpstreamJson(trace));
  const rt::Bytes& upstreamLengthFrame = fingerprintFrames.at(fingerprintFrames.size() - 2);
  EXPECT_EQ(bigUint64(upstreamLengthFrame), canonicalUpstream.size());
  EXPECT_EQ(fingerprintFrames.back(), canonicalUpstream);
  // Without an explicit trace the frame is present and empty, so a
  // request-only fingerprint can never collide with a trace-bearing one.
  EXPECT_EQ(framesWithoutUpstream.back(), rt::Bytes());
  EXPECT(fingerprintFrames != fingerprintParts(input, "signature", "fingerprint", v1Trace(cerebrasAttempt({}))));

  // The HMAC is deterministic, purpose-separated and hex-encoded.
  const std::vector<rt::Bytes> parts{encode("a"), encode("b")};
  const std::string fingerprintPurpose = "fingerprint";
  const std::string caseGroupPurpose = "case-group";
  auto firstHmac = hmacHex(KEY_BYTES, fingerprintPurpose, parts);
  const std::string fingerprintDigest = co_await std::move(firstHmac);
  EXPECT(rt::Regex("^[0-9a-f]{64}$").test(fingerprintDigest));
  auto secondHmac = hmacHex(KEY_BYTES, fingerprintPurpose, parts);
  const std::string repeated = co_await std::move(secondHmac);
  EXPECT_EQ(fingerprintDigest, repeated);
  auto caseGroupHmac = hmacHex(KEY_BYTES, caseGroupPurpose, parts);
  const std::string caseGroupDigest = co_await std::move(caseGroupHmac);
  EXPECT(fingerprintDigest != caseGroupDigest);
  const rt::Bytes otherKey(32, 0x22);
  auto otherHmac = hmacHex(otherKey, fingerprintPurpose, parts);
  const std::string otherDigest = co_await std::move(otherHmac);
  EXPECT(fingerprintDigest != otherDigest);

  // The ciphertext digest is the SHA-256 hex of the exact bytes.
  const rt::Bytes bytes{1, 2, 3, 4, 5};
  auto digestTask = ciphertextDigest(bytes);
  const std::string digest = co_await std::move(digestTask);
  EXPECT_EQ(digest, utils::encodeHex(rt::crypto::subtle::digest("SHA-256", bytes)));

  // Gzip round-trips and refuses both truncated input and over-large output.
  const rt::Bytes compressible = encode(rt::repeat("compressible", 64));
  auto gzipTask = gzip(compressible);
  const rt::Bytes compressed = co_await std::move(gzipTask);
  auto gunzipTask = gunzip(compressed);
  const rt::Bytes inflated = co_await std::move(gunzipTask);
  EXPECT_EQ(inflated, compressible);
  EXPECT_EQ(compressed.at(0), 0x1f);
  EXPECT_EQ(compressed.at(1), 0x8b);
  const rt::Bytes notGzip = encode("not gzip");
  auto notGzipTask = gunzip(notGzip);
  auto notGzipCheck = expectRejectsMatching(std::move(notGzipTask), "error", "i", "gunzip(not gzip)", __FILE__, __LINE__);
  co_await std::move(notGzipCheck);
  const rt::Bytes zeros(static_cast<std::size_t>(MAX_REPLAY_PLAINTEXT_BYTES) + 1);
  auto tooLargeTask = gzip(zeros);
  const rt::Bytes tooLarge = co_await std::move(tooLargeTask);
  auto tooLargeGunzip = gunzip(tooLarge);
  auto tooLargeCheck = expectRejectsMatching(std::move(tooLargeGunzip), "Sentinel replay plaintext exceeds its size limit", "", "gunzip(tooLarge)", __FILE__, __LINE__);
  co_await std::move(tooLargeCheck);

  // The AES key is derived, not the raw secret, and is usable both ways.
  auto keyTask = importAesKey(KEY_BYTES);
  const rt::crypto::CryptoKey key = co_await std::move(keyTask);
  EXPECT_EQ(key.algorithm().name, "AES-GCM");
  const rt::Bytes iv(12, 7);
  const rt::Bytes sealed =
    rt::crypto::subtle::encrypt(rt::crypto::Algorithm{.name = "AES-GCM", .iv = iv, .additionalData = encryptionAdditionalData(std::string(64, 'a'))}, key, encode("payload"));
  std::string additional = "uos-sentinel-replay-v1";
  additional.push_back('\0');
  additional += std::string(64, 'a');
  EXPECT_EQ(encryptionAdditionalData(std::string(64, 'a')), encode(additional));
  const rt::Bytes opened =
    rt::crypto::subtle::decrypt(rt::crypto::Algorithm{.name = "AES-GCM", .iv = iv, .additionalData = encryptionAdditionalData(std::string(64, 'a'))}, key, sealed);
  EXPECT_EQ(rt::decodeUtf8(opened), "payload");
  const rt::crypto::Algorithm wrongAad{.name = "AES-GCM", .iv = iv, .additionalData = encryptionAdditionalData(std::string(64, 'b'))};
  EXPECT_THROWS(rt::crypto::subtle::decrypt(wrongAad, key, sealed), rt::JsError);

  // Compatibility header validation is an exact allowlist with canonical values.
  EXPECT(isCompatibilityHeaders(json::object()));
  EXPECT(isCompatibilityHeaders(rt::jsonParse(R"({"accept":"application/json","x-stainless-lang":"js"})")));
  EXPECT(!isCompatibilityHeaders(rt::jsonParse(R"({"x-forwarded-for":"198.51.100.7"})")));
  EXPECT(!isCompatibilityHeaders(rt::jsonParse(R"({"accept":""})")));
  EXPECT(!isCompatibilityHeaders(rt::jsonParse(R"({"accept":" application/json"})")));
  EXPECT(!isCompatibilityHeaders(rt::jsonParse(R"({"accept":7})")));
  EXPECT(!isCompatibilityHeaders(json::value("accept: application/json")));
  EXPECT(!isCompatibilityHeaders(json::value(nullptr)));
  // The record check is structural, so an empty JSON array carries no headers
  // and therefore nothing disallowed.
  EXPECT(isCompatibilityHeaders(json::array()));

  // The dedupe record and the downstream projection keep their exact shapes.
  const rt::KvKey manifestKey{"uos_ai", "sentinel_replay", "v1", "manifest", NOW, std::string(64, 'b'), "capture-1"};
  rt::KvArray keyParts;
  for (const rt::KvKeyPart& part : manifestKey) keyParts.push_back(std::visit([](const auto& value) { return rt::KvValue(value); }, part.value));
  rt::KvObject dedupeRow;
  dedupeRow.set("manifest_key", rt::KvValue(keyParts));
  EXPECT(dedupeManifestKey(rt::KvValue(dedupeRow)) == std::optional<rt::KvKey>(manifestKey));
  // Fresh Bun 1.4.0 source oracle (2026-11-17): the unchecked stored key retains a Uint8Array
  // view's offset and two viewed bytes; key conversion must not reject the view or use its backing.
  rt::KvArrayBufferView byteView;
  byteView.kind = rt::KvViewKind::Uint8Array;
  byteView.buffer = {9, 1, 2, 8};
  byteView.byteOffset = 1;
  byteView.byteLength = 2;
  rt::KvArray viewParts = keyParts;
  viewParts[5] = rt::KvValue(byteView);
  rt::KvObject viewRow;
  viewRow.set("manifest_key", rt::KvValue(viewParts));
  rt::KvKey viewKey = manifestKey;
  viewKey[5] = rt::KvKeyPart(rt::KvBytes{1, 2});
  EXPECT(dedupeManifestKey(rt::KvValue(viewRow)) == std::optional<rt::KvKey>(viewKey));

  rt::KvObject shortRow;
  shortRow.set("manifest_key", rt::KvValue::array({rt::KvValue("short")}));
  EXPECT(!dedupeManifestKey(rt::KvValue(shortRow)));
  rt::KvObject textRow;
  textRow.set("manifest_key", rt::KvValue("not-a-key"));
  EXPECT(!dedupeManifestKey(rt::KvValue(textRow)));
  EXPECT(!dedupeManifestKey(rt::KvValue("not-a-record")));
  const SentinelReplayDownstream downstream = downstreamObservation(clientObservation([](SentinelClientFailureObservation& value) { value.terminal_body_truncated = true; }));
  const SentinelReplayDownstream expected{
    .terminal_type = "http.error",
    .failure_kind = "upstream_timeout",
    .error_code = "upstream_error",
    .error_param = std::nullopt,
    .body_base64 = rt::btoa("upstream failed"),
    .body_truncated = true,
  };
  EXPECT(downstream == expected);
}

// ---------------------------------------------------------------------------
// Client-observation inspector
// ---------------------------------------------------------------------------

ASYNC_TEST("an accepted request is captured once, and a body the cap cannot carry is declared omitted") {
  const rt::Request request_ = newRequest("https://ai.ubq.fi/v1/responses?after_ms=1", "POST",
                                       rt::Headers{{"content-type", "application/json"}, {"accept", "application/json"}, {"x-forwarded-for", std::string(CLIENT_ADDRESS)}});
  const std::shared_ptr<SentinelReplayCaptureCandidate> candidate = captureAcceptedSentinelReplayInput(request_, "capture-input-request");
  ASSERT(candidate != nullptr);
  EXPECT_EQ(candidate->endpoint, "/v1/responses?after_ms=1");
  EXPECT_EQ(candidate->method, "POST");
  EXPECT_EQ(candidate->content_type, std::optional<std::string>("application/json"));
  // Only allowlisted compatibility headers are retained, never the client IP.
  EXPECT(rt::deepEqual(json::value(candidate->compatibility_headers), rt::jsonParse(R"({"accept":"application/json"})")));
  EXPECT(candidate->body == nullptr);
  EXPECT_EQ(candidate->body_omitted_reason, std::nullopt);

  // The route hands the accepted bytes over exactly once.
  const std::shared_ptr<rt::Bytes> accepted = sharedBytes(R"({"model":"gpt-5"})");
  EXPECT(request::captureRawBodyOnce(request_, accepted));
  EXPECT(candidate->body == accepted);
  EXPECT(!request::captureRawBodyOnce(request_, accepted));
  const std::optional<AcceptedSentinelReplayInput> materialized = materializeSentinelReplayInput(candidate);
  ASSERT(materialized.has_value());
  EXPECT_EQ(materialized->request_id, "capture-input-request");
  EXPECT_EQ(*materialized->body, *accepted);
  EXPECT(candidate->body == nullptr);

  // A GET has no accepted request body to capture at all.
  EXPECT(captureAcceptedSentinelReplayInput(newRequest("https://ai.ubq.fi/v1/responses", "GET"), "get-request") == nullptr);

  // A body past the fixed replay cap is rejected with an explicit reason, so
  // the omission is reportable instead of looking like an empty history.
  const rt::Request oversized = newRequest("https://ai.ubq.fi/v1/responses", "POST");
  const std::shared_ptr<SentinelReplayCaptureCandidate> oversizedCandidate = captureAcceptedSentinelReplayInput(oversized, "oversized-request");
  ASSERT(oversizedCandidate != nullptr);
  EXPECT(!request::captureRawBodyOnce(oversized, std::make_shared<rt::Bytes>(static_cast<std::size_t>(request::MAX_ACCEPTED_JSON_BODY_BYTES) + 1)));
  EXPECT_EQ(oversizedCandidate->body_omitted_reason, std::optional<std::string>("body_over_limit"));
  EXPECT(!materializeSentinelReplayInput(oversizedCandidate));

  // A candidate whose body never arrived carries the default omission reason.
  const std::shared_ptr<SentinelReplayCaptureCandidate> unavailable =
    captureAcceptedSentinelReplayInput(newRequest("https://ai.ubq.fi/v1/responses", "POST"), "missing-body-request");
  ASSERT(unavailable != nullptr);
  EXPECT(!materializeSentinelReplayInput(unavailable));
  EXPECT_EQ(unavailable->body_omitted_reason, std::optional<std::string>("body_unavailable"));
  EXPECT(!materializeSentinelReplayInput(nullptr));

  // Discarding zeroes the retained bytes and drops the reference.
  const rt::Request discardedRequest = newRequest("https://ai.ubq.fi/v1/responses", "POST");
  const std::shared_ptr<SentinelReplayCaptureCandidate> discarded = captureAcceptedSentinelReplayInput(discardedRequest, "discarded-request");
  ASSERT(discarded != nullptr);
  const std::shared_ptr<rt::Bytes> bytes = sharedBytes("discard-me");
  EXPECT(request::captureRawBodyOnce(discardedRequest, bytes));
  discardSentinelReplayCaptureCandidate(discarded);
  EXPECT(discarded->body == nullptr);
  EXPECT(allZero(*bytes));
  discardSentinelReplayCaptureCandidate(nullptr);

  // The snapshot is an independent copy, and a request-owned recorder is sealed
  // and disposed as part of it.
  const AcceptedSentinelReplayInput zeroTarget = acceptedInput("snapshot-me", "snapshot-request");
  const SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const SentinelUpstreamAttemptHandle handle = recorder.startAttempt("deepseek");
  const rt::Response wrapped = handle.wrap(response(rt::BodyInit(std::string("{}")), 200, rt::Headers{{"content-type", "application/json"}}));
  auto drain = wrapped.arrayBuffer();
  (void)co_await std::move(drain);
  AcceptedSentinelReplayInput withRecorder = zeroTarget;
  withRecorder.upstreamRecorder = recorder;
  const AcceptedSentinelReplayInput snapshot = snapshotSentinelReplayInput(withRecorder);
  EXPECT_EQ(*snapshot.body, *zeroTarget.body);
  EXPECT_EQ(snapshot.upstream ? snapshot.upstream->attempts.size() : 0u, 1u);
  EXPECT(!snapshot.upstreamRecorder.has_value());
  zeroSentinelReplayInput(&zeroTarget);
  EXPECT(allZero(*zeroTarget.body));
  EXPECT_EQ(*snapshot.body, *acceptedInput("snapshot-me", "snapshot-request").body);
  zeroSentinelReplayInput(nullptr);
  zeroSentinelReplayInput(std::optional<AcceptedSentinelReplayInput>());
  disposeSentinelUpstreamRecorder(&snapshot);
  disposeSentinelUpstreamRecorder(nullptr);
}

TEST("buffered responses are classified from their status, body and semantic markers") {
  const auto json_ = [](std::string_view text) { return jsonBytes(rt::jsonParse(text)); };
  const SentinelClientBodyObservation embeddingsQueued = inspectSentinelBufferedResponse(202, "application/json", json_(R"({"object":"embeddings.job","status":"queued"})"));
  EXPECT_EQ(embeddingsQueued.terminal_type, std::optional<std::string>("job.queued"));
  EXPECT_EQ(embeddingsQueued.completed, false);
  for (const std::string_view status : {"running", "in_progress"}) {
    json::object body;
    body["object"] = "embeddings.job";
    body["status"] = std::string(status);
    EXPECT_EQ(inspectSentinelBufferedResponse(202, "application/json", jsonBytes(json::value(body))).terminal_type, std::optional<std::string>("job.queued"));
  }
  const SentinelClientBodyObservation failed =
    inspectSentinelBufferedResponse(200, "application/json", json_(R"({"object":"embeddings.job","status":"failed","error":{"code":"job_error"}})"));
  EXPECT_EQ(failed.terminal_type, std::optional<std::string>("job.failed"));
  EXPECT_EQ(failed.failure_kind, std::optional<std::string>("job_error"));
  const SentinelClientBodyObservation succeeded = inspectSentinelBufferedResponse(200, "application/json", json_(R"({"object":"embeddings.job","status":"succeeded"})"));
  EXPECT_EQ(succeeded.terminal_type, std::optional<std::string>("job.succeeded"));
  EXPECT_EQ(succeeded.completed, true);
  EXPECT(succeeded.terminal_body_base64 == rt::undefined);
  // An embeddings job with an unknown semantic status falls through to the
  // response-level reader, which keeps the HTTP status authoritative.
  EXPECT_EQ(inspectSentinelBufferedResponse(200, "application/json", json_(R"({"object":"embeddings.job","status":"weird"})")).completed, true);

  const SentinelClientBodyObservation responseFailed =
    inspectSentinelBufferedResponse(200, "application/json", json_(R"({"status":"failed","error":{"code":"server_error"}})"));
  EXPECT_EQ(responseFailed.terminal_type, std::optional<std::string>("response.failed"));
  EXPECT(responseFailed.error_code == std::string("server_error"));
  const SentinelClientBodyObservation responseIncomplete =
    inspectSentinelBufferedResponse(200, "application/json", json_(R"({"status":"incomplete","incomplete_details":{"reason":"max_output_tokens"}})"));
  EXPECT_EQ(responseIncomplete.terminal_type, std::optional<std::string>("response.incomplete"));
  EXPECT_EQ(responseIncomplete.failure_kind, std::optional<std::string>("max_output_tokens"));
  // When the incomplete details carry no usable reason, the nested error code
  // is the fallback cause rather than a dropped diagnostic.
  const SentinelClientBodyObservation incompleteWithoutReason =
    inspectSentinelBufferedResponse(200, "application/json", json_(R"({"status":"incomplete","error":{"code":"server_error"}})"));
  EXPECT_EQ(incompleteWithoutReason.terminal_type, std::optional<std::string>("response.incomplete"));
  EXPECT_EQ(incompleteWithoutReason.failure_kind, std::optional<std::string>("server_error"));
  const SentinelClientBodyObservation responseCompleted = inspectSentinelBufferedResponse(200, "application/json", json_(R"({"status":"succeeded"})"));
  EXPECT_EQ(responseCompleted.terminal_type, std::optional<std::string>("response.completed"));
  EXPECT_EQ(responseCompleted.failure_kind, std::nullopt);
  const SentinelClientBodyObservation acceptedResponse = inspectSentinelBufferedResponse(202, "application/json", json_(R"({"status":"queued"})"));
  EXPECT_EQ(acceptedResponse.terminal_type, std::optional<std::string>("http.accepted"));
  EXPECT_EQ(acceptedResponse.failure_kind, std::nullopt);

  // A retained terminal body is bounded and reported as truncated past the cap.
  json::object largeError;
  largeError["code"] = "invalid_request_error";
  largeError["param"] = "model";
  json::object large;
  large["error"] = largeError;
  large["padding"] = std::string(static_cast<std::size_t>(SENTINEL_REPLAY_MAX_DOWNSTREAM_BODY_BYTES), 'x');
  const SentinelClientBodyObservation truncated = inspectSentinelBufferedResponse(400, "application/json", jsonBytes(json::value(large)));
  EXPECT_EQ(truncated.terminal_type, std::optional<std::string>("http.error"));
  EXPECT(truncated.error_param == std::string("model"));
  EXPECT_EQ(truncated.terminal_body_truncated, std::optional<bool>(true));

  // Non-JSON bodies keep the HTTP status as the terminal, with the observed
  // body retained as bounded evidence.
  const SentinelClientBodyObservation plainError = inspectSentinelBufferedResponse(400, "text/plain", encode("bad request"));
  EXPECT_EQ(plainError.terminal_type, std::optional<std::string>("http.error"));
  EXPECT_EQ(plainError.failure_kind, std::nullopt);
  EXPECT_EQ(plainError.stream, false);
  EXPECT_EQ(plainError.framing_valid, true);
  EXPECT(plainError.terminal_body_base64 == rt::btoa("bad request"));
  const SentinelClientBodyObservation plainAccepted = inspectSentinelBufferedResponse(202, "text/plain", rt::Bytes());
  EXPECT_EQ(plainAccepted.terminal_type, std::optional<std::string>("http.accepted"));
  EXPECT(plainAccepted.terminal_body_base64 == rt::undefined);
  const SentinelClientBodyObservation plainCompleted = inspectSentinelBufferedResponse(200, "text/plain", rt::Bytes());
  EXPECT_EQ(plainCompleted.terminal_type, std::optional<std::string>("http.completed"));
  EXPECT_EQ(plainCompleted.completed, true);

  // A body that is not JSON at all is never parsed into a semantic claim.
  const SentinelClientBodyObservation malformed = inspectSentinelBufferedResponse(502, "application/json", encode("{not json"));
  EXPECT_EQ(malformed.terminal_type, std::optional<std::string>("http.error"));
  EXPECT(malformed.error_code == nullptr);
  const SentinelClientBodyObservation malformedOk = inspectSentinelBufferedResponse(200, "application/json", encode("{not json"));
  EXPECT_EQ(malformedOk.terminal_type, std::optional<std::string>("http.completed"));

  // A JSON array is not a response object.
  EXPECT_EQ(inspectSentinelBufferedResponse(200, "application/json", json_("[1,2,3]")).terminal_type, std::optional<std::string>("http.completed"));
}

ASYNC_TEST("buffered response bodies are read once, under a bound, and never throw at the caller") {
  const rt::Response ok = response(rt::BodyInit(std::string(R"({"error":{"code":"upstream_error"}})")), 502, rt::Headers{{"content-type", "application/json"}});
  auto inspectOk = inspectSentinelBufferedResponseBody(ok);
  const std::optional<SentinelClientBodyObservation> observation = co_await std::move(inspectOk);
  ASSERT(observation.has_value());
  EXPECT_EQ(observation->terminal_type, std::optional<std::string>("http.error"));
  EXPECT(observation->error_code == std::string("upstream_error"));
  // The observed clone never consumes the response the caller still needs.
  auto textTask = ok.text();
  const std::string text = co_await std::move(textTask);
  EXPECT(rt::length(text) > 0);

  // A bodyless response is inspected with an empty body.
  auto bodylessTask = inspectSentinelBufferedResponseBody(response(rt::BodyInit(nullptr), 204));
  const std::optional<SentinelClientBodyObservation> bodyless = co_await std::move(bodylessTask);
  ASSERT(bodyless.has_value());
  EXPECT_EQ(bodyless->terminal_type, std::optional<std::string>("http.completed"));
  EXPECT_EQ(bodyless->completed, true);

  // A declared length past the observation bound is refused before reading.
  const std::string overBound = rt::numberToString(SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES + 1);
  auto tooLongTask = inspectSentinelBufferedResponseBody(response(rt::BodyInit(std::string("{}")), 502, rt::Headers{{"content-length", overBound}}));
  const std::optional<SentinelClientBodyObservation> tooLong = co_await std::move(tooLongTask);
  EXPECT(!tooLong.has_value());
  // A declared length that is not a canonical integer is a refusal, not a guess.
  auto invalidTask = inspectSentinelBufferedResponseBody(response(rt::BodyInit(std::string("{}")), 502, rt::Headers{{"content-length", "12abc"}}));
  const std::optional<SentinelClientBodyObservation> invalidLength = co_await std::move(invalidTask);
  EXPECT(!invalidLength.has_value());
  auto negativeTask = inspectSentinelBufferedResponseBody(response(rt::BodyInit(std::string("{}")), 502, rt::Headers{{"content-length", "-1"}}));
  const std::optional<SentinelClientBodyObservation> negativeLength = co_await std::move(negativeTask);
  EXPECT(!negativeLength.has_value());
  // Digits that are not a safe integer are refused as well.
  auto unsafeTask = inspectSentinelBufferedResponseBody(response(rt::BodyInit(std::string("{}")), 502, rt::Headers{{"content-length", rt::repeat("9", 30)}}));
  const std::optional<SentinelClientBodyObservation> unsafeLength = co_await std::move(unsafeTask);
  EXPECT(!unsafeLength.has_value());
  // An undeclared length is enforced while streaming.
  auto streamedTask = inspectSentinelBufferedResponseBody(
    response(rt::BodyInit(rt::Bytes(static_cast<std::size_t>(SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES) + 1)), 502));
  const std::optional<SentinelClientBodyObservation> streamed = co_await std::move(streamedTask);
  EXPECT(!streamed.has_value());
}

TEST("the SSE inspector accepts real frames and reports every framing fault") {
  const SentinelClientBodyObservation hello = inspectSentinelSse(encode("data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\"}}\n\ndata: [DONE]\n\n"));
  EXPECT_EQ(hello.stream, true);
  EXPECT_EQ(hello.completed, true);
  EXPECT_EQ(hello.framing_valid, true);
  EXPECT_EQ(hello.terminal_type, std::optional<std::string>("response.completed"));
  // A later terminal of equal rank never replaces the first one observed.
  EXPECT_EQ(inspectSentinelSse(encode("data: [DONE]\n\ndata: [DONE]\n\n")).terminal_type, std::optional<std::string>("[DONE]"));

  // Unknown fields are a framing fault, and a failed event outranks completion.
  const SentinelClientBodyObservation unknownField =
    inspectSentinelSse(encode("weird: 1\ndata: {\"type\":\"response.completed\"}\n\ndata: {\"type\":\"response.failed\"}\n\n"));
  EXPECT_EQ(unknownField.framing_valid, false);
  EXPECT_EQ(unknownField.terminal_type, std::optional<std::string>("response.failed"));
  EXPECT_EQ(unknownField.completed, false);

  // A data payload that is not a JSON object is invalid framing.
  const SentinelClientBodyObservation malformed = inspectSentinelSse(encode("data: not json\n\n"));
  EXPECT_EQ(malformed.framing_valid, false);
  EXPECT_EQ(malformed.terminal_type, std::nullopt);
  const SentinelClientBodyObservation arrayPayload = inspectSentinelSse(encode("data: [1,2]\n\n"));
  EXPECT_EQ(arrayPayload.framing_valid, false);
  // A comment-only stream carries no terminal at all.
  const SentinelClientBodyObservation commentOnly = inspectSentinelSse(encode(": keepalive\n\n"));
  EXPECT_EQ(commentOnly.terminal_type, std::nullopt);

  // An error event keeps the failing cause, including a nested param.
  const SentinelClientBodyObservation errorEvent =
    inspectSentinelSse(encode("event: error\ndata: {\"error\":{\"code\":\"invalid_request_error\",\"param\":\"model\"}}\n\n"));
  EXPECT_EQ(errorEvent.terminal_type, std::optional<std::string>("error"));
  EXPECT(errorEvent.error_code == std::string("invalid_request_error"));
  EXPECT(errorEvent.error_param == std::string("model"));

  // A read error before any terminal is reported as the transport cause, and an
  // incomplete stream keeps its own missing-terminal reason.
  const SentinelSseInspector readFailure = createSentinelSseInspector();
  readFailure.push(encode(": keepalive\n\n"));
  EXPECT_EQ(readFailure.finish("read_error").failure_kind, std::optional<std::string>("stream_read_error"));
  const SentinelSseInspector eofFailure = createSentinelSseInspector();
  eofFailure.push(encode(": keepalive\n\n"));
  EXPECT_EQ(eofFailure.finish(std::nullopt).failure_kind, std::optional<std::string>("missing_sse_terminal"));
  const SentinelSseInspector terminated = createSentinelSseInspector();
  terminated.push(encode("data: {\"type\":\"response.completed\"}\n\n"));
  EXPECT_EQ(terminated.finish("read_error").terminal_type, std::optional<std::string>("response.completed"));

  // A carried carriage return is normalized across chunk boundaries.
  const SentinelSseInspector carriageReturn = createSentinelSseInspector();
  carriageReturn.push(encode("data: {\"type\":\"response.completed\"}\r"));
  carriageReturn.push(encode("\n\r\n"));
  EXPECT_EQ(carriageReturn.finish(std::nullopt).terminal_type, std::optional<std::string>("response.completed"));
  // A CR held back at a chunk boundary is joined to the next chunk's CRLF, so
  // the event is not split into stray field lines.
  const SentinelSseInspector splitCrlf = createSentinelSseInspector();
  splitCrlf.push(encode("data: {\"type\":\"response.completed\"}\r"));
  splitCrlf.push(encode("\n\ndata: [DONE]\n\n"));
  const SentinelClientBodyObservation rejoined = splitCrlf.finish(std::nullopt);
  EXPECT_EQ(rejoined.framing_valid, true);
  EXPECT_EQ(rejoined.terminal_type, std::optional<std::string>("response.completed"));
  EXPECT_EQ(rejoined.completed, true);
  // A stream that ends on a CR still terminates the pending event, because the
  // deferred carriage return is flushed as the final line break.
  const SentinelClientBodyObservation loneCr = inspectSentinelSse(encode("data: {\"type\":\"response.completed\"}\r\r"));
  EXPECT_EQ(loneCr.terminal_type, std::optional<std::string>("response.completed"));
  EXPECT_EQ(loneCr.framing_valid, true);

  // An event past the fixed frame bound is dropped while the stream continues,
  // and the framing fault is remembered.
  const SentinelSseInspector oversized = createSentinelSseInspector();
  oversized.push(rt::Bytes(static_cast<std::size_t>(MAX_SSE_EVENT_CHARS) + 1, static_cast<std::uint8_t>('x')));
  oversized.push(encode("data: {\"type\":\"response.completed\"}\n\n"));
  const SentinelClientBodyObservation dropped = oversized.finish(std::nullopt);
  EXPECT_EQ(dropped.framing_valid, false);
  EXPECT_EQ(dropped.terminal_type, std::nullopt);

  // A stream that ends mid-event is not valid framing.
  const SentinelClientBodyObservation pending = inspectSentinelSse(encode("data: {\"type\":\"response.completed\"}"));
  EXPECT_EQ(pending.framing_valid, false);
}

TEST("the client failure observation keeps the specific cause and the client-visible literals") {
  const SentinelClientFailureObservation cancelled = resolveSentinelClientFailureObservation(internalObservation([](SentinelFailureObservation& value) {
    value.terminal_type = "cancelled";
    value.failure_kind = "client_aborted";
    value.stream = true;
  }));
  EXPECT_EQ(cancelled.failure_kind, std::nullopt);
  EXPECT_EQ(cancelled.error_code, std::nullopt);
  EXPECT_EQ(cancelled.error_param, std::nullopt);
  EXPECT_EQ(cancelled.terminal_body_base64, std::nullopt);
  EXPECT_EQ(cancelled.terminal_body_truncated, false);
  EXPECT_EQ(cancelled.terminal_type, std::optional<std::string>("cancelled"));
  EXPECT_EQ(cancelled.stream, true);

  // A synthetic post-commit terminal reports `server_error` to the client while
  // the transport cause stays in the diagnostic slot.
  const auto syntheticInternal = [](SentinelFailureObservation& value) {
    value.terminal_type = "eof";
    value.failure_kind = "premature_eof";
    value.synthetic_terminal_type = "response.failed";
    value.stream = true;
  };
  const SentinelClientFailureObservation synthetic = resolveSentinelClientFailureObservation(internalObservation(syntheticInternal));
  EXPECT_EQ(synthetic.terminal_type, std::optional<std::string>("response.failed"));
  EXPECT_EQ(synthetic.failure_kind, std::optional<std::string>("premature_eof"));
  EXPECT_EQ(synthetic.error_code, std::optional<std::string>("server_error"));
  // An observed body always carries its own terminal, even when the internal
  // observation claims a synthetic one.
  const SentinelClientFailureObservation observedBodyWins =
    resolveSentinelClientFailureObservation(internalObservation(syntheticInternal), bodyObservation(true, false, "response.incomplete", std::nullopt, true));
  EXPECT_EQ(observedBodyWins.terminal_type, std::optional<std::string>("response.incomplete"));
  EXPECT_EQ(observedBodyWins.failure_kind, std::optional<std::string>("premature_eof"));
  const SentinelClientFailureObservation genericOnly = resolveSentinelClientFailureObservation(internalObservation([](SentinelFailureObservation& value) {
                                                                                                 value.terminal_type = "eof";
                                                                                                 value.failure_kind = std::nullopt;
                                                                                                 value.synthetic_terminal_type = "response.failed";
                                                                                               }),
                                                                                               bodyObservation(false, false, std::nullopt, "server_error", true));
  EXPECT_EQ(genericOnly.failure_kind, std::optional<std::string>("server_error"));
  EXPECT_EQ(genericOnly.error_code, std::optional<std::string>("server_error"));
  // With no transport cause at all, the synthetic terminal is the only
  // evidence and is reported as the generic server error it was.
  const SentinelClientFailureObservation syntheticOnly = resolveSentinelClientFailureObservation(internalObservation([](SentinelFailureObservation& value) {
    value.status = 200;
    value.completed = false;
    value.terminal_type = "eof";
    value.failure_kind = std::nullopt;
    value.synthetic_terminal_type = "response.failed";
  }));
  EXPECT_EQ(syntheticOnly.terminal_type, std::optional<std::string>("response.failed"));
  EXPECT_EQ(syntheticOnly.failure_kind, std::optional<std::string>("server_error"));
  EXPECT_EQ(syntheticOnly.error_code, std::optional<std::string>("server_error"));
  const SentinelClientFailureObservation genericPair = resolveSentinelClientFailureObservation(internalObservation([](SentinelFailureObservation& value) {
                                                                                                 value.terminal_type = "error";
                                                                                                 value.failure_kind = "error";
                                                                                               }),
                                                                                               bodyObservation(false, false, "error", "server_error", true));
  EXPECT_EQ(genericPair.failure_kind, std::optional<std::string>("server_error"));

  // Without an observed body the fallback terminal is derived from the status.
  const SentinelClientFailureObservation httpError = resolveSentinelClientFailureObservation(internalObservation());
  EXPECT_EQ(httpError.terminal_type, std::optional<std::string>("http.error"));
  EXPECT_EQ(httpError.completed, false);
  const SentinelClientFailureObservation completed = resolveSentinelClientFailureObservation(internalObservation([](SentinelFailureObservation& value) {
    value.status = 200;
    value.completed = true;
    value.terminal_type = std::nullopt;
    value.failure_kind = std::nullopt;
  }));
  EXPECT_EQ(completed.terminal_type, std::optional<std::string>("http.completed"));
  const SentinelClientFailureObservation carried = resolveSentinelClientFailureObservation(internalObservation([](SentinelFailureObservation& value) {
    value.status = 200;
    value.terminal_type = "response.failed";
    value.failure_kind = "server_error";
  }));
  EXPECT_EQ(carried.terminal_type, std::optional<std::string>("response.failed"));
  EXPECT_EQ(carried.framing_valid, false);
  EXPECT_EQ(carried.stream, true);
}

TEST("persistence and incident signal decisions follow the recorded terminal, not the status alone") {
  using Internal = SentinelFailureObservation;
  using Client = SentinelClientFailureObservation;
  EXPECT_EQ(shouldPersistSentinelReplay(internalObservation([](Internal& value) { value.terminal_type = "cancelled"; })), false);
  EXPECT_EQ(shouldPersistSentinelReplay(internalObservation([](Internal& value) {
              value.status = 200;
              value.completed = true;
              value.terminal_type = "response.completed";
              value.failure_kind = std::nullopt;
            })),
            false);
  EXPECT_EQ(shouldPersistSentinelReplay(internalObservation([](Internal& value) {
              value.status = 429;
              value.terminal_type = "http.error";
            })),
            true);
  // A synthetic terminal with no transport cause is still a failure worth
  // persisting, and a null-cause non-2xx terminal is not.
  EXPECT_EQ(shouldPersistSentinelReplay(internalObservation([](Internal& value) {
              value.status = 200;
              value.completed = false;
              value.terminal_type = "eof";
              value.failure_kind = std::nullopt;
              value.synthetic_terminal_type = "response.failed";
            })),
            true);
  EXPECT_EQ(shouldPersistSentinelReplay(internalObservation([](Internal& value) {
                                          value.status = 200;
                                          value.completed = true;
                                          value.terminal_type = "response.completed";
                                          value.failure_kind = std::nullopt;
                                        }),
                                        clientObservation([](Client& value) {
                                          value.stream = true;
                                          value.framing_valid = false;
                                          value.completed = false;
                                          value.terminal_type = "response.incomplete";
                                          value.failure_kind = "invalid_sse_framing";
                                        })),
            true);
  EXPECT_EQ(shouldPersistSentinelReplay(internalObservation([](Internal& value) {
                                          value.status = 200;
                                          value.completed = false;
                                          value.terminal_type = "response.incomplete";
                                          value.failure_kind = "gateway_timeout";
                                        }),
                                        clientObservation([](Client& value) {
                                          value.status = 200;
                                          value.completed = false;
                                          value.terminal_type = "response.incomplete";
                                          value.failure_kind = "gateway_timeout";
                                        })),
            true);
  EXPECT_EQ(shouldPersistSentinelReplay(internalObservation([](Internal& value) {
                                          value.status = 200;
                                          value.completed = false;
                                          value.terminal_type = "response.incomplete";
                                          value.failure_kind = "client_choice";
                                        }),
                                        clientObservation([](Client& value) {
                                          value.status = 200;
                                          value.completed = false;
                                          value.terminal_type = "response.incomplete";
                                          value.failure_kind = "client_choice";
                                        })),
            false);
  // A client observation that itself carries the cancellation terminal cannot
  // make an otherwise successful request persistable.
  EXPECT_EQ(shouldPersistSentinelReplay(internalObservation([](Internal& value) {
                                          value.status = 200;
                                          value.stream = false;
                                          value.terminal_type = std::nullopt;
                                          value.failure_kind = std::nullopt;
                                        }),
                                        clientObservation([](Client& value) {
                                          value.status = 200;
                                          value.terminal_type = "cancelled";
                                          value.failure_kind = std::nullopt;
                                        })),
            false);

  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) { value.terminal_type = "cancelled"; }),
                                         clientObservation([](Client& value) { value.terminal_type = "cancelled"; })),
            false);
  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) {
                                           value.status = 200;
                                           value.completed = false;
                                           value.terminal_type = "response.incomplete";
                                           value.failure_kind = std::nullopt;
                                         }),
                                         clientObservation([](Client& value) {
                                           value.stream = true;
                                           value.framing_valid = false;
                                         })),
            true);
  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) {
                                           value.status = 200;
                                           value.completed = true;
                                           value.terminal_type = "response.completed";
                                           value.failure_kind = std::nullopt;
                                         }),
                                         clientObservation([](Client& value) {
                                           value.status = 200;
                                           value.completed = true;
                                           value.terminal_type = "response.completed";
                                           value.failure_kind = std::nullopt;
                                         })),
            false);
  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) {
                                           value.status = 503;
                                           value.terminal_type = "http.error";
                                         }),
                                         clientObservation([](Client& value) { value.status = 503; })),
            true);
  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) {
                                           value.status = 200;
                                           value.terminal_type = "response.incomplete";
                                           value.failure_kind = std::nullopt;
                                         }),
                                         clientObservation([](Client& value) {
                                           value.status = 200;
                                           value.failure_kind = "upstream_timeout";
                                         })),
            true);
  // An incomplete response whose only cause is a client-side choice is not an
  // incident, while a gateway-side incomplete reason is.
  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) {
                                           value.status = 200;
                                           value.completed = false;
                                           value.terminal_type = "response.incomplete";
                                           value.failure_kind = std::nullopt;
                                         }),
                                         clientObservation([](Client& value) {
                                           value.status = 200;
                                           value.completed = false;
                                           value.terminal_type = "response.incomplete";
                                           value.failure_kind = std::nullopt;
                                         })),
            false);
  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) {
                                           value.status = 200;
                                           value.completed = false;
                                           value.terminal_type = "response.incomplete";
                                           value.failure_kind = "response_incomplete:gateway_timeout";
                                         }),
                                         clientObservation([](Client& value) {
                                           value.status = 200;
                                           value.completed = false;
                                           value.terminal_type = "response.incomplete";
                                           value.failure_kind = std::nullopt;
                                         })),
            true);
  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) {
                                           value.status = 200;
                                           value.terminal_type = "eof";
                                           value.failure_kind = std::nullopt;
                                         }),
                                         clientObservation([](Client& value) {
                                           value.status = 200;
                                           value.failure_kind = std::nullopt;
                                         })),
            true);
  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) {
                                           value.status = 200;
                                           value.completed = true;
                                           value.terminal_type = "response.completed";
                                           value.failure_kind = "server_error";
                                         }),
                                         clientObservation([](Client& value) {
                                           value.status = 200;
                                           value.failure_kind = "server_error";
                                         })),
            false);
  // A synthetic terminal, a specific transport cause or a deadline terminal is
  // always reportable.
  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) {
                                           value.status = 200;
                                           value.completed = false;
                                           value.terminal_type = "response.completed";
                                           value.failure_kind = std::nullopt;
                                           value.synthetic_terminal_type = "response.failed";
                                         }),
                                         clientObservation([](Client& value) {
                                           value.status = 200;
                                           value.completed = true;
                                           value.terminal_type = "response.completed";
                                           value.failure_kind = std::nullopt;
                                         })),
            true);
  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) {
                                           value.status = 200;
                                           value.completed = false;
                                           value.terminal_type = "response.completed";
                                           value.failure_kind = "upstream_timeout";
                                         }),
                                         clientObservation([](Client& value) {
                                           value.status = 200;
                                           value.completed = true;
                                           value.terminal_type = "response.completed";
                                           value.failure_kind = std::nullopt;
                                         })),
            true);
  EXPECT_EQ(shouldSignalSentinelIncident(internalObservation([](Internal& value) {
                                           value.status = 200;
                                           value.terminal_type = "deadline";
                                           value.failure_kind = std::nullopt;
                                         }),
                                         clientObservation([](Client& value) {
                                           value.status = 200;
                                           value.failure_kind = std::nullopt;
                                         })),
            true);
}
