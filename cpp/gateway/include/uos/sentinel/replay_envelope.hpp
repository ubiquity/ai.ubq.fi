#pragma once
// Port of src/sentinel/replay-envelope.ts.
// Sentinel replay envelope, key derivation and validation, split out of src/sentinel_replay_capture.ts.
//
// gzip/gunzip stand in for `CompressionStream("gzip")` / `DecompressionStream("gzip")` (provided by uos::rt::gzip):
// they use the system zlib, so the compressed bytes are valid gzip but not byte-identical to
// Bun's (whose deflate is not zlib's); decompression accepts every gzip stream Bun's does and
// fails with Bun's TypeError texts.
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uos::sentinel::replay_envelope {
  // std::nullopt is null.
  std::optional<rt::Bytes> decodeSentinelReplayKey(const std::string& raw);

  // `Omit<SentinelReplayPlaintext, "body">`: the plaintext metadata the envelope JSON-encodes.
  struct ReplayMetadata {
    double version;  // 2 | 3
    double captured_at_ms;
    std::string endpoint;
    std::string method;
    std::optional<std::string> content_type;
    replay_model::SentinelCompatibilityHeaders compatibility_headers;
    std::string failure_signature;
    replay_model::SentinelFailureObservation observation;
    replay_model::SentinelClientFailureObservation client_observation;
    std::string request_id;
    std::string git_sha;
    std::string deno_revision;
    upstream_capture::SentinelUpstreamTrace upstream;
    std::optional<replay_model::SentinelReplaySettings> settings{};
    std::optional<std::string> capture_status{};
    std::optional<std::string> replay_coverage{};
    std::optional<std::vector<replay_model::SentinelReplayUnavailableReason>> unavailable{};
    std::optional<std::string> body_sha256{};
    std::optional<double> body_bytes{};
    std::optional<replay_model::SentinelReplayDownstream> downstream{};
    friend bool operator==(const ReplayMetadata&, const ReplayMetadata&) = default;
  };

  // JSON.stringify(metadata)'s object: members in declaration order, undefined members absent.
  void tag_invoke(const json::value_from_tag&, json::value& out, const ReplayMetadata& value);

  // ---- the module's `export { ... }` list, in its order ----------------------------------------
  [[nodiscard]] rt::Task<std::string> ciphertextDigest(rt::Bytes bytes);

  // Throws rt::JsError with the envelope's messages, rt::SyntaxError for metadata that is not JSON.
  replay_model::SentinelReplayPlaintext decodePlaintext(std::span<const std::uint8_t> bytes);

  // `value: unknown`; std::nullopt is null.
  std::optional<rt::KvKey> dedupeManifestKey(const rt::KvValue& value);

  /** The durable dedupe row now carries the winning manifest's real identity and expiry. */
  rt::KvValue dedupeRecordFor(const rt::KvKey& manifestKey, const replay_model::SentinelReplayManifest& manifest);

  // `Readonly<{ capturedAtMs: number; expiresAtMs: number }>`
  struct DedupeWinnerIdentity {
    double capturedAtMs;
    double expiresAtMs;
    friend bool operator==(const DedupeWinnerIdentity&, const DedupeWinnerIdentity&) = default;
  };

  /** Winner identity from a dedupe row; legacy rows (no expiry) return null and require a manifest read. */
  std::optional<DedupeWinnerIdentity> dedupeWinnerIdentity(const rt::KvValue& value);

  replay_model::SentinelReplayDownstream downstreamObservation(const replay_model::SentinelClientFailureObservation& observation);

  // `metadata` as typed metadata, or as the JSON value JSON.stringify receives.
  rt::Bytes encodePlaintext(const ReplayMetadata& metadata, std::span<const std::uint8_t> body);
  rt::Bytes encodePlaintext(const json::value& metadata, std::span<const std::uint8_t> body);

  rt::Bytes encryptionAdditionalData(const std::string& fingerprint);

  // `purpose`: "fingerprint" | "case-group"; the overload without `upstream` is undefined.
  std::vector<rt::Bytes> fingerprintParts(const replay_model::AcceptedSentinelReplayInput& input, const std::string& failureSignature, const std::string& purpose);
  std::vector<rt::Bytes> fingerprintParts(const replay_model::AcceptedSentinelReplayInput& input, const std::string& failureSignature, const std::string& purpose,
                                          const upstream_capture::SentinelUpstreamTrace& upstream);

  [[nodiscard]] rt::Task<rt::Bytes> gunzip(rt::Bytes bytes);

  [[nodiscard]] rt::Task<rt::Bytes> gzip(rt::Bytes bytes);

  // `purpose`: "fingerprint" | "case-group".
  [[nodiscard]] rt::Task<std::string> hmacHex(rt::Bytes keyBytes, std::string purpose, std::vector<rt::Bytes> parts);

  [[nodiscard]] rt::Task<rt::crypto::CryptoKey> importAesKey(rt::Bytes keyBytes);

  // `value: unknown`
  bool isCompatibilityHeaders(const json::value& value);

  // "full" | "partial" | "unavailable"
  std::string replayCoverageFor(const upstream_capture::SentinelUpstreamTrace& upstream, const replay_model::SentinelClientFailureObservation& clientObservation,
                                const std::vector<replay_model::SentinelReplayUnavailableReason>& unavailable);

  /** Every reason this capture is not a complete, fully replayable record. */
  std::vector<replay_model::SentinelReplayUnavailableReason> replayUnavailableReasons(const upstream_capture::SentinelUpstreamTrace& upstream,
                                                                                    const replay_model::SentinelClientFailureObservation& clientObservation);

  /**
   * Request-declared settings read back from the exact recorded request bytes.
   * This never invents a serving decision: `provider_route` and
   * `stream_observed` are observations, and the model/reasoning/stream values are
   * labelled as the recorded request's own declared settings.
   */
  replay_model::SentinelReplaySettings requestSettingsFromBody(std::span<const std::uint8_t> body, const std::string& providerRoute,
                                                               const std::optional<bool>& streamObserved);

  std::vector<rt::Bytes> splitChunks(std::span<const std::uint8_t> bytes);
}
