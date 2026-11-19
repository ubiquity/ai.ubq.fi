// Links: tests/sentinel_replay_read_coverage_test.cpp src/sentinel/replay_store.cpp src/sentinel/replay_retention.cpp
//        src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_read.cpp
//        src/sentinel/incident_outbox.cpp src/sentinel/replay_envelope.cpp src/sentinel/replay_observation.cpp
//        src/sentinel/replay_retention_schema.cpp src/sentinel/replay_model.cpp src/sentinel/upstream_capture.cpp
//        src/sentinel/replay_limits.cpp src/request.cpp src/config.cpp src/release.cpp src/kv.cpp src/utils.cpp
//        src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
// Port of tests/sentinel-replay-read-coverage.test.ts, in full (all 13 cases). Storage is `openSentinelTestKv()`, the
// runtime's own `:memory:` KV (rt::MemoryKv). Sealed fixtures are built with the production primitives
// (encodePlaintext, gzip, importAesKey, splitChunks, hmacHex, fingerprintParts), as the TS `sealEnvelope` does; the
// other captures are stored through src/sentinel/replay-store.ts. The TS Proxy over `set` is an rt::ForwardingKv. The
// environment case clears SENTINEL_REPLAY_KEY and the production-runtime variables for its scope (the TS assumes them
// unset).
#include "uos/kv.hpp"
#include "uos/sentinel/incident_outbox.hpp"
#include "uos/sentinel/replay_envelope.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "uos/sentinel/replay_store.hpp"
#include "uos/sentinel/upstream_capture.hpp"
#include "uos/utils.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::sentinel::incident_outbox;

namespace {
  constexpr double NOW = 1'800'000'000'000;
  const std::string GIT_SHA(40, 'c');
  const std::string INCIDENT_UUID = "12345678-1234-4abc-8def-1234567890ab";
  using uos::sentinel::replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX;
  using uos::sentinel::replay_model::SENTINEL_REPLAY_TTL_MS;

  void closeKv(rt::Kv& kv) {
    try {
      kv.close();
    } catch (...) {
      // Idempotent close for both the stub and a real in-memory KV.
    }
  }

  json::value with(const json::value& value, std::string_view key, json::value member) {
    json::object out = value.as_object();
    rt::objectDefine(out, key, std::move(member));
    return out;
  }
}

namespace {
  namespace model = uos::sentinel::replay_model;
  namespace envelope = uos::sentinel::replay_envelope;
  namespace observation = uos::sentinel::replay_observation;
  namespace replay_read = uos::sentinel::replay_read;
  namespace upstream = uos::sentinel::upstream_capture;
  namespace utils = uos::utils;

  const rt::Bytes KEY_BYTES(32, 0x33);
  const std::string SEALED_REQUEST_ID = "sealed-request";
  const std::string SEALED_ENDPOINT = "/v1/responses";
  const std::string SEALED_BODY = R"({"model":"gpt-5","stream":true})";

  rt::Bytes sealedIv() { return rt::Bytes{3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8}; }

  model::SentinelFailureObservation internalObservation() {
    return {
      .status = 502,
      .stream = true,
      .completed = false,
      .terminal_type = "http.error",
      .failure_kind = "upstream_timeout",
      .synthetic_terminal_type = std::nullopt,
      .provider_route = "test-provider",
    };
  }

  model::SentinelClientFailureObservation clientObservation() {
    return {
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
  }

  model::AcceptedSentinelReplayInput acceptedInput(const std::string& body, const std::string& requestId) {
    return {
      .endpoint = SEALED_ENDPOINT,
      .method = "POST",
      .body = std::make_shared<rt::Bytes>(rt::encodeUtf8(body)),
      .content_type = "application/json",
      .compatibility_headers = json::object(),
      .request_id = requestId,
      .git_sha = GIT_SHA,
      .deno_revision = "sealed-revision",
    };
  }

  // `sealEnvelope` options: `aadFingerprint` and `mutateMetadata` are the two tampering seams.
  struct SealOptions {
    std::optional<std::string> body{};
    std::optional<std::string> aadFingerprint{};
    std::function<envelope::ReplayMetadata(envelope::ReplayMetadata)> mutateMetadata{};
  };

  // `Readonly<{ manifest; chunks: string[]; metadata: ReplayMetadata; body }>`
  struct SealedEnvelope {
    model::SentinelReplayManifest manifest;
    std::vector<std::string> chunks;
    envelope::ReplayMetadata metadata;
    rt::Bytes body;
  };

  /**
   * Seal one capture with the production primitives. `aadFingerprint` and
   * `mutateMetadata` are the two tampering seams: the ciphertext is sealed under
   * the given additional data (which is also what the manifest claims as its
   * fingerprint, so the bytes decrypt) while the plaintext carries metadata that
   * no longer reproduces that fingerprint, which is exactly the disagreement the
   * reader must detect.
   */
  rt::Task<SealedEnvelope> sealEnvelopeTask(SealOptions options) {
    const std::string bodyText = options.body.value_or(SEALED_BODY);
    const rt::Bytes body = rt::encodeUtf8(bodyText);
    const model::AcceptedSentinelReplayInput input = acceptedInput(bodyText, SEALED_REQUEST_ID);
    const model::SentinelFailureObservation internal = internalObservation();
    const model::SentinelClientFailureObservation client = clientObservation();
    const upstream::SentinelUpstreamTrace trace = upstream::emptySentinelUpstreamTrace();
    const std::string failureSignature = observation::sentinelFailureSignature(client);
    auto digesting = envelope::ciphertextDigest(body);
    const std::string bodySha256 = co_await std::move(digesting);
    const envelope::ReplayMetadata base{
      .version = 3,
      .captured_at_ms = NOW,
      .endpoint = input.endpoint,
      .method = input.method,
      .content_type = input.content_type,
      .compatibility_headers = input.compatibility_headers,
      .failure_signature = failureSignature,
      .observation = internal,
      .client_observation = client,
      .request_id = input.request_id,
      .git_sha = input.git_sha,
      .deno_revision = input.deno_revision,
      .upstream = trace,
      .settings =
        model::SentinelReplaySettings{
          .source = "recorded_request_body",
          .provider_route = internal.provider_route,
          .model_requested = "gpt-5",
          .reasoning_requested = std::nullopt,
          .stream_requested = true,
          .stream_observed = true,
        },
      .capture_status = "ready",
      .replay_coverage = "full",
      .unavailable = std::vector<model::SentinelReplayUnavailableReason>{},
      .body_sha256 = bodySha256,
      .body_bytes = static_cast<double>(body.size()),
      .downstream = envelope::downstreamObservation(client),
    };
    const envelope::ReplayMetadata metadata = options.mutateMetadata ? options.mutateMetadata(base) : base;
    auto fingerprinting = envelope::hmacHex(KEY_BYTES, "fingerprint", envelope::fingerprintParts(input, failureSignature, "fingerprint", trace));
    const std::string fingerprint = co_await std::move(fingerprinting);
    auto grouping = envelope::hmacHex(KEY_BYTES, "case-group", envelope::fingerprintParts(input, failureSignature, "case-group"));
    const std::string caseGroupDigest = co_await std::move(grouping);
    const rt::Bytes plaintext = envelope::encodePlaintext(metadata, body);
    auto compressing = envelope::gzip(plaintext);
    const rt::Bytes compressed = co_await std::move(compressing);
    const rt::Bytes iv = sealedIv();
    auto importing = envelope::importAesKey(KEY_BYTES);
    const rt::crypto::CryptoKey key = co_await std::move(importing);
    const std::string sealedFingerprint = options.aadFingerprint.value_or(fingerprint);
    const rt::crypto::Algorithm algorithm{.name = "AES-GCM", .iv = iv, .additionalData = envelope::encryptionAdditionalData(sealedFingerprint)};
    const rt::Bytes ciphertext = rt::crypto::subtle::encrypt(algorithm, key, compressed);
    const std::vector<rt::Bytes> chunks = envelope::splitChunks(ciphertext);
    std::vector<std::string> encoded;
    for (const rt::Bytes& chunk : chunks) encoded.push_back(utils::base64UrlEncode(chunk));
    model::SentinelReplayManifest manifest{
      .version = model::ENVELOPE_VERSION,
      .capture_id = "sealed-capture",
      .fingerprint = sealedFingerprint,
      .case_group_digest = caseGroupDigest,
      .captured_at_ms = NOW,
      .expires_at_ms = NOW + SENTINEL_REPLAY_TTL_MS,
      .algorithm = "AES-256-GCM",
      .compression = "gzip",
      .iv = utils::base64UrlEncode(iv),
      .chunk_count = static_cast<double>(chunks.size()),
      .ciphertext_bytes = static_cast<double>(ciphertext.size()),
    };
    co_return SealedEnvelope{.manifest = std::move(manifest), .chunks = std::move(encoded), .metadata = metadata, .body = body};
  }
  // The options aggregate is copied into the coroutine here, outside the caller's co_await expression (GCC 12.2).
  rt::Task<SealedEnvelope> sealEnvelope(const SealOptions& options) { return sealEnvelopeTask(options); }
  rt::Task<SealedEnvelope> sealEnvelope() { return sealEnvelopeTask(SealOptions{}); }

  model::ExportedSentinelReplayCapture exportOf(const SealedEnvelope& sealed) { return {.manifest = sealed.manifest, .chunks = sealed.chunks}; }

  // assert.rejects(fn, /pattern/flags): node:assert matches String(error).
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
#define EXPECT_REJECTS_MATCHING(task, pattern, flags)                                     \
  do {                                                                                    \
    auto rejecting_ = expectRejectsMatching((task), (pattern), (flags), #task, __FILE__, __LINE__); \
    co_await std::move(rejecting_);                                                       \
  } while (false)

  rt::Task<model::SentinelReplayPlaintext> decrypt(const model::ExportedSentinelReplayCapture& exported, const rt::Bytes& keyBytes) {
    return replay_read::decryptExportedSentinelReplay(exported, keyBytes);
  }

  json::value without(const json::value& value, std::string_view key) {
    json::object out = value.as_object();
    rt::objectDelete(out, key);
    return out;
  }
  json::value number(double value) { return rt::makeNumber(value); }
  json::value text(std::string value) { return json::value(json::string(value)); }
}

ASYNC_TEST("a real manifest is accepted and every out-of-contract field is rejected") {
  const SealedEnvelope sealed = co_await sealEnvelope();
  const model::SentinelReplayManifest& manifest = sealed.manifest;
  EXPECT(replay_read::isSentinelReplayManifest(json::value_from(manifest)));

  const json::value base = json::value_from(manifest);
  const std::string twelveByteIv = utils::base64UrlEncode(rt::Bytes(static_cast<std::size_t>(model::AES_GCM_IV_BYTES), 0));
  const double chunkBytes = model::SENTINEL_REPLAY_CHUNK_BYTES;
  std::vector<std::pair<std::string, json::value>> variants;
  variants.emplace_back("not an object", nullptr);
  variants.emplace_back("array", json::array());
  variants.emplace_back("envelope version", with(base, "version", 2));
  variants.emplace_back("empty capture id", with(base, "capture_id", ""));
  variants.emplace_back("over-long capture id", with(base, "capture_id", text(std::string(static_cast<std::size_t>(model::MAX_CAPTURE_ID_CHARS) + 1, 'a'))));
  variants.emplace_back("non-url-safe capture id", with(base, "capture_id", "capture/1"));
  variants.emplace_back("non-hex fingerprint", with(base, "fingerprint", text(std::string(64, 'z'))));
  variants.emplace_back("short case group digest", with(base, "case_group_digest", text(std::string(63, 'a'))));
  variants.emplace_back("float captured_at_ms", with(base, "captured_at_ms", number(NOW + 0.5)));
  variants.emplace_back("negative captured_at_ms", with(base, "captured_at_ms", -1));
  variants.emplace_back("expiry not derived from capture time", with(base, "expires_at_ms", number(NOW + SENTINEL_REPLAY_TTL_MS + 1)));
  variants.emplace_back("unknown algorithm", with(base, "algorithm", "AES-128-GCM"));
  variants.emplace_back("unknown compression", with(base, "compression", "deflate"));
  variants.emplace_back("missing iv", without(base, "iv"));
  variants.emplace_back("short iv", with(base, "iv", text(utils::base64UrlEncode(rt::Bytes(static_cast<std::size_t>(model::AES_GCM_IV_BYTES) - 1, 0)))));
  variants.emplace_back("over-long iv", with(base, "iv", text(utils::base64UrlEncode(rt::Bytes(static_cast<std::size_t>(model::AES_GCM_IV_BYTES) + 1, 0)))));
  variants.emplace_back("non-base64 iv", with(base, "iv", "________!_______"));
  variants.emplace_back("zero chunk count", with(base, "chunk_count", 0));
  variants.emplace_back("fractional chunk count", with(base, "chunk_count", 1.5));
  variants.emplace_back("chunk count above the bound", with(base, "chunk_count", number(model::MAX_REPLAY_CHUNKS + 1)));
  variants.emplace_back("ciphertext below the AEAD tag", with(base, "ciphertext_bytes", 15));
  variants.emplace_back("ciphertext above the bound", with(base, "ciphertext_bytes", number(model::MAX_REPLAY_CIPHERTEXT_BYTES + 1)));
  variants.emplace_back("ciphertext too small for its chunks", with(with(base, "chunk_count", 3), "ciphertext_bytes", number(2 * chunkBytes)));
  variants.emplace_back("ciphertext too large for its chunks", with(with(base, "chunk_count", 2), "ciphertext_bytes", number(2 * chunkBytes + 1)));
  for (const auto& [label, candidate] : variants) {
    const json::value untrusted = rt::jsonParse(rt::jsonStringify(candidate));
    if (replay_read::isSentinelReplayManifest(untrusted)) std::fprintf(stderr, "    %s\n", label.c_str());
    EXPECT(!replay_read::isSentinelReplayManifest(untrusted));
  }
  // A one-chunk manifest spans the whole lower/upper chunk arithmetic.
  EXPECT(replay_read::isSentinelReplayManifest(with(with(base, "chunk_count", 1), "ciphertext_bytes", 16)));
  EXPECT(replay_read::isSentinelReplayManifest(with(base, "iv", text(twelveByteIv))));
  EXPECT(!replay_read::isSentinelReplayManifest(with(base, "iv", text(std::string(20, 'A')))));
  EXPECT(!replay_read::isSentinelReplayManifest(with(base, "iv", text(std::string(24, 'A')))));
}

ASYNC_TEST("an export is accepted exactly when its chunk payloads match the manifest") {
  const SealedEnvelope sealed = co_await sealEnvelope();
  EXPECT_EQ(sealed.manifest.chunk_count, 1.0);
  const json::value manifest = json::value_from(sealed.manifest);
  json::array chunks;
  for (const std::string& chunk : sealed.chunks) chunks.emplace_back(json::string(chunk));
  const auto exported = [](const json::value& manifestValue, const json::value& chunksValue) {
    json::object out;
    out["manifest"] = manifestValue;
    out["chunks"] = chunksValue;
    return json::value(std::move(out));
  };
  EXPECT(replay_read::isExportedSentinelReplayCapture(exported(manifest, chunks)));

  const double chunkBytes = model::SENTINEL_REPLAY_CHUNK_BYTES;
  const auto chunkList = [](json::value chunk) {
    json::array out;
    out.push_back(std::move(chunk));
    return json::value(std::move(out));
  };
  json::object missingManifest;
  missingManifest["chunks"] = chunks;
  std::vector<std::pair<std::string, json::value>> variants;
  variants.emplace_back("not an object", "sealed");
  variants.emplace_back("missing manifest", missingManifest);
  variants.emplace_back("chunks not an array", exported(manifest, json::object()));
  variants.emplace_back("chunk count mismatch", exported(with(manifest, "chunk_count", 2), chunks));
  variants.emplace_back("non-string chunk", exported(manifest, chunkList(7)));
  variants.emplace_back("empty chunk", exported(manifest, chunkList("")));
  variants.emplace_back("over-long chunk",
                        exported(manifest, chunkList(text("a" + std::string(static_cast<std::size_t>(rt::Math::ceil(chunkBytes / 3) * 4), 'A')))));
  variants.emplace_back("non-url-safe chunk", exported(manifest, chunkList("has spaces")));
  variants.emplace_back("chunk does not match the manifest size", exported(manifest, chunkList(text(utils::base64UrlEncode(rt::Bytes(4, 0))))));
  variants.emplace_back("fewer chunks than the manifest declares",
                        exported(with(with(manifest, "chunk_count", 2), "ciphertext_bytes", number(chunkBytes + 16)),
                                 chunkList(text(utils::base64UrlEncode(rt::Bytes(static_cast<std::size_t>(chunkBytes) + 16, 0))))));
  for (const auto& [label, candidate] : variants) {
    const json::value untrusted = rt::jsonParse(rt::jsonStringify(candidate));
    if (replay_read::isExportedSentinelReplayCapture(untrusted)) std::fprintf(stderr, "    %s\n", label.c_str());
    EXPECT(!replay_read::isExportedSentinelReplayCapture(untrusted));
  }
  // Unpadded base64 whose decoded length is wrong for the manifest.
  EXPECT(!replay_read::isExportedSentinelReplayCapture(
    exported(with(with(manifest, "chunk_count", 1), "ciphertext_bytes", 16), chunkList(text(utils::base64UrlEncode(rt::Bytes(17, 0)))))));
}

ASYNC_TEST("decryption returns the exact accepted request and rejects key, export and integrity faults") {
  const SealedEnvelope sealed = co_await sealEnvelope();
  const rt::Bytes shortKey(31, 0);
  EXPECT_REJECTS_MATCHING(decrypt(exportOf(sealed), shortKey), "Sentinel replay key must be 32 bytes", "");
  const model::ExportedSentinelReplayCapture notBase64{.manifest = sealed.manifest, .chunks = {"not base64!"}};
  EXPECT_REJECTS_MATCHING(decrypt(notBase64, KEY_BYTES), "Sentinel replay export is invalid", "");

  auto decrypting = decrypt(exportOf(sealed), KEY_BYTES);
  const model::SentinelReplayPlaintext plaintext = co_await std::move(decrypting);
  EXPECT_EQ(plaintext.version, 3.0);
  EXPECT_EQ(plaintext.endpoint, SEALED_ENDPOINT);
  EXPECT_EQ(plaintext.method, std::string("POST"));
  EXPECT_EQ(plaintext.request_id, SEALED_REQUEST_ID);
  EXPECT_EQ(plaintext.captured_at_ms, NOW);
  EXPECT_EQ(plaintext.capture_status, std::optional<std::string>("ready"));
  EXPECT_EQ(plaintext.replay_coverage, std::optional<std::string>("full"));
  EXPECT(plaintext.unavailable == std::optional<std::vector<std::string>>(std::vector<std::string>{}));
  EXPECT(plaintext.body && *plaintext.body == sealed.body);
  EXPECT(plaintext.upstream.attempts.empty());
  EXPECT_EQ(plaintext.settings ? plaintext.settings->model_requested : std::nullopt, std::optional<std::string>("gpt-5"));
  EXPECT_EQ(plaintext.downstream ? plaintext.downstream->body_base64 : std::nullopt, std::optional<std::string>(rt::btoa("upstream failed")));
  EXPECT_EQ(observation::sentinelFailureSignature(plaintext.client_observation), plaintext.failure_signature);

  // A different key cannot decrypt the same envelope.
  const rt::Bytes otherKey(32, 0x44);
  EXPECT_REJECTS_MATCHING(decrypt(exportOf(sealed), otherKey), "OperationError|Decryption failed|Unsupported|error", "i");

  // Sealed under a fingerprint the plaintext does not reproduce: the bytes
  // decrypt, and the reader must still refuse them.
  const SealOptions foreignOptions{.aadFingerprint = std::string(64, 'a')};
  const SealedEnvelope foreignAad = co_await sealEnvelope(foreignOptions);
  EXPECT_REJECTS_MATCHING(decrypt(exportOf(foreignAad), KEY_BYTES), "Sentinel replay manifest integrity check failed", "");
  // A case-group digest that no longer matches its metadata.
  const SealedEnvelope mismatchedGroup = co_await sealEnvelope();
  model::ExportedSentinelReplayCapture regrouped = exportOf(mismatchedGroup);
  regrouped.manifest.case_group_digest = std::string(64, 'b');
  EXPECT_REJECTS_MATCHING(decrypt(regrouped, KEY_BYTES), "Sentinel replay manifest integrity check failed", "");
  // A capture time that disagrees with the sealed metadata.
  SealOptions shiftedOptions;
  shiftedOptions.mutateMetadata = [](envelope::ReplayMetadata metadata) {
    metadata.captured_at_ms = NOW + 5;
    return metadata;
  };
  const SealedEnvelope shiftedTime = co_await sealEnvelope(shiftedOptions);
  EXPECT_REJECTS_MATCHING(decrypt(exportOf(shiftedTime), KEY_BYTES), "Sentinel replay manifest integrity check failed", "");
  // A failure signature that no longer matches the client observation.
  SealOptions orphanedOptions;
  orphanedOptions.mutateMetadata = [](envelope::ReplayMetadata metadata) {
    metadata.failure_signature = "{}";
    return metadata;
  };
  const SealedEnvelope orphanedSignature = co_await sealEnvelope(orphanedOptions);
  EXPECT_REJECTS_MATCHING(decrypt(exportOf(orphanedSignature), KEY_BYTES), "Sentinel replay manifest integrity check failed", "");
  // The request frames are integrity-protected too: a rewritten endpoint or
  // request body cannot be smuggled in under a valid manifest fingerprint.
  SealOptions endpointOptions;
  endpointOptions.mutateMetadata = [](envelope::ReplayMetadata metadata) {
    metadata.endpoint = "/v1/chat/completions";
    return metadata;
  };
  const SealedEnvelope rewrittenEndpoint = co_await sealEnvelope(endpointOptions);
  EXPECT_REJECTS_MATCHING(decrypt(exportOf(rewrittenEndpoint), KEY_BYTES), "Sentinel replay manifest integrity check failed", "");
  SealOptions methodOptions;
  methodOptions.body = SEALED_BODY;
  methodOptions.mutateMetadata = [](envelope::ReplayMetadata metadata) {
    metadata.method = "PUT";
    return metadata;
  };
  const SealedEnvelope swappedBody = co_await sealEnvelope(methodOptions);
  EXPECT_REJECTS_MATCHING(decrypt(exportOf(swappedBody), KEY_BYTES), "Sentinel replay manifest integrity check failed", "");
}

ASYNC_TEST("incident failure events reject every out-of-contract shape and completion") {
  const std::shared_ptr<rt::Kv> kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  const std::string incidentId = "provider-" + INCIDENT_UUID;
  json::object capturingObject;
  capturingObject["version"] = 1;
  capturingObject["incident_id"] = incidentId;
  capturingObject["state"] = "capturing";
  capturingObject["observed_at_ms"] = rt::makeNumber(NOW);
  capturingObject["created_at_ms"] = rt::makeNumber(NOW);
  capturingObject["ready_at_ms"] = nullptr;
  capturingObject["capture_status"] = "pending";
  capturingObject["capture_fingerprint"] = nullptr;
  capturingObject["manifest_key"] = nullptr;
  const json::value capturing(capturingObject);
  EXPECT(isSentinelIncidentFailureEvent(capturing));
  const json::value ready = with(with(capturing, "state", "ready"), "capture_status", "unavailable");
  std::vector<std::pair<std::string, json::value>> variants;
  variants.emplace_back("not a record", json::value("event"));
  variants.emplace_back("wrong version", with(capturing, "version", 2));
  variants.emplace_back("invalid incident id", with(capturing, "incident_id", "provider-nope"));
  variants.emplace_back("zero observation time", with(capturing, "observed_at_ms", 0));
  variants.emplace_back("fractional observation time", with(capturing, "observed_at_ms", 1.5));
  variants.emplace_back("creation before observation", with(capturing, "created_at_ms", rt::makeNumber(NOW - 1)));
  variants.emplace_back("non-null ready time while capturing", with(capturing, "ready_at_ms", rt::makeNumber(NOW)));
  variants.emplace_back("non-hex capture fingerprint", with(capturing, "capture_fingerprint", "not-hex"));
  variants.emplace_back("short manifest key", with(capturing, "manifest_key", json::array{"short"}));
  variants.emplace_back("unknown state", with(capturing, "state", "finished"));
  variants.emplace_back("ready without ready time", ready);
  variants.emplace_back("ready before creation", with(ready, "ready_at_ms", rt::makeNumber(NOW - 1)));
  variants.emplace_back("unavailable with a fingerprint",
                        with(with(ready, "ready_at_ms", rt::makeNumber(NOW)), "capture_fingerprint", json::value(json::string(std::string(64, 'a')))));
  variants.emplace_back("stored without evidence", with(with(ready, "ready_at_ms", rt::makeNumber(NOW)), "capture_status", "stored"));
  for (const auto& [label, candidate] : variants) {
    const json::value untrusted = rt::jsonParse(rt::jsonStringify(candidate));
    if (isSentinelIncidentFailureEvent(untrusted)) std::fprintf(stderr, "    %s\n", label.c_str());
    EXPECT(!isSentinelIncidentFailureEvent(untrusted));
  }

  // A completion whose evidence cannot describe a capture is refused
  // rather than written as a ready event.
  SentinelIncidentDependencies dependencies;
  dependencies.randomUuid = [] { return INCIDENT_UUID; };
  const rt::KvEntry<SentinelIncidentFailureEvent> event = co_await createSentinelIncidentFailureEvent(*kv, NOW, dependencies);
  const IncidentCaptureCompletion badCompletion = IncidentCaptureCompletionCaptured{.status = "stored", .fingerprint = "not-hex", .manifestKey = rt::KvKey{"short"}};
  EXPECT_THROWS_MESSAGE(readySentinelIncidentFailureEvent(event, NOW + 1, badCompletion), rt::JsError, "Sentinel incident capture completion is invalid");

  // The same bound applies to the evidence a capture may claim: a capture
  // id that cannot be a bounded opaque reference never reaches storage.
  const SentinelIncidentIndexObservationInput input{.endpoint = "/v1/responses", .method = "POST", .gitSha = GIT_SHA, .observedAtMs = NOW,
                                                    .observation = {.status = 502, .stream = false, .completed = false, .framing_valid = true,
                                                                    .terminal_type = std::string("http.error")}};
  const rt::KvEntry<SentinelIncidentIndexRow> row = co_await recordSentinelIncidentIndexObservation(*kv, input, dependencies);
  const SentinelIncidentIndexEvidenceBinding binding{
    .observedAtMs = NOW,
    .captureId = "capture with spaces",
    .gitSha = GIT_SHA,
    .referenceFingerprint = std::string(64, '9'),
    .manifestKey = rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, std::string(64, '9'), "capture-bad-id"},
    .manifestVersionstamp = std::nullopt,
    .capturedAtMs = NOW,
    .digest = std::string(64, '9'),
    .expiresAtMs = NOW + SENTINEL_REPLAY_TTL_MS,
  };
  EXPECT_REJECTS_MESSAGE(bindSentinelIncidentIndexEvidence(*kv, row.value.fingerprint, binding), rt::JsError, "Sentinel incident index evidence is invalid");
  const rt::KvEntryMaybe<> reference = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, std::string(64, '9')});
  EXPECT(reference.value.isNull());
  closeKv(*kv);
}

// ---- the cases that store captures through src/sentinel/replay-store.ts --------------------------------------------

namespace {
  namespace store = uos::sentinel::replay_store;
  using observation::PersistDependencies;
  using observation::SentinelReplayPersistResult;
  using observation::SentinelReplayPersistResultDisabled;
  using observation::SentinelReplayPersistResultDuplicate;
  using observation::SentinelReplayPersistResultStored;

  std::shared_ptr<rt::Kv> openSentinelTestKv() { return std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); }); }

  PersistDependencies sealedDependencies(std::shared_ptr<rt::Kv> kv, double nowMs, std::optional<std::string> captureId) {
    PersistDependencies dependencies{.kv = std::move(kv), .keyBytes = KEY_BYTES, .now = [nowMs] { return nowMs; }};
    if (captureId) {
      dependencies.randomUuid = [id = *captureId] { return id; };
      dependencies.randomBytes = [](double) { return sealedIv(); };
    }
    return dependencies;
  }

  rt::Task<SentinelReplayPersistResult> persistTask(model::AcceptedSentinelReplayInput input, model::SentinelFailureObservation internal,
                                                    PersistDependencies dependencies, std::optional<model::SentinelClientFailureObservation> client) {
    auto persisting = client ? store::persistEncryptedSentinelReplay(input, internal, dependencies, *client)
                             : store::persistEncryptedSentinelReplay(input, internal, dependencies);
    SentinelReplayPersistResult result = co_await std::move(persisting);
    co_return result;
  }

  /** Store a real capture through the store and return what it wrote. */
  rt::Task<SentinelReplayPersistResultStored> storeCaptureTask(std::shared_ptr<rt::Kv> kv, std::string body, std::string requestId, std::string captureId,
                                                              double nowMs) {
    auto persisting = persistTask(acceptedInput(body, requestId), internalObservation(), sealedDependencies(kv, nowMs, captureId), clientObservation());
    const SentinelReplayPersistResult result = co_await std::move(persisting);
    // The assertion narrows the returned result to the stored variant.
    const auto* stored = std::get_if<SentinelReplayPersistResultStored>(&result);
    if (!stored) throw rt::JsError("expected a stored capture");
    co_return *stored;
  }
  rt::Task<SentinelReplayPersistResultStored> storeCapture(std::shared_ptr<rt::Kv> kv, const std::string& body, const std::string& requestId,
                                                          const std::string& captureId, double nowMs = NOW) {
    return storeCaptureTask(std::move(kv), body, requestId, captureId, nowMs);
  }

  rt::KvValue keyValue(const rt::KvKey& key) {
    rt::KvArray parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(std::visit([](const auto& value) { return rt::KvValue(value); }, part.value));
    return rt::KvValue(std::move(parts));
  }
  rt::KvValue manifestValue(const model::SentinelReplayManifest& manifest) { return rt::KvValue(json::value_from(manifest)); }
  rt::KvValue referenceValue(double version, const rt::KvKey& manifestKey) {
    rt::KvObject reference;
    reference.set("version", rt::KvValue(version));
    reference.set("manifest_key", keyValue(manifestKey));
    return rt::KvValue(std::move(reference));
  }

  rt::Task<std::vector<rt::KvKey>> keysUnder(rt::Kv& kv, rt::KvKey prefix) {
    std::vector<rt::KvKey> keys;
    rt::KvListSelector selector;
    selector.prefix = std::move(prefix);
    const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
    while (const std::optional<rt::KvEntry<>> entry = co_await iterator->next()) keys.push_back(entry->key);
    co_return keys;
  }

  rt::Task<replay_read::EncryptedSentinelReplayPage> listRange(rt::Kv& kv, double afterMs, double beforeMs, std::optional<std::string> cursor = std::nullopt,
                                                               std::optional<double> limit = std::nullopt) {
    replay_read::ListEncryptedSentinelReplaysOptions options{.afterMs = afterMs, .beforeMs = beforeMs};
    options.cursor = std::move(cursor);
    options.limit = limit;
    auto listing = replay_read::listEncryptedSentinelReplays(kv, options);
    replay_read::EncryptedSentinelReplayPage page = co_await std::move(listing);
    co_return page;
  }

  rt::Task<replay_read::EncryptedSentinelReplayPage> listIncident(rt::Kv& kv, std::string incidentId, std::optional<std::string> cursor = std::nullopt,
                                                                  std::optional<double> limit = std::nullopt) {
    replay_read::ListEncryptedSentinelIncidentReplaysOptions options;
    options.incidentId = std::move(incidentId);
    options.cursor = std::move(cursor);
    options.limit = limit;
    auto listing = replay_read::listEncryptedSentinelIncidentReplays(kv, options);
    replay_read::EncryptedSentinelReplayPage page = co_await std::move(listing);
    co_return page;
  }

  rt::Task<replay_read::EncryptedSentinelRequestReplays> listByRequestId(rt::Kv& kv, std::string requestId) {
    auto listing = replay_read::listEncryptedSentinelReplaysByRequestId(kv, std::move(requestId));
    replay_read::EncryptedSentinelRequestReplays result = co_await std::move(listing);
    co_return result;
  }

  replay_read::EncryptedSentinelReplayPage emptyPage() { return {.captures = {}, .cursor = ""}; }

  SentinelIncidentClassificationInput classificationOf(const model::AcceptedSentinelReplayInput& input, const model::SentinelClientFailureObservation& client) {
    return {
      .endpoint = input.endpoint,
      .method = input.method,
      .observation = {.status = client.status, .stream = client.stream, .completed = client.completed, .framing_valid = client.framing_valid,
                      .terminal_type = client.terminal_type},
    };
  }
}

ASYNC_TEST("chunk reads are paged, size-checked and fail closed on missing storage") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const SentinelReplayPersistResultStored stored = co_await storeCapture(kv, SEALED_BODY, "chunk-request", "capture-chunks");
  auto reading = replay_read::getChunks(*kv, stored.manifest);
  const std::vector<rt::Bytes> chunks = co_await std::move(reading);
  ASSERT_EQ(chunks.size(), std::size_t{1});
  EXPECT_EQ(static_cast<double>(chunks[0].size()), stored.manifest.ciphertext_bytes);
  const rt::KvEntryMaybe<> storedChunk = co_await kv->get(rt::KvKey{model::SENTINEL_REPLAY_CHUNK_PREFIX, "capture-chunks", 0});
  EXPECT(storedChunk.value.isBytes() && storedChunk.value.asBytes() == chunks[0]);

  const rt::KvValue shortChunk(rt::KvBytes(4, 0));
  co_await kv->set(rt::KvKey{model::SENTINEL_REPLAY_CHUNK_PREFIX, "capture-chunks", 0}, shortChunk);
  EXPECT_REJECTS_MATCHING(replay_read::getChunks(*kv, stored.manifest), "Sentinel replay chunk size does not match its manifest", "");

  co_await kv->delete_(rt::KvKey{model::SENTINEL_REPLAY_CHUNK_PREFIX, "capture-chunks", 0});
  EXPECT_REJECTS_MATCHING(replay_read::getChunks(*kv, stored.manifest), "Sentinel replay chunk is missing", "");

  // Eleven chunks exercise the ten-key batch boundary and the exact tail size.
  const double tailBytes = 1;
  model::SentinelReplayManifest pageManifest = stored.manifest;
  pageManifest.capture_id = "capture-paged";
  pageManifest.chunk_count = 11;
  pageManifest.ciphertext_bytes = 10 * model::SENTINEL_REPLAY_CHUNK_BYTES + tailBytes;
  for (double index = 0; index < 10; index += 1) {
    const rt::KvValue chunk(rt::KvBytes(static_cast<std::size_t>(model::SENTINEL_REPLAY_CHUNK_BYTES), static_cast<std::uint8_t>(index)));
    co_await kv->set(rt::KvKey{model::SENTINEL_REPLAY_CHUNK_PREFIX, "capture-paged", index}, chunk);
  }
  const rt::KvValue tail(rt::KvBytes(static_cast<std::size_t>(tailBytes), 0xab));
  co_await kv->set(rt::KvKey{model::SENTINEL_REPLAY_CHUNK_PREFIX, "capture-paged", 10}, tail);
  auto paging = replay_read::getChunks(*kv, pageManifest);
  const std::vector<rt::Bytes> paged = co_await std::move(paging);
  ASSERT_EQ(paged.size(), std::size_t{11});
  EXPECT_EQ(static_cast<double>(paged[9].size()), model::SENTINEL_REPLAY_CHUNK_BYTES);
  EXPECT_EQ(static_cast<double>(paged[10].size()), tailBytes);
  EXPECT(!paged[10].empty() && paged[10][0] == 0xab);
  // A missing tail chunk in the second batch is still reported.
  co_await kv->delete_(rt::KvKey{model::SENTINEL_REPLAY_CHUNK_PREFIX, "capture-paged", 10});
  EXPECT_REJECTS_MATCHING(replay_read::getChunks(*kv, pageManifest), "Sentinel replay chunk is missing", "");

  EXPECT_EQ(replay_read::manifestMatchesKey(rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, stored.manifest.fingerprint, "capture-chunks"}, stored.manifest), true);
  EXPECT_EQ(replay_read::manifestMatchesKey(rt::KvKey{"wrong", "prefix"}, stored.manifest), false);
  EXPECT_EQ(replay_read::manifestMatchesKey(rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW}, stored.manifest), false);
  EXPECT_EQ(replay_read::manifestMatchesKey(rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW + 1, stored.manifest.fingerprint, "capture-chunks"}, stored.manifest),
            false);
  EXPECT_EQ(replay_read::manifestMatchesKey(rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, std::string(64, '0'), "capture-chunks"}, stored.manifest), false);
  closeKv(*kv);
}

ASYNC_TEST("the time-range export validates its window, pages by cursor and refuses corrupt manifests") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const double windowAfter = 0;
  const double windowBefore = NOW + 10'000;
  const std::vector<double> badStarts{-1, 1.5};
  for (const double afterMs : badStarts) EXPECT_REJECTS_MATCHING(listRange(*kv, afterMs, windowBefore), "Sentinel replay export start is invalid", "");
  const std::vector<double> badEnds{-1, 1.5, rt::Number::MAX_SAFE_INTEGER};
  for (const double beforeMs : badEnds) EXPECT_REJECTS_MATCHING(listRange(*kv, windowAfter, beforeMs), "Sentinel replay export end is invalid", "");
  EXPECT_REJECTS_MATCHING(listRange(*kv, windowAfter, windowBefore, std::nullopt, 2), "Sentinel replay export limit must be one", "");
  const std::vector<std::string> badCursors{"", std::string(2'049, 'a'), "has spaces"};
  for (const std::string& cursor : badCursors) EXPECT_REJECTS_MATCHING(listRange(*kv, windowAfter, windowBefore, cursor), "Sentinel replay export cursor is invalid", "");
  const replay_read::EncryptedSentinelReplayPage empty = co_await listRange(*kv, windowAfter, windowBefore);
  EXPECT(empty == emptyPage());

  const SentinelReplayPersistResultStored first = co_await storeCapture(kv, SEALED_BODY, "range-request-1", "capture-range-1", NOW);
  const SentinelReplayPersistResultStored second =
    co_await storeCapture(kv, R"({"model":"gpt-5","stream":false})", "range-request-2", "capture-range-2", NOW + 1'000);

  const replay_read::EncryptedSentinelReplayPage pageOne = co_await listRange(*kv, windowAfter, windowBefore);
  ASSERT_EQ(pageOne.captures.size(), std::size_t{1});
  EXPECT_EQ(pageOne.captures[0].manifest.captured_at_ms, NOW);
  EXPECT(pageOne.cursor != "");
  const replay_read::EncryptedSentinelReplayPage pageTwo = co_await listRange(*kv, windowAfter, windowBefore, pageOne.cursor);
  ASSERT_EQ(pageTwo.captures.size(), std::size_t{1});
  EXPECT_EQ(pageTwo.captures[0].manifest.captured_at_ms, NOW + 1'000);
  const replay_read::EncryptedSentinelReplayPage pageThree = co_await listRange(*kv, windowAfter, windowBefore, pageTwo.cursor);
  EXPECT(pageThree.captures.empty());
  // A window that ends before the first capture exhausts the range instead
  // of handing out a cursor that would rescan it forever.
  const replay_read::EncryptedSentinelReplayPage before = co_await listRange(*kv, 0, NOW - 1);
  EXPECT(before == emptyPage());

  // The exported chunk text is the stored ciphertext, and it decrypts.
  const model::ExportedSentinelReplayCapture& exported = pageOne.captures[0];
  EXPECT_EQ(static_cast<double>(exported.chunks.size()), first.manifest.chunk_count);
  const model::SentinelReplayPlaintext plaintext = co_await decrypt(exported, KEY_BYTES);
  EXPECT_EQ(plaintext.request_id, std::string("range-request-1"));

  // A manifest-shaped value under a manifest key is still rejected when it
  // disagrees with the key it was stored under.
  const rt::KvKey misplacedKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW + 7, second.manifest.fingerprint, "capture-range-2"};
  co_await kv->set(misplacedKey, manifestValue(second.manifest));
  EXPECT_REJECTS_MATCHING(listRange(*kv, NOW + 5, NOW + 10'000), "Sentinel replay manifest is invalid", "");
  co_await kv->delete_(misplacedKey);
  const rt::KvKey secondKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW + 1'000, second.manifest.fingerprint, "capture-range-2"};
  rt::KvObject versionTwo;
  versionTwo.set("version", rt::KvValue(2));
  const rt::KvValue versionTwoValue(std::move(versionTwo));
  co_await kv->set(secondKey, versionTwoValue);
  EXPECT_REJECTS_MATCHING(listRange(*kv, NOW + 500, NOW + 10'000), "Sentinel replay manifest is invalid", "");
  co_await kv->set(secondKey, manifestValue(second.manifest));
  co_await kv->delete_(rt::KvKey{model::SENTINEL_REPLAY_CHUNK_PREFIX, "capture-range-2", 0});
  EXPECT_REJECTS_MATCHING(listRange(*kv, NOW + 500, NOW + 10'000), "Sentinel replay chunk is missing", "");
  closeKv(*kv);
}

ASYNC_TEST("the incident export validates ids and references and refuses unavailable evidence") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const std::string incidentId = "provider-" + INCIDENT_UUID;
  EXPECT_REJECTS_MATCHING(listIncident(*kv, "provider-nope"), "Sentinel incident ID is invalid", "");
  EXPECT_REJECTS_MATCHING(listIncident(*kv, incidentId, std::nullopt, 2), "Sentinel replay export limit must be one", "");
  const std::vector<std::string> badCursors{"", std::string(2'049, 'a'), "has spaces"};
  for (const std::string& cursor : badCursors) EXPECT_REJECTS_MATCHING(listIncident(*kv, incidentId, cursor), "Sentinel replay export cursor is invalid", "");
  const replay_read::EncryptedSentinelReplayPage empty = co_await listIncident(*kv, incidentId);
  EXPECT(empty == emptyPage());

  const SentinelReplayPersistResultStored stored = co_await storeCapture(kv, SEALED_BODY, "incident-request", "capture-incident");
  ASSERT(stored.manifest_key.has_value());
  const rt::KvKey manifestKey = *stored.manifest_key;

  // A reference record that cannot describe a capture is never skipped.
  rt::KvObject shortReference;
  shortReference.set("version", rt::KvValue(1));
  shortReference.set("manifest_key", keyValue(rt::KvKey{NOW}));
  const std::vector<rt::KvValue> invalidValues{referenceValue(2, manifestKey), rt::KvValue(std::move(shortReference))};
  for (const rt::KvValue& value : invalidValues) {
    const rt::KvKey key{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, std::string(64, '2')};
    co_await kv->set(key, value);
    EXPECT_REJECTS_MATCHING(listIncident(*kv, incidentId), "Sentinel incident replay reference is invalid", "");
    co_await kv->delete_(key);
  }
  const rt::KvKey overLongKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, std::string(64, '3'), "extra"};
  co_await kv->set(overLongKey, referenceValue(1, manifestKey));
  EXPECT_REJECTS_MATCHING(listIncident(*kv, incidentId), "Sentinel incident replay reference is invalid", "");
  co_await kv->delete_(overLongKey);
  const rt::KvKey nonHexKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, "not-hex"};
  co_await kv->set(nonHexKey, referenceValue(1, manifestKey));
  EXPECT_REJECTS_MATCHING(listIncident(*kv, incidentId), "Sentinel incident replay reference is invalid", "");
  co_await kv->delete_(nonHexKey);

  auto linking = linkSentinelReplayToIncident(*kv, incidentId, stored.manifest.fingerprint, manifestKey);
  co_await std::move(linking);
  const replay_read::EncryptedSentinelReplayPage listed = co_await listIncident(*kv, incidentId);
  ASSERT_EQ(listed.captures.size(), std::size_t{1});
  EXPECT_EQ(listed.captures[0].manifest.fingerprint, stored.manifest.fingerprint);
  const model::SentinelReplayPlaintext plaintext = co_await decrypt(listed.captures[0], KEY_BYTES);
  EXPECT_EQ(plaintext.request_id, std::string("incident-request"));

  // A reference that points at no manifest names a payload the store no
  // longer holds (evicted, expired, or written without a TTL by an older
  // revision). It is skipped and removed, and the same page still returns
  // the next live capture instead of a fault. The all-zero fingerprint
  // sorts before every real HMAC fingerprint, so the skip path runs first.
  const std::string danglingFingerprint(64, '0');
  EXPECT(stored.manifest.fingerprint != danglingFingerprint);
  const rt::KvKey danglingRefKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, danglingFingerprint};
  co_await kv->set(danglingRefKey, referenceValue(1, rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, danglingFingerprint, "capture-dangling"}));
  const replay_read::EncryptedSentinelReplayPage firstPage = co_await listIncident(*kv, incidentId);
  ASSERT_EQ(firstPage.captures.size(), std::size_t{1});
  EXPECT_EQ(firstPage.captures[0].manifest.fingerprint, stored.manifest.fingerprint);
  EXPECT(firstPage.cursor != "");
  const rt::KvEntryMaybe<> removed = co_await kv->get(danglingRefKey);
  EXPECT(removed.value.isNull());
  const replay_read::EncryptedSentinelReplayPage nextPage = co_await listIncident(*kv, incidentId, firstPage.cursor);
  EXPECT(nextPage == emptyPage());

  // A reference whose manifest exists but is not its own is still a fault.
  co_await kv->delete_(rt::KvKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, stored.manifest.fingerprint});
  co_await kv->set(danglingRefKey, referenceValue(1, rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, NOW, stored.manifest.fingerprint, "capture-incident"}));
  EXPECT_REJECTS_MATCHING(listIncident(*kv, incidentId), "Sentinel incident replay manifest is unavailable", "");
  co_await kv->delete_(danglingRefKey);
  closeKv(*kv);
}

ASYNC_TEST("a request id resolves to its capture, to nothing, or to an explicit expiry") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const replay_read::EncryptedSentinelRequestReplays unknown = co_await listByRequestId(*kv, "never-captured-request");
  EXPECT(unknown.captures.empty());
  EXPECT_EQ(unknown.status.status, std::string("unknown"));
  EXPECT_EQ(unknown.status.reason, std::optional<std::string>("no_capture_record"));

  // A request that was refused before capture keeps its own status and has
  // no manifest to export.
  const model::SentinelReplayCaptureStatusRow disabledRow{
    .version = 1,
    .request_id = "disabled-request",
    .status = "disabled",
    .reason = "key_missing",
    .captured_at_ms = NOW,
    .manifest_key = std::nullopt,
    .fingerprint = std::nullopt,
    .expires_at_ms = std::nullopt,
  };
  co_await kv->set(rt::KvKey{"uos_ai", "sentinel_replay", "v1", "request", "disabled-request"}, model::toKvValue(disabledRow));
  const replay_read::EncryptedSentinelRequestReplays disabled = co_await listByRequestId(*kv, "disabled-request");
  EXPECT(disabled.captures.empty());
  EXPECT_EQ(disabled.status.status, std::string("disabled"));

  const SentinelReplayPersistResultStored stored = co_await storeCapture(kv, SEALED_BODY, "by-request-id", "capture-by-request");
  const replay_read::EncryptedSentinelRequestReplays resolved = co_await listByRequestId(*kv, "by-request-id");
  ASSERT_EQ(resolved.captures.size(), std::size_t{1});
  EXPECT_EQ(resolved.status.status, std::string("incomplete"));
  const model::SentinelReplayPlaintext plaintext = co_await decrypt(resolved.captures[0], KEY_BYTES);
  EXPECT_EQ(plaintext.request_id, std::string("by-request-id"));

  // A status row whose manifest is gone is reported as expired, with the
  // reason that says why, never as an empty successful history.
  ASSERT(stored.manifest_key.has_value());
  co_await kv->delete_(*stored.manifest_key);
  const replay_read::EncryptedSentinelRequestReplays missingManifest = co_await listByRequestId(*kv, "by-request-id");
  EXPECT(missingManifest.captures.empty());
  EXPECT_EQ(missingManifest.status.status, std::string("expired"));
  EXPECT_EQ(missingManifest.status.reason, std::optional<std::string>("manifest_unavailable"));

  co_await kv->set(*stored.manifest_key, manifestValue(stored.manifest));
  const model::SentinelReplayCaptureStatusRow mismatchedRow{
    .version = 1,
    .request_id = "mismatched-request",
    .status = "ready",
    .reason = std::nullopt,
    .captured_at_ms = NOW,
    .manifest_key = stored.manifest_key,
    .fingerprint = std::string(64, '5'),
    .expires_at_ms = NOW + SENTINEL_REPLAY_TTL_MS,
  };
  co_await kv->set(rt::KvKey{"uos_ai", "sentinel_replay", "v1", "request", "mismatched-request"}, model::toKvValue(mismatchedRow));
  const replay_read::EncryptedSentinelRequestReplays mismatched = co_await listByRequestId(*kv, "mismatched-request");
  EXPECT(mismatched.captures.empty());
  EXPECT_EQ(mismatched.status.status, std::string("expired"));
  EXPECT_EQ(mismatched.status.fingerprint, std::optional<std::string>(std::string(64, '5')));
  EXPECT_REJECTS_MATCHING(listByRequestId(*kv, "bad request id"), "Sentinel replay request ID is invalid", "");
  auto reading = replay_read::readSentinelReplayCaptureStatus(*kv, "by-request-id");
  const model::SentinelReplayCaptureStatusRow status = co_await std::move(reading);
  EXPECT_EQ(status.status, std::string("incomplete"));
  closeKv(*kv);
}

ASYNC_TEST("a duplicate pointing at unavailable evidence fails closed instead of claiming a capture") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const model::SentinelClientFailureObservation client = clientObservation();
  const model::AcceptedSentinelReplayInput input = acceptedInput(SEALED_BODY, "ghost-request");
  const SentinelReplayPersistResultStored first = co_await storeCapture(kv, SEALED_BODY, "ghost-request", "capture-ghost");
  ASSERT(first.manifest_key.has_value());
  auto fingerprinting = sentinelIncidentFingerprint(classificationOf(input, client));
  const std::string indexFingerprint = co_await std::move(fingerprinting);
  const SentinelIncidentIndexObservationInput seed{
    .endpoint = input.endpoint,
    .method = input.method,
    .gitSha = GIT_SHA,
    .observedAtMs = NOW,
    .observation = classificationOf(input, client).observation,
  };
  SentinelIncidentDependencies seedDependencies;
  seedDependencies.randomUuid = [] { return INCIDENT_UUID; };
  auto seeding = recordSentinelIncidentIndexObservation(*kv, seed, seedDependencies);
  const rt::KvEntry<SentinelIncidentIndexRow> seededIndex = co_await std::move(seeding);
  // The group fingerprint the duplicate path resolves is the seeded row's.
  EXPECT(seededIndex.key.back() == rt::KvKeyPart(indexFingerprint));
  const rt::KvKey dedupeKey{model::SENTINEL_REPLAY_DEDUPE_PREFIX, first.manifest.fingerprint};

  // 1. The dedupe winner's manifest is gone.
  rt::KvObject legacyDedupe;
  legacyDedupe.set("manifest_key", keyValue(*first.manifest_key));
  const rt::KvValue legacyDedupeValue(std::move(legacyDedupe));
  co_await kv->set(dedupeKey, legacyDedupeValue);
  co_await kv->delete_(*first.manifest_key);
  model::AcceptedSentinelReplayInput ghostOne = input;
  ghostOne.request_id = "ghost-request-1";
  EXPECT_REJECTS_MATCHING(persistTask(ghostOne, internalObservation(), sealedDependencies(kv, NOW, std::nullopt), client),
                          "Sentinel incident replay manifest is unavailable", "");

  // 2. The winning manifest record exists but is not a manifest.
  rt::KvObject versionTwo;
  versionTwo.set("version", rt::KvValue(2));
  const rt::KvValue versionTwoValue(std::move(versionTwo));
  co_await kv->set(*first.manifest_key, versionTwoValue);
  model::AcceptedSentinelReplayInput ghostTwo = input;
  ghostTwo.request_id = "ghost-request-2";
  EXPECT_REJECTS_MATCHING(persistTask(ghostTwo, internalObservation(), sealedDependencies(kv, NOW, std::nullopt), client),
                          "Sentinel incident replay manifest is unavailable", "");

  // 3. The record is a real manifest, just not the one the dedupe row claims.
  const SentinelReplayPersistResultStored other = co_await storeCapture(kv, R"({"model":"other"})", "other-request", "capture-other");
  ASSERT(other.manifest_key.has_value());
  co_await kv->delete_(*first.manifest_key);
  rt::KvObject otherDedupe;
  otherDedupe.set("manifest_key", keyValue(*other.manifest_key));
  const rt::KvValue otherDedupeValue(std::move(otherDedupe));
  co_await kv->set(dedupeKey, otherDedupeValue);
  model::AcceptedSentinelReplayInput ghostThree = input;
  ghostThree.request_id = "ghost-request-3";
  EXPECT_REJECTS_MATCHING(persistTask(ghostThree, internalObservation(), sealedDependencies(kv, NOW, std::nullopt), client),
                          "Sentinel incident replay manifest is unavailable", "");
  const rt::KvEntryMaybe<> reference = co_await kv->get(rt::KvKey{SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, "provider-" + INCIDENT_UUID, first.manifest.fingerprint});
  EXPECT(reference.value.isNull());
  closeKv(*kv);
}

ASYNC_TEST("a capture that cannot be named, and an outbox event that lost its race, are both rejected") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  // The per-request status row is part of the capture contract: a request
  // id that cannot be published aborts the store instead of writing a
  // capture nothing can find.
  EXPECT_REJECTS_MATCHING(persistTask(acceptedInput(SEALED_BODY, "bad request id"), internalObservation(), sealedDependencies(kv, NOW, "capture-unnamed"),
                                      std::nullopt),
                          "Sentinel replay capture status is invalid", "");
  const rt::KvEntryMaybe<> chunk = co_await kv->get(rt::KvKey{model::SENTINEL_REPLAY_CHUNK_PREFIX, "capture-unnamed", 0});
  EXPECT(chunk.value.isNull());
  const std::vector<rt::KvKey> manifestKeys = co_await keysUnder(*kv, SENTINEL_REPLAY_MANIFEST_PREFIX);
  EXPECT(manifestKeys.empty());

  // An incident event whose recorded versionstamp was replaced can never be
  // completed, and the store reports the conflict instead of pretending.
  SentinelIncidentDependencies eventDependencies;
  eventDependencies.randomUuid = [] { return std::string("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"); };
  auto creating = createSentinelIncidentFailureEvent(*kv, NOW, eventDependencies);
  const rt::KvEntry<SentinelIncidentFailureEvent> event = co_await std::move(creating);
  co_await kv->set(event.key, toKvValue(event.value));
  const model::AcceptedSentinelReplayInput input = acceptedInput(SEALED_BODY, "stale-event-request");
  auto storing = persistTask(input, internalObservation(), sealedDependencies(kv, NOW, "capture-stale-event"), clientObservation());
  const SentinelReplayPersistResult stored = co_await std::move(storing);
  EXPECT(std::holds_alternative<SentinelReplayPersistResultStored>(stored));
  model::AcceptedSentinelReplayInput second = input;
  second.request_id = "stale-event-request-2";
  PersistDependencies staleDependencies = sealedDependencies(kv, NOW, std::nullopt);
  staleDependencies.incidentEvent = event;
  EXPECT_REJECTS_MATCHING(persistTask(second, internalObservation(), staleDependencies, clientObservation()), "Sentinel incident capture completion conflicted", "");
  const IncidentCaptureCompletion unavailable = IncidentCaptureCompletionUnavailable{};
  auto completing = completeSentinelIncidentFailureEvent(*kv, event, NOW + 1, unavailable);
  const bool completed = co_await std::move(completing);
  EXPECT_EQ(completed, false);
  closeKv(*kv);
}

ASYNC_TEST("an observation the index cannot classify still stores its capture without index evidence") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  model::SentinelClientFailureObservation client = clientObservation();
  client.status = 700;
  model::SentinelFailureObservation internal = internalObservation();
  internal.status = 700;
  auto storing = persistTask(acceptedInput(SEALED_BODY, "unclassifiable-request"), internal, sealedDependencies(kv, NOW, "capture-unclassified"), client);
  const SentinelReplayPersistResult result = co_await std::move(storing);
  const auto* stored = std::get_if<SentinelReplayPersistResultStored>(&result);
  ASSERT(stored != nullptr);
  EXPECT_EQ(stored->manifest.capture_id, std::string("capture-unclassified"));
  const std::vector<rt::KvKey> indexKeys = co_await keysUnder(*kv, rt::KvKey{"uos_ai", "sentinel_incident", "v1", "index"});
  EXPECT(indexKeys.empty());
  const replay_read::EncryptedSentinelRequestReplays resolved = co_await listByRequestId(*kv, "unclassifiable-request");
  EXPECT_EQ(resolved.captures.size(), std::size_t{1});
  closeKv(*kv);
}

ASYNC_TEST("two identical requests racing the dedupe check settle to one capture and one duplicate") {
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  const model::SentinelClientFailureObservation client = clientObservation();
  const model::AcceptedSentinelReplayInput input = acceptedInput(SEALED_BODY, "race-request-a");
  model::AcceptedSentinelReplayInput raced = input;
  raced.request_id = "race-request-b";
  rt::Promise<SentinelReplayPersistResult> left(persistTask(input, internalObservation(), sealedDependencies(kv, NOW, "capture-race-a"), client));
  rt::Promise<SentinelReplayPersistResult> right(persistTask(raced, internalObservation(), sealedDependencies(kv, NOW, "capture-race-b"), client));
  auto both = rt::all(left, right);
  const std::tuple<SentinelReplayPersistResult, SentinelReplayPersistResult> results = co_await both.wait();
  const SentinelReplayPersistResult& leftResult = std::get<0>(results);
  const SentinelReplayPersistResult& rightResult = std::get<1>(results);
  const bool leftDuplicate = std::holds_alternative<SentinelReplayPersistResultDuplicate>(leftResult);
  const SentinelReplayPersistResult& loserResult = leftDuplicate ? leftResult : rightResult;
  const SentinelReplayPersistResult& winnerResult = leftDuplicate ? rightResult : leftResult;
  // These assertions both document the outcome and narrow the two results.
  const auto* loser = std::get_if<SentinelReplayPersistResultDuplicate>(&loserResult);
  const auto* winner = std::get_if<SentinelReplayPersistResultStored>(&winnerResult);
  ASSERT(loser != nullptr);
  ASSERT(winner != nullptr);
  EXPECT_EQ(loser->fingerprint, winner->manifest.fingerprint);
  EXPECT(loser->manifest_key == winner->manifest_key);

  // Exactly one manifest and one chunk set survived the race.
  const std::vector<rt::KvKey> manifestKeys = co_await keysUnder(*kv, SENTINEL_REPLAY_MANIFEST_PREFIX);
  ASSERT_EQ(manifestKeys.size(), std::size_t{1});
  EXPECT(winner->manifest_key && manifestKeys[0] == *winner->manifest_key);
  const std::vector<rt::KvKey> chunkKeys = co_await keysUnder(*kv, model::SENTINEL_REPLAY_CHUNK_PREFIX);
  EXPECT_EQ(static_cast<double>(chunkKeys.size()), winner->manifest.chunk_count);
  for (const rt::KvKey& key : chunkKeys) EXPECT(key.size() >= 2 && key[key.size() - 2] == rt::KvKeyPart(winner->manifest.capture_id));

  // Both requests resolve to that one capture.
  const std::vector<std::string> requestIds{"race-request-a", "race-request-b"};
  for (const std::string& requestId : requestIds) {
    const replay_read::EncryptedSentinelRequestReplays resolved = co_await listByRequestId(*kv, requestId);
    EXPECT_EQ(resolved.captures.size(), std::size_t{1});
    EXPECT_EQ(resolved.status.fingerprint, std::optional<std::string>(winner->manifest.fingerprint));
  }
  closeKv(*kv);
}

namespace {
  rt::Task<rt::KvCommitResult> rejectedStatusSet() {
    throw rt::JsError("status storage unavailable");
    co_return rt::KvCommitResult{};
  }
  // `new Proxy(kv, { get: set rejects })`
  class UnwritableKv final : public rt::ForwardingKv {
  public:
    using rt::ForwardingKv::ForwardingKv;
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions = {}) override { return rejectedStatusSet(); }
  };
}

ASYNC_TEST("a status row write failure never replaces the reported persist outcome") {
  // The TS assumes no replay key and no production runtime in the environment.
  const rt::env::ScopedOverride environment(
    {{"SENTINEL_REPLAY_KEY", std::nullopt}, {"DENO_DEPLOY_ORG_SLUG", std::nullopt}, {"DENO_DEPLOY_APP_SLUG", std::nullopt}, {"DENO_TIMELINE", std::nullopt}});
  const std::shared_ptr<rt::Kv> kv = openSentinelTestKv();
  uos::kv::setKvForTest(std::make_shared<UnwritableKv>(*kv));
  const model::AcceptedSentinelReplayInput input = acceptedInput(SEALED_BODY, "unwritable-status-request");
  const model::SentinelFailureObservation internal = internalObservation();
  const std::optional<model::SentinelClientFailureObservation> client = clientObservation();
  auto persisting = store::persistSentinelReplayFromEnvironment(input, internal, client);
  const SentinelReplayPersistResult result = co_await std::move(persisting);
  const auto* disabled = std::get_if<SentinelReplayPersistResultDisabled>(&result);
  EXPECT(disabled != nullptr && disabled->reason == "key_missing");
  EXPECT(std::all_of(input.body->begin(), input.body->end(), [](std::uint8_t byte) { return byte == 0; }));
  uos::kv::setKvForTest(nullptr);
  closeKv(*kv);
}
