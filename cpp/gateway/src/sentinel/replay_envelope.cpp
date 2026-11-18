#include "uos/sentinel/replay_envelope.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/sentinel/replay_observation.hpp"
#include "uos/utils.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace uos::sentinel::replay_envelope {
  namespace {
    using replay_model::SentinelClientFailureObservation;
    using rt::record::Writer;

    void zero(rt::Bytes& bytes) {
      if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
    }

    rt::Bytes deriveKeyBytes(std::span<const std::uint8_t> keyBytes, const std::string& purpose) {
      const rt::crypto::CryptoKey material = rt::crypto::subtle::importKey("raw", keyBytes, "HKDF", false, {"deriveBits"});
      return rt::crypto::subtle::deriveBits(
        rt::crypto::Algorithm{
          .name = "HKDF",
          .hash = "SHA-256",
          .salt = replay_model::KEY_DERIVATION_SALT,
          .info = replay_model::TEXT_ENCODER.encode(purpose),
        },
        material, 256);
    }

    rt::crypto::CryptoKey importHmacKey(std::span<const std::uint8_t> keyBytes, const std::string& purpose) {
      rt::Bytes derived = deriveKeyBytes(keyBytes, purpose);
      const rt::crypto::CryptoKey key = rt::crypto::subtle::importKey("raw", derived, rt::crypto::Algorithm{.name = "HMAC", .hash = "SHA-256"}, false, {"sign"});
      zero(derived);
      return key;
    }

    // isStringRecord(value): utils.isRecord (arrays included) with only string values.
    bool isStringRecord(const json::value& value) {
      if (!utils::isRecord(value)) return false;
      if (const json::array* items = value.if_array()) return std::all_of(items->begin(), items->end(), [](const json::value& item) { return item.is_string(); });
      const json::object& object = value.get_object();
      return std::all_of(object.begin(), object.end(), [](const json::key_value_pair& member) { return member.value().is_string(); });
    }

    // `value === null || typeof value === "string"` on a member (absent is undefined).
    bool nullOrString(const json::value* value) { return value && (value->is_null() || value->is_string()); }
    bool nullOrBoolean(const json::value* value) { return value && (value->is_null() || value->is_bool()); }
    bool isBoolean(const json::value* value) { return value && value->is_bool(); }
    bool isString(const json::value* value) { return value && value->is_string(); }
    // `typeof value === "number" && Number.isSafeInteger(value)`
    bool isSafeInteger(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      return number && rt::Number::isSafeInteger(*number);
    }
    // `value === null || boundedFailureKind(value) !== null`
    bool nullOrBoundedKind(const json::value* value) { return value && (value->is_null() || replay_observation::boundedFailureKind(value).has_value()); }
    bool nullOrBoundedParam(const json::value* value) { return value && (value->is_null() || replay_observation::boundedErrorParam(value).has_value()); }

    bool isFailureObservation(const json::value* value) {
      return utils::isRecord(value) && isSafeInteger(rt::get(value, "status")) && nullOrBoolean(rt::get(value, "stream")) &&
             isBoolean(rt::get(value, "completed")) && nullOrString(rt::get(value, "terminal_type")) && nullOrString(rt::get(value, "failure_kind")) &&
             nullOrString(rt::get(value, "synthetic_terminal_type")) && isString(rt::get(value, "provider_route"));
    }

    bool isClientFailureObservation(const json::value* value, bool requireRich) {
      return utils::isRecord(value) && isSafeInteger(rt::get(value, "status")) && isBoolean(rt::get(value, "stream")) && isBoolean(rt::get(value, "completed")) &&
             nullOrString(rt::get(value, "terminal_type")) && nullOrString(rt::get(value, "failure_kind")) && isBoolean(rt::get(value, "framing_valid")) &&
             isString(rt::get(value, "provider_route")) &&
             (!requireRich || (nullOrBoundedKind(rt::get(value, "error_code")) && nullOrBoundedParam(rt::get(value, "error_param")) &&
                               nullOrString(rt::get(value, "terminal_body_base64")) && isBoolean(rt::get(value, "terminal_body_truncated"))));
    }

    bool isOneOf(const json::value* value, std::initializer_list<std::string_view> allowed) {
      const std::optional<std::string_view> text = rt::getString(value);
      return text && std::find(allowed.begin(), allowed.end(), *text) != allowed.end();
    }

    bool isReplaySettings(const json::value* value) {
      return utils::isRecord(value) && isOneOf(rt::get(value, "source"), {"recorded_request_body", "unavailable"}) &&
             isString(rt::get(value, "provider_route")) && nullOrBoundedKind(rt::get(value, "model_requested")) &&
             nullOrBoundedKind(rt::get(value, "reasoning_requested")) && nullOrBoolean(rt::get(value, "stream_requested")) &&
             nullOrBoolean(rt::get(value, "stream_observed"));
    }

    bool isReplayDownstream(const json::value* value) {
      return utils::isRecord(value) && nullOrString(rt::get(value, "terminal_type")) && nullOrString(rt::get(value, "failure_kind")) &&
             nullOrBoundedKind(rt::get(value, "error_code")) && nullOrBoundedParam(rt::get(value, "error_param")) &&
             nullOrString(rt::get(value, "body_base64")) && isBoolean(rt::get(value, "body_truncated"));
    }

    bool isUnavailableReasons(const json::value* value) {
      const json::array* items = rt::getArray(value);
      if (!items || items->size() > replay_model::SENTINEL_REPLAY_UNAVAILABLE_REASONS.size()) return false;
      return std::all_of(items->begin(), items->end(), [](const json::value& item) {
        const std::optional<std::string_view> text = rt::getString(item);
        return text && std::find(replay_model::SENTINEL_REPLAY_UNAVAILABLE_REASONS.begin(), replay_model::SENTINEL_REPLAY_UNAVAILABLE_REASONS.end(), *text) !=
                         replay_model::SENTINEL_REPLAY_UNAVAILABLE_REASONS.end();
      });
    }

    constexpr std::array<std::string_view, 13> REPLAY_METADATA_V2_KEYS{
      "version",     "captured_at_ms",     "endpoint",   "method",  "content_type",  "compatibility_headers", "failure_signature",
      "observation", "client_observation", "request_id", "git_sha", "deno_revision", "upstream",
    };

    constexpr std::array<std::string_view, 20> REPLAY_METADATA_V3_KEYS{
      "version",        "captured_at_ms",     "endpoint",    "method",      "content_type",  "compatibility_headers", "failure_signature",
      "observation",    "client_observation", "request_id",  "git_sha",     "deno_revision", "upstream",              "settings",
      "capture_status", "replay_coverage",    "unavailable", "body_sha256", "body_bytes",    "downstream",
    };

    // `value === REPLAY_PLAINTEXT_VERSION_V2 || value === REPLAY_PLAINTEXT_VERSION`
    std::optional<double> replayPlaintextVersion(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      if (number && (*number == replay_model::REPLAY_PLAINTEXT_VERSION_V2 || *number == replay_model::REPLAY_PLAINTEXT_VERSION)) return number;
      return std::nullopt;
    }

    /** The version-3 additions: settings, coverage and the downstream outcome. */
    bool isReplayMetadataV3(const json::value& value) {
      if (!isReplaySettings(rt::get(value, "settings")) || !isReplayDownstream(rt::get(value, "downstream"))) return false;
      if (!isOneOf(rt::get(value, "capture_status"), {"ready", "incomplete"})) return false;
      if (!isOneOf(rt::get(value, "replay_coverage"), {"full", "partial", "unavailable"})) return false;
      if (!isUnavailableReasons(rt::get(value, "unavailable"))) return false;
      const std::optional<std::string_view> digest = rt::getString(value, "body_sha256");
      if (!digest || !replay_model::HEX_DIGEST.test(*digest)) return false;
      const std::optional<double> bodyBytes = rt::getNumber(value, "body_bytes");
      if (!bodyBytes || !rt::Number::isSafeInteger(*bodyBytes) || *bodyBytes < 0) return false;
      return true;
    }

    /** The metadata fields every plaintext version shares. */
    bool isReplayMetadataCommon(const json::value& value, double version) {
      const std::optional<double> capturedAtMs = rt::getNumber(value, "captured_at_ms");
      if (!capturedAtMs || !rt::Number::isSafeInteger(*capturedAtMs) || *capturedAtMs < 0) return false;
      if (!isString(rt::get(value, "endpoint")) || !isString(rt::get(value, "method"))) return false;
      const json::value* contentType = rt::get(value, "content_type");
      if (!(contentType && contentType->is_null()) && !isString(contentType)) return false;
      const json::value* headers = rt::get(value, "compatibility_headers");
      if (!headers || !isCompatibilityHeaders(*headers)) return false;
      if (!isString(rt::get(value, "failure_signature"))) return false;
      if (!isFailureObservation(rt::get(value, "observation")) ||
          !isClientFailureObservation(rt::get(value, "client_observation"), version == replay_model::REPLAY_PLAINTEXT_VERSION)) {
        return false;
      }
      if (!isString(rt::get(value, "request_id")) || !isString(rt::get(value, "git_sha"))) return false;
      if (!isString(rt::get(value, "deno_revision"))) return false;
      return true;
    }

    /** The envelope key set must match the declared plaintext version exactly. */
    bool hasExactReplayMetadataKeys(const json::object& value, double version) {
      const std::span<const std::string_view> expectedKeys = version == replay_model::REPLAY_PLAINTEXT_VERSION
                                                               ? std::span<const std::string_view>(REPLAY_METADATA_V3_KEYS)
                                                               : std::span<const std::string_view>(REPLAY_METADATA_V2_KEYS);
      const std::vector<std::string> actualKeys = rt::objectKeys(value);
      return actualKeys.size() == expectedKeys.size() && std::all_of(actualKeys.begin(), actualKeys.end(), [&expectedKeys](const std::string& key) {
               return std::find(expectedKeys.begin(), expectedKeys.end(), key) != expectedKeys.end();
             });
    }

    bool isReplayMetadata(const json::value& value) {
      if (!utils::isRecord(value)) return false;
      const std::optional<double> version = replayPlaintextVersion(rt::get(value, "version"));
      if (!version) return false;
      // An array has no own `version`, so only an object reaches here.
      if (!hasExactReplayMetadataKeys(value.get_object(), *version)) return false;
      if (!isReplayMetadataCommon(value, *version)) return false;
      try {
        (void)upstream_capture::parseSentinelUpstreamTrace(*rt::get(value, "upstream"));
      } catch (...) {
        return false;
      }
      if (*version == replay_model::REPLAY_PLAINTEXT_VERSION && !isReplayMetadataV3(value)) return false;
      return true;
    }

    std::optional<std::string> nullableText(const json::value* value) {
      if (const std::optional<std::string_view> text = rt::getString(value)) return std::string(*text);
      return std::nullopt;
    }

    // The validated client observation. Version 2 does not validate (or carry) the rich members:
    // what is there and has the declared type is kept, the rest reads as null / false.
    SentinelClientFailureObservation clientObservationFrom(const json::value& value, bool rich) {
      if (rich) return json::value_to<SentinelClientFailureObservation>(value);
      return {
        .status = *rt::getNumber(value, "status"),
        .stream = *rt::getBool(value, "stream"),
        .completed = *rt::getBool(value, "completed"),
        .terminal_type = nullableText(rt::get(value, "terminal_type")),
        .failure_kind = nullableText(rt::get(value, "failure_kind")),
        .framing_valid = *rt::getBool(value, "framing_valid"),
        .provider_route = std::string(*rt::getString(value, "provider_route")),
        .error_code = nullableText(rt::get(value, "error_code")),
        .error_param = nullableText(rt::get(value, "error_param")),
        .terminal_body_base64 = nullableText(rt::get(value, "terminal_body_base64")),
        .terminal_body_truncated = rt::getBool(value, "terminal_body_truncated").value_or(false),
      };
    }

    /**
     * A 4xx rejection the gateway produced without dispatching a provider is
     * decided by the recorded request bytes and the gateway's own validation, so
     * its empty attempt list is correct evidence rather than a missing attempt.
     */
    bool isGatewayReproducibleRejection(const SentinelClientFailureObservation& clientObservation) {
      return clientObservation.status >= 400 && clientObservation.status < 500;
    }

    // `frame(value)`: [8-byte big-endian length, value]
    void frame(std::vector<rt::Bytes>& parts, rt::Bytes value) {
      rt::Bytes length(8);
      std::uint64_t size = value.size();
      for (std::size_t index = 8; index-- > 0;) {
        length[index] = static_cast<std::uint8_t>(size & 0xff);
        size >>= 8;
      }
      parts.push_back(std::move(length));
      parts.push_back(std::move(value));
    }

    std::vector<rt::Bytes> fingerprintPartsOf(const replay_model::AcceptedSentinelReplayInput& input, const std::string& failureSignature, const std::string& purpose,
                                              const upstream_capture::SentinelUpstreamTrace* upstream) {
      std::vector<rt::Bytes> parts;
      frame(parts, replay_model::TEXT_ENCODER.encode(purpose == "fingerprint" ? replay_model::FINGERPRINT_NAMESPACE_V2 : replay_model::CASE_GROUP_NAMESPACE_V1));
      frame(parts, replay_model::TEXT_ENCODER.encode(input.method));
      frame(parts, replay_model::TEXT_ENCODER.encode(input.endpoint));
      frame(parts, replay_model::TEXT_ENCODER.encode(replay_model::stableHeaderText(input.compatibility_headers)));
      // Keep the case-group identity exactly the v1 request-only HMAC. Only the
      // fingerprint gains one appended frame of canonical upstream JSON, so
      // different partial/complete traces cannot suppress one another.
      frame(parts, input.body ? *input.body : rt::Bytes());
      if (purpose == "fingerprint") {
        frame(parts, replay_model::TEXT_ENCODER.encode(failureSignature));
        frame(parts, replay_model::TEXT_ENCODER.encode(upstream ? upstream_capture::canonicalSentinelUpstreamJson(*upstream) : std::string()));
      }
      return parts;
    }


    // A stored array element as a key part (the TS casts the array to KvKey).

    // `typeof value === "object" && value !== null` (utils.isRecord)
  }

  std::optional<rt::Bytes> decodeSentinelReplayKey(const std::string& raw) {
    static const rt::Regex KEY("^[A-Za-z0-9_-]{43}=?$", "");
    if (!KEY.test(raw)) return std::nullopt;
    try {
      rt::Bytes decoded = utils::base64UrlDecode(raw);
      if (static_cast<double>(decoded.size()) == replay_model::REPLAY_KEY_BYTES) return decoded;
      return std::nullopt;
    } catch (...) {
      return std::nullopt;
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ReplayMetadata& value) {
    out = Writer()
            .required("version", value.version)
            .required("captured_at_ms", value.captured_at_ms)
            .required("endpoint", value.endpoint)
            .required("method", value.method)
            .orNull("content_type", value.content_type)
            .required("compatibility_headers", value.compatibility_headers)
            .required("failure_signature", value.failure_signature)
            .required("observation", value.observation)
            .required("client_observation", value.client_observation)
            .required("request_id", value.request_id)
            .required("git_sha", value.git_sha)
            .required("deno_revision", value.deno_revision)
            .required("upstream", value.upstream)
            .orUndefined("settings", value.settings)
            .orUndefined("capture_status", value.capture_status)
            .orUndefined("replay_coverage", value.replay_coverage)
            .orUndefined("unavailable", value.unavailable)
            .orUndefined("body_sha256", value.body_sha256)
            .orUndefined("body_bytes", value.body_bytes)
            .orUndefined("downstream", value.downstream)
            .done();
  }

  rt::Task<std::string> ciphertextDigest(rt::Bytes bytes) {
    const rt::Bytes digest = rt::crypto::subtle::digest("SHA-256", bytes);
    co_await rt::microtask();
    co_return utils::encodeHex(digest);
  }

  replay_model::SentinelReplayPlaintext decodePlaintext(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4 || static_cast<double>(bytes.size()) > replay_model::MAX_REPLAY_PLAINTEXT_BYTES) {
      throw rt::JsError("Sentinel replay envelope size is invalid");
    }
    const std::uint32_t metadataLength = (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
                                         (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
    if (static_cast<double>(metadataLength) > replay_model::MAX_REPLAY_METADATA_BYTES) throw rt::JsError("Sentinel replay metadata is too large");
    const std::size_t bodyOffset = 4 + static_cast<std::size_t>(metadataLength);
    if (bodyOffset > bytes.size()) throw rt::JsError("Sentinel replay metadata length is invalid");
    if (static_cast<double>(bytes.size() - bodyOffset) > replay_model::SENTINEL_REPLAY_MAX_BODY_BYTES) {
      throw rt::JsError("Sentinel replay body is too large");
    }
    const json::value parsed = rt::jsonParse(replay_model::TEXT_DECODER.decode(bytes.subspan(4, metadataLength)));
    if (!isReplayMetadata(parsed)) throw rt::JsError("Sentinel replay metadata is invalid");
    const double version = *rt::getNumber(parsed, "version");
    const bool v3 = version == replay_model::REPLAY_PLAINTEXT_VERSION;
    // An empty JSON array also passes as compatibility headers; it holds none.
    const json::object* headers = rt::getObject(parsed, "compatibility_headers");
    replay_model::SentinelReplayPlaintext plaintext{
      .version = version,
      .captured_at_ms = *rt::getNumber(parsed, "captured_at_ms"),
      .endpoint = std::string(*rt::getString(parsed, "endpoint")),
      .method = std::string(*rt::getString(parsed, "method")),
      .content_type = nullableText(rt::get(parsed, "content_type")),
      .compatibility_headers = headers ? *headers : json::object(),
      .failure_signature = std::string(*rt::getString(parsed, "failure_signature")),
      .observation = json::value_to<replay_model::SentinelFailureObservation>(*rt::get(parsed, "observation")),
      .client_observation = clientObservationFrom(*rt::get(parsed, "client_observation"), v3),
      .request_id = std::string(*rt::getString(parsed, "request_id")),
      .git_sha = std::string(*rt::getString(parsed, "git_sha")),
      .deno_revision = std::string(*rt::getString(parsed, "deno_revision")),
      .upstream = upstream_capture::parseSentinelUpstreamTrace(*rt::get(parsed, "upstream")),
      .body = std::make_shared<rt::Bytes>(replay_model::cloneBytes(bytes.subspan(bodyOffset))),
    };
    if (v3) {
      plaintext.settings = json::value_to<replay_model::SentinelReplaySettings>(*rt::get(parsed, "settings"));
      plaintext.capture_status = std::string(*rt::getString(parsed, "capture_status"));
      plaintext.replay_coverage = std::string(*rt::getString(parsed, "replay_coverage"));
      plaintext.unavailable = json::value_to<std::vector<std::string>>(*rt::get(parsed, "unavailable"));
      plaintext.body_sha256 = std::string(*rt::getString(parsed, "body_sha256"));
      plaintext.body_bytes = *rt::getNumber(parsed, "body_bytes");
      plaintext.downstream = json::value_to<replay_model::SentinelReplayDownstream>(*rt::get(parsed, "downstream"));
    }
    return plaintext;
  }

  std::optional<rt::KvKey> dedupeManifestKey(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return std::nullopt;
    const rt::KvValue& manifestKey = value["manifest_key"];
    if (!manifestKey.isArray() || manifestKey.asArray().size() != 7) return std::nullopt;
    rt::KvKey key;
    for (const rt::KvValue& item : manifestKey.asArray()) {
      std::optional<rt::KvKeyPart> part = rt::kvKeyPartFromValue(item);
      // The TS casts any 7-element array to a key; a stored dedupe row only ever holds key parts.
      if (!part) throw rt::TypeError("KV value is " + std::string(item.kind()) + ", not a key part");
      key.push_back(std::move(*part));
    }
    return key;
  }

  rt::KvValue dedupeRecordFor(const rt::KvKey& manifestKey, const replay_model::SentinelReplayManifest& manifest) {
    rt::KvObject object;
    object.set("manifest_key", rt::kvValueFromKey(manifestKey));
    object.set("captured_at_ms", rt::KvValue(manifest.captured_at_ms));
    object.set("expires_at_ms", rt::KvValue(manifest.expires_at_ms));
    return rt::KvValue(std::move(object));
  }

  std::optional<DedupeWinnerIdentity> dedupeWinnerIdentity(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return std::nullopt;
    const rt::KvValue& capturedAtMs = value["captured_at_ms"];
    const rt::KvValue& expiresAtMs = value["expires_at_ms"];
    if (!capturedAtMs.isNumber() || !rt::Number::isSafeInteger(capturedAtMs.asNumber()) || !expiresAtMs.isNumber() ||
        !rt::Number::isSafeInteger(expiresAtMs.asNumber()))
      return std::nullopt;
    return DedupeWinnerIdentity{.capturedAtMs = capturedAtMs.asNumber(), .expiresAtMs = expiresAtMs.asNumber()};
  }

  replay_model::SentinelReplayDownstream downstreamObservation(const SentinelClientFailureObservation& observation) {
    return {
      .terminal_type = observation.terminal_type,
      .failure_kind = observation.failure_kind,
      .error_code = observation.error_code,
      .error_param = observation.error_param,
      .body_base64 = observation.terminal_body_base64,
      .body_truncated = observation.terminal_body_truncated,
    };
  }

  rt::Bytes encodePlaintext(const ReplayMetadata& metadata, std::span<const std::uint8_t> body) { return encodePlaintext(json::value_from(metadata), body); }

  rt::Bytes encodePlaintext(const json::value& metadata, std::span<const std::uint8_t> body) {
    const rt::Bytes metadataBytes = replay_model::TEXT_ENCODER.encode(rt::jsonStringify(metadata));
    if (static_cast<double>(metadataBytes.size()) > replay_model::MAX_REPLAY_METADATA_BYTES) throw rt::JsError("Sentinel replay metadata is too large");
    if (static_cast<double>(body.size()) > replay_model::SENTINEL_REPLAY_MAX_BODY_BYTES) throw rt::JsError("Sentinel replay body is too large");
    const auto length = static_cast<std::uint32_t>(metadataBytes.size());
    const std::array<std::uint8_t, 4> size{static_cast<std::uint8_t>(length >> 24), static_cast<std::uint8_t>(length >> 16),
                                           static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length)};
    return replay_model::concatBytes({std::span<const std::uint8_t>(size), std::span<const std::uint8_t>(metadataBytes), body});
  }

  rt::Bytes encryptionAdditionalData(const std::string& fingerprint) {
    std::string text = "uos-sentinel-replay-v1";
    text.push_back('\0');
    text += fingerprint;
    return replay_model::TEXT_ENCODER.encode(text);
  }

  std::vector<rt::Bytes> fingerprintParts(const replay_model::AcceptedSentinelReplayInput& input, const std::string& failureSignature, const std::string& purpose) {
    return fingerprintPartsOf(input, failureSignature, purpose, nullptr);
  }

  std::vector<rt::Bytes> fingerprintParts(const replay_model::AcceptedSentinelReplayInput& input, const std::string& failureSignature, const std::string& purpose,
                                          const upstream_capture::SentinelUpstreamTrace& upstream) {
    return fingerprintPartsOf(input, failureSignature, purpose, &upstream);
  }

  rt::Task<rt::Bytes> gunzip(rt::Bytes bytes) {
    return rt::gzip::gunzip(std::move(bytes), static_cast<std::size_t>(replay_model::MAX_REPLAY_PLAINTEXT_BYTES), "Sentinel replay plaintext exceeds its size limit");
  }

  rt::Task<rt::Bytes> gzip(rt::Bytes bytes) { return rt::gzip::gzip(std::move(bytes)); }

  rt::Task<std::string> hmacHex(rt::Bytes keyBytes, std::string purpose, std::vector<rt::Bytes> parts) {
    const rt::crypto::CryptoKey key = importHmacKey(keyBytes, purpose);
    co_await rt::microtask();
    rt::Bytes message = replay_model::concatBytes(parts);
    // The TS parts alias buffers their owners zero later; these are this call's own copies.
    for (rt::Bytes& part : parts) zero(part);
    rt::Reason failure;
    std::string digestHex;
    try {
      const rt::Bytes digest = rt::crypto::subtle::sign("HMAC", key, message);
      digestHex = utils::encodeHex(digest);
    } catch (...) {
      failure = std::current_exception();
    }
    zero(message);
    if (failure) std::rethrow_exception(failure);
    co_await rt::microtask();
    co_return digestHex;
  }

  rt::Task<rt::crypto::CryptoKey> importAesKey(rt::Bytes keyBytes) {
    rt::Bytes derived = deriveKeyBytes(keyBytes, "encryption");
    const rt::crypto::CryptoKey key = rt::crypto::subtle::importKey("raw", derived, rt::crypto::Algorithm{.name = "AES-GCM"}, false, {"encrypt", "decrypt"});
    zero(derived);
    co_await rt::microtask();
    co_return key;
  }

  bool isCompatibilityHeaders(const json::value& value) {
    if (!isStringRecord(value)) return false;
    // An array's entries are index names, never allowlisted.
    if (const json::array* items = value.if_array()) return items->empty();
    for (const json::key_value_pair& member : value.get_object()) {
      const std::string headerValue(member.value().get_string());
      if (!replay_model::COMPATIBILITY_HEADER_NAME_SET.contains(std::string_view(member.key())) || headerValue.empty() || rt::trim(headerValue) != headerValue) {
        return false;
      }
    }
    return true;
  }

  std::string replayCoverageFor(const upstream_capture::SentinelUpstreamTrace& upstream, const SentinelClientFailureObservation& clientObservation,
                                const std::vector<replay_model::SentinelReplayUnavailableReason>& unavailable) {
    if (rt::includes(unavailable, std::string("upstream_trace_truncated")) || rt::includes(unavailable, std::string("upstream_attempt_pending"))) return "partial";
    if (upstream.attempts.empty()) {
      // No provider was dispatched. Only a deterministic gateway-side rejection
      // can be reproduced without provider state.
      const double status = clientObservation.status;
      return status >= 400 && status < 500 ? "full" : "unavailable";
    }
    return unavailable.empty() ? "full" : "partial";
  }

  std::vector<replay_model::SentinelReplayUnavailableReason> replayUnavailableReasons(const upstream_capture::SentinelUpstreamTrace& upstream,
                                                                                    const SentinelClientFailureObservation& clientObservation) {
    std::vector<replay_model::SentinelReplayUnavailableReason> reasons;
    const auto some = [&upstream](auto predicate) { return std::any_of(upstream.attempts.begin(), upstream.attempts.end(), predicate); };
    using Attempt = upstream_capture::SentinelUpstreamAttempt;
    if (upstream.attempts.empty() && !isGatewayReproducibleRejection(clientObservation)) reasons.push_back("upstream_trace_empty");
    if (upstream.attempts_truncated || upstream.bytes_truncated || upstream.chunks_truncated) reasons.push_back("upstream_trace_truncated");
    if (some([](const Attempt& attempt) { return attempt.terminal == "pending"; })) reasons.push_back("upstream_attempt_pending");
    if (some([](const Attempt& attempt) { return !attempt.started_at_ms.has_value(); })) reasons.push_back("upstream_timing_unavailable");
    if (some([](const Attempt& attempt) { return !attempt.headers.has_value(); })) reasons.push_back("upstream_response_headers_unavailable");
    if (some([](const Attempt& attempt) { return attempt.headers_truncated == true; })) reasons.push_back("upstream_response_headers_unavailable");
    if (!clientObservation.terminal_body_base64) reasons.push_back("downstream_terminal_body_unavailable");
    // An unreported serving-model label is optional diagnostic metadata. It is
    // never evidence the capture is missing: the recorded provider response bytes
    // are what replay consumes, so an unknown label must not make byte-complete
    // evidence permanently partial.
    return reasons;
  }

  replay_model::SentinelReplaySettings requestSettingsFromBody(std::span<const std::uint8_t> body, const std::string& providerRoute,
                                                               const std::optional<bool>& streamObserved) {
    const replay_model::SentinelReplaySettings observed{
      .source = "unavailable",
      .provider_route = providerRoute,
      .model_requested = std::nullopt,
      .reasoning_requested = std::nullopt,
      .stream_requested = std::nullopt,
      .stream_observed = streamObserved,
    };
    try {
      const json::value parsed = rt::jsonParse(replay_model::TEXT_DECODER.decode(body));
      if (!utils::isRecord(parsed) || parsed.is_array()) return observed;
      const json::value* reasoningMember = rt::get(parsed, "reasoning");
      std::optional<std::string> reasoning =
        utils::isRecord(reasoningMember) ? replay_observation::boundedFailureKind(rt::get(reasoningMember, "effort")) : std::nullopt;
      if (!reasoning) reasoning = replay_observation::boundedFailureKind(rt::get(parsed, "reasoning_effort"));
      return {
        .source = "recorded_request_body",
        .provider_route = providerRoute,
        .model_requested = replay_observation::boundedFailureKind(rt::get(parsed, "model")),
        .reasoning_requested = reasoning,
        .stream_requested = rt::getBool(parsed, "stream"),
        .stream_observed = streamObserved,
      };
    } catch (...) {
      return observed;
    }
  }

  std::vector<rt::Bytes> splitChunks(std::span<const std::uint8_t> bytes) {
    std::vector<rt::Bytes> chunks;
    const auto chunkBytes = static_cast<std::size_t>(replay_model::SENTINEL_REPLAY_CHUNK_BYTES);
    for (std::size_t offset = 0; offset < bytes.size(); offset += chunkBytes) {
      chunks.push_back(replay_model::cloneBytes(bytes.subspan(offset, std::min(chunkBytes, bytes.size() - offset))));
    }
    if (chunks.empty()) chunks.emplace_back();
    return chunks;
  }
}
