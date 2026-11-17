#include "uos/sentinel/replay_read.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/sentinel/incident_outbox.hpp"
#include "uos/sentinel/replay_envelope.hpp"
#include "uos/sentinel/replay_observation.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"
#include "uos/utils.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <openssl/crypto.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <utility>

namespace uos::sentinel::replay_read {
  namespace {
    using replay_model::ExportedSentinelReplayCapture;
    using replay_model::SentinelReplayCaptureStatusRow;
    using replay_model::SentinelReplayManifest;
    using replay_model::SentinelReplayPlaintext;

    // `/^[A-Za-z0-9_-]+$/`
    const rt::Regex& URL_SAFE() {
      static const rt::Regex pattern("^[A-Za-z0-9_-]+$", "");
      return pattern;
    }

    // `/^[0-9a-f]{64}$/`
    const rt::Regex& LOWER_HEX_SHA256() {
      static const rt::Regex pattern("^[0-9a-f]{64}$", "");
      return pattern;
    }

    // `typeof value === "object" && value !== null` (utils.isRecord)
    bool isText(const rt::KvValue& value, std::string_view text) { return value.isString() && value.asString() == text; }
    // `typeof value === "number" && Number.isSafeInteger(value)`
    bool isSafeIntegerNumber(const rt::KvValue& value) { return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()); }
    double lengthOf(const std::string& text) { return static_cast<double>(rt::length(text)); }

    // `value instanceof Uint8Array`: its bytes.

    // `x.fill(0)` on a buffer that held ciphertext or decrypted replay content.
    void zero(rt::Bytes& bytes) {
      if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
    }

    // `left === right` on two key parts: by value, except Uint8Arrays, which are distinct objects.

    // `key[index] === text` / `key[index] === number` (undefined past the end).
    bool partIs(const rt::KvKey& key, std::size_t index, std::string_view text) {
      return index < key.size() && key[index].isString() && key[index].string() == text;
    }
    bool partIs(const rt::KvKey& key, std::size_t index, double number) { return index < key.size() && key[index].isNumber() && key[index].number() == number; }

    // `SENTINEL_REPLAY_MANIFEST_PREFIX.every((part, index) => key[index] === part)`
    bool hasManifestPrefix(const rt::KvKey& key) {
      const rt::KvKey& prefix = replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX;
      for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (index >= key.size() || !rt::kvKeyPartStrictEquals(key[index], prefix[index])) return false;
      }
      return true;
    }


    // `String(value)` of a stored value, for the SDK's key encoder error.

    // A stored array element passed on as a key part: the SDK packs strings, numbers, bigints,
    // booleans and Uint8Arrays and rejects anything else.

    // A stored array used as a KV key.

    // The typed view of a value isSentinelReplayCaptureStatusRow accepted.
    SentinelReplayCaptureStatusRow statusRowOf(const rt::KvValue& value) {
      const rt::KvValue& reason = value["reason"];
      const rt::KvValue& manifestKey = value["manifest_key"];
      const rt::KvValue& fingerprint = value["fingerprint"];
      const rt::KvValue& expiresAtMs = value["expires_at_ms"];
      return {
        .version = value["version"].asNumber(),
        .request_id = value["request_id"].asString(),
        .status = value["status"].asString(),
        .reason = reason.isString() ? std::optional<std::string>(reason.asString()) : std::nullopt,
        .captured_at_ms = value["captured_at_ms"].asNumber(),
        .manifest_key = manifestKey.isArray() ? std::optional<rt::KvKey>(rt::kvKeyFromValue(manifestKey)) : std::nullopt,
        .fingerprint = fingerprint.isString() ? std::optional<std::string>(fingerprint.asString()) : std::nullopt,
        .expires_at_ms = expiresAtMs.isNumber() ? std::optional<double>(expiresAtMs.asNumber()) : std::nullopt,
      };
    }

    std::vector<std::string> base64UrlChunks(const std::vector<rt::Bytes>& chunks) {
      std::vector<std::string> encoded;
      encoded.reserve(chunks.size());
      for (const rt::Bytes& chunk : chunks) encoded.push_back(utils::base64UrlEncode(chunk));
      return encoded;
    }

    bool decodedIvIsValid(const std::string& value) {
      if (lengthOf(value) < 16 || lengthOf(value) > 24 || !URL_SAFE().test(value)) return false;
      try {
        return static_cast<double>(utils::base64UrlDecode(value).size()) == replay_model::AES_GCM_IV_BYTES;
      } catch (...) {
        return false;
      }
    }
  }

  bool isSentinelReplayManifest(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return false;
    const rt::KvValue& version = value["version"];
    const rt::KvValue& captureId = value["capture_id"];
    const rt::KvValue& fingerprint = value["fingerprint"];
    const rt::KvValue& caseGroupDigest = value["case_group_digest"];
    const rt::KvValue& capturedAtMs = value["captured_at_ms"];
    const rt::KvValue& expiresAtMs = value["expires_at_ms"];
    const rt::KvValue& iv = value["iv"];
    const rt::KvValue& chunkCount = value["chunk_count"];
    const rt::KvValue& ciphertextBytes = value["ciphertext_bytes"];
    const rt::KvValue& storedBytes = value["stored_bytes"];
    const rt::KvValue& requestId = value["request_id"];
    if (!(version.isNumber() && version.asNumber() == replay_model::ENVELOPE_VERSION) || !captureId.isString() || lengthOf(captureId.asString()) < 1 ||
        lengthOf(captureId.asString()) > replay_model::MAX_CAPTURE_ID_CHARS || !replay_model::CAPTURE_ID.test(captureId.asString()) || !fingerprint.isString() ||
        !replay_model::HEX_DIGEST.test(fingerprint.asString()) || !caseGroupDigest.isString() || !replay_model::HEX_DIGEST.test(caseGroupDigest.asString()) ||
        !isSafeIntegerNumber(capturedAtMs) || capturedAtMs.asNumber() < 0 || !isSafeIntegerNumber(expiresAtMs) ||
        expiresAtMs.asNumber() != capturedAtMs.asNumber() + replay_model::SENTINEL_REPLAY_TTL_MS || !isText(value["algorithm"], "AES-256-GCM") ||
        !isText(value["compression"], "gzip") || !iv.isString() || !decodedIvIsValid(iv.asString()) || !isSafeIntegerNumber(chunkCount) ||
        chunkCount.asNumber() < 1 || chunkCount.asNumber() > replay_model::MAX_REPLAY_CHUNKS || !isSafeIntegerNumber(ciphertextBytes) ||
        ciphertextBytes.asNumber() < 16 || ciphertextBytes.asNumber() > replay_model::MAX_REPLAY_CIPHERTEXT_BYTES ||
        (!storedBytes.isUndefined() && (!isSafeIntegerNumber(storedBytes) || storedBytes.asNumber() <= 0)) ||
        (!requestId.isUndefined() && !replay_observation::isSentinelReplayRequestId(requestId)))
      return false;
    const double minimumBytes = (chunkCount.asNumber() - 1) * replay_model::SENTINEL_REPLAY_CHUNK_BYTES + 1;
    return ciphertextBytes.asNumber() >= minimumBytes && ciphertextBytes.asNumber() <= chunkCount.asNumber() * replay_model::SENTINEL_REPLAY_CHUNK_BYTES;
  }

  namespace {
    double expectedChunkBytes(const SentinelReplayManifest& manifest, double index) {
      return index < manifest.chunk_count - 1 ? replay_model::SENTINEL_REPLAY_CHUNK_BYTES
                                              : manifest.ciphertext_bytes - (manifest.chunk_count - 1) * replay_model::SENTINEL_REPLAY_CHUNK_BYTES;
    }

    void assertChunkSize(const SentinelReplayManifest& manifest, double index, std::span<const std::uint8_t> bytes) {
      if (static_cast<double>(bytes.size()) != expectedChunkBytes(manifest, index)) {
        throw rt::JsError("Sentinel replay chunk size does not match its manifest");
      }
    }
  }

  bool isExportedSentinelReplayCapture(const rt::KvValue& value) {
    if (!rt::kvIsObject(value) || !isSentinelReplayManifest(value["manifest"]) || !value["chunks"].isArray()) return false;
    const SentinelReplayManifest manifest = toSentinelReplayManifest(value["manifest"]);
    const rt::KvArray& chunks = value["chunks"].asArray();
    if (static_cast<double>(chunks.size()) != manifest.chunk_count) return false;
    // `Math.ceil(SENTINEL_REPLAY_CHUNK_BYTES / 3) * 4`
    constexpr double maximumEncodedChars = rt::Math::ceil(replay_model::SENTINEL_REPLAY_CHUNK_BYTES / 3) * 4;
    try {
      for (std::size_t index = 0; index < chunks.size(); index++) {
        const rt::KvValue& encoded = chunks[index];
        if (!encoded.isString() || lengthOf(encoded.asString()) < 1 || lengthOf(encoded.asString()) > maximumEncodedChars || !URL_SAFE().test(encoded.asString()))
          return false;
        assertChunkSize(manifest, static_cast<double>(index), utils::base64UrlDecode(encoded.asString()));
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  rt::Task<std::vector<rt::Bytes>> getChunks(rt::Kv& kv, SentinelReplayManifest manifest) {
    std::vector<rt::Bytes> chunks;
    for (double offset = 0; offset < manifest.chunk_count; offset += 10) {
      // `Array.from({ length: Math.min(10, manifest.chunk_count - offset) }, ...)`: ToLength of the count.
      const double count = rt::Math::min(10, manifest.chunk_count - offset);
      const double length = std::isnan(count) ? 0 : rt::Math::max(0, rt::Math::trunc(count));
      std::vector<rt::KvKey> keys;
      for (double index = 0; index < length; index++) keys.push_back(rt::KvKey{replay_model::SENTINEL_REPLAY_CHUNK_PREFIX, manifest.capture_id, offset + index});
      const std::vector<rt::KvEntryMaybe<>> entries = co_await kv.getMany(std::move(keys));
      for (std::size_t entryIndex = 0; entryIndex < entries.size(); entryIndex++) {
        const rt::KvEntryMaybe<>& entry = entries[entryIndex];
        const std::optional<std::span<const std::uint8_t>> bytes = rt::kvUint8ArrayBytes(entry.value);
        if (!bytes) throw rt::JsError("Sentinel replay chunk is missing");
        assertChunkSize(manifest, offset + static_cast<double>(entryIndex), *bytes);
        chunks.push_back(replay_model::cloneBytes(*bytes));
      }
    }
    co_return chunks;
  }

  namespace {
    // `manifestEntryMatchesKey(entry)` over the entry's key and its (validated) manifest value.
    bool manifestEntryMatchesKey(const rt::KvKey& key, const SentinelReplayManifest& value) {
      if (!hasManifestPrefix(key)) return false;
      const std::size_t prefixLength = replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX.size();
      // `const suffix = entry.key.slice(SENTINEL_REPLAY_MANIFEST_PREFIX.length)`
      return key.size() == prefixLength + 3 && partIs(key, prefixLength, value.captured_at_ms) && partIs(key, prefixLength + 1, value.fingerprint) &&
             partIs(key, prefixLength + 2, value.capture_id);
    }
  }

  bool manifestMatchesKey(const rt::KvKey& key, const SentinelReplayManifest& manifest) {
    const std::size_t prefixLength = replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX.size();
    // `key.slice(SENTINEL_REPLAY_MANIFEST_PREFIX.length)` has length 3 exactly when the key has 7 parts.
    return hasManifestPrefix(key) && key.size() == prefixLength + 3 && partIs(key, prefixLength, manifest.captured_at_ms) &&
           partIs(key, prefixLength + 1, manifest.fingerprint) && partIs(key, prefixLength + 2, manifest.capture_id);
  }

  namespace {
    /**
     * Project a stored manifest onto the frozen export wire schema. `stored_bytes`
     * and `request_id` are additive retention-accounting fields of the stored row;
     * the export contract is frozen and its strict consumer rejects unknown
     * manifest keys, so those fields never leave the store.
     */
    SentinelReplayManifest exportedManifest(const SentinelReplayManifest& manifest) {
      return {
        .version = manifest.version,
        .capture_id = manifest.capture_id,
        .fingerprint = manifest.fingerprint,
        .case_group_digest = manifest.case_group_digest,
        .captured_at_ms = manifest.captured_at_ms,
        .expires_at_ms = manifest.expires_at_ms,
        .algorithm = manifest.algorithm,
        .compression = manifest.compression,
        .iv = manifest.iv,
        .chunk_count = manifest.chunk_count,
        .ciphertext_bytes = manifest.ciphertext_bytes,
      };
    }

    // `options.cursor !== undefined && (options.cursor.length < 1 || options.cursor.length > 2_048 || !KV_CURSOR.test(options.cursor))`
    bool invalidCursor(const std::optional<std::string>& cursor) {
      return cursor && (lengthOf(*cursor) < 1 || lengthOf(*cursor) > 2'048 || !replay_model::KV_CURSOR.test(*cursor));
    }
  }

  rt::Task<EncryptedSentinelReplayPage> listEncryptedSentinelReplays(rt::Kv& kv, ListEncryptedSentinelReplaysOptions options) {
    if (!rt::Number::isSafeInteger(options.afterMs) || options.afterMs < 0) {
      throw rt::JsError("Sentinel replay export start is invalid");
    }
    if (!rt::Number::isSafeInteger(options.beforeMs) || options.beforeMs < options.afterMs || options.beforeMs >= rt::Number::MAX_SAFE_INTEGER) {
      throw rt::JsError("Sentinel replay export end is invalid");
    }
    if (options.limit && *options.limit != replay_model::SENTINEL_REPLAY_EXPORT_PAGE_LIMIT) {
      throw rt::JsError("Sentinel replay export limit must be one");
    }
    if (invalidCursor(options.cursor)) {
      throw rt::JsError("Sentinel replay export cursor is invalid");
    }
    rt::KvListSelector selector;
    selector.prefix = replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX;
    selector.start = rt::KvKey{replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX, options.afterMs};
    rt::KvListOptions listOptions;
    listOptions.limit = replay_model::SENTINEL_REPLAY_EXPORT_PAGE_LIMIT;
    listOptions.cursor = options.cursor;
    const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector), std::move(listOptions));
    std::vector<ExportedSentinelReplayCapture> captures;
    bool rangeExhausted = false;
    // `for await (const entry of iterator)`: every pass leaves the loop, by `break` or a throw, and
    // both close the iterator first.
    const std::optional<rt::KvEntry<>> entry = co_await iterator->next();
    if (entry) {
      rt::Reason failure;
      try {
        if (!isSentinelReplayManifest(entry->value)) throw rt::JsError("Sentinel replay manifest is invalid");
        const SentinelReplayManifest manifest = toSentinelReplayManifest(entry->value);
        if (!manifestEntryMatchesKey(entry->key, manifest)) throw rt::JsError("Sentinel replay manifest is invalid");
        if (manifest.captured_at_ms < options.afterMs) throw rt::JsError("Sentinel replay manifest order is invalid");
        if (manifest.captured_at_ms > options.beforeMs) {
          rangeExhausted = true;
        } else {
          auto reading = getChunks(kv, manifest);
          const std::vector<rt::Bytes> chunks = co_await std::move(reading);
          captures.push_back(ExportedSentinelReplayCapture{.manifest = exportedManifest(manifest), .chunks = base64UrlChunks(chunks)});
        }
      } catch (...) {
        failure = std::current_exception();
      }
      co_await iterator->return_();
      if (failure) std::rethrow_exception(failure);
    }
    std::string cursor = rangeExhausted ? std::string() : iterator->cursor();
    co_return EncryptedSentinelReplayPage{.captures = std::move(captures), .cursor = std::move(cursor)};
  }

  namespace {
    // `Readonly<{ fingerprint: string; manifestKey: KvKey }>`: the key is the stored array as it
    // is (the TS hands it to kv.get unchecked).
    struct IncidentCaptureReference {
      std::string fingerprint;
      rt::KvValue manifestKey;
    };

    /** Validate one capture reference row and return its fingerprint and manifest key. */
    IncidentCaptureReference incidentCaptureReference(const rt::KvEntry<>& reference, double prefixLength) {
      // `reference.key.at(-1)`
      const rt::KvKeyPart* fingerprint = reference.key.empty() ? nullptr : &reference.key.back();
      if (static_cast<double>(reference.key.size()) != prefixLength + 1 || !fingerprint || !fingerprint->isString() ||
          !LOWER_HEX_SHA256().test(fingerprint->string()) || !incident_outbox::isSentinelIncidentCaptureReference(reference.value))
        throw rt::JsError("Sentinel incident replay reference is invalid");
      return {.fingerprint = fingerprint->string(), .manifestKey = reference.value["manifest_key"]};
    }

    /**
     * Best-effort CAS cleanup of a reference whose manifest is gone. The check is
     * pinned to the listed versionstamp so a concurrent writer that re-created the
     * reference wins; the scan still advances past the entry either way.
     */
    rt::Task<void> dropStaleCaptureReference(rt::Kv& kv, rt::KvEntry<> reference) {
      const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(rt::AtomicCheck(reference.key, reference.versionstamp)).delete_(reference.key);
      try {
        co_await operation->commit();
      } catch (...) {
        // `.catch(() => {})`
      }
    }
  }

  rt::Task<EncryptedSentinelReplayPage> listEncryptedSentinelIncidentReplays(rt::Kv& kv, ListEncryptedSentinelIncidentReplaysOptions options) {
    if (!incident_outbox::isSentinelIncidentId(rt::KvValue(options.incidentId))) throw rt::JsError("Sentinel incident ID is invalid");
    if (options.limit && *options.limit != replay_model::SENTINEL_REPLAY_EXPORT_PAGE_LIMIT) {
      throw rt::JsError("Sentinel replay export limit must be one");
    }
    if (invalidCursor(options.cursor)) throw rt::JsError("Sentinel replay export cursor is invalid");
    const rt::KvKey prefix{incident_outbox::SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, options.incidentId};
    std::vector<ExportedSentinelReplayCapture> captures;
    std::optional<std::string> scanCursor = options.cursor;
    for (;;) {
      rt::KvListSelector selector;
      selector.prefix = prefix;
      rt::KvListOptions listOptions;
      listOptions.limit = replay_model::SENTINEL_REPLAY_EXPORT_PAGE_LIMIT;
      listOptions.cursor = scanCursor;
      const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector), std::move(listOptions));
      // `for await (const entry of iterator) { reference = entry; break; }`
      const std::optional<rt::KvEntry<>> reference = co_await iterator->next();
      if (reference) co_await iterator->return_();
      // Exhaustion is an empty page, never a fault: an incident whose captures
      // were all evicted or expired genuinely has nothing left to export.
      if (!reference) co_return EncryptedSentinelReplayPage{.captures = std::move(captures), .cursor = ""};
      const std::string resumeCursor = iterator->cursor();
      const IncidentCaptureReference captureReference = incidentCaptureReference(*reference, static_cast<double>(prefix.size()));
      const rt::KvKey manifestKey = rt::kvKeyFromValue(captureReference.manifestKey);
      const rt::KvEntryMaybe<> manifestEntry = co_await kv.get(manifestKey);
      if (manifestEntry.value.isNull()) {
        auto dropping = dropStaleCaptureReference(kv, *reference);
        co_await std::move(dropping);
        scanCursor = resumeCursor;
        continue;
      }
      if (!isSentinelReplayManifest(manifestEntry.value)) throw rt::JsError("Sentinel incident replay manifest is unavailable");
      const SentinelReplayManifest manifest = toSentinelReplayManifest(manifestEntry.value);
      if (manifest.fingerprint != captureReference.fingerprint || !manifestMatchesKey(manifestKey, manifest))
        throw rt::JsError("Sentinel incident replay manifest is unavailable");
      auto reading = getChunks(kv, manifest);
      const std::vector<rt::Bytes> chunks = co_await std::move(reading);
      captures.push_back(ExportedSentinelReplayCapture{.manifest = exportedManifest(manifest), .chunks = base64UrlChunks(chunks)});
      co_return EncryptedSentinelReplayPage{.captures = std::move(captures), .cursor = resumeCursor};
    }
  }

  rt::Task<SentinelReplayCaptureStatusRow> readSentinelReplayCaptureStatus(rt::Kv& kv, std::string requestId) {
    const double nowMs = rt::dateNow();
    auto reading = readSentinelReplayCaptureStatus(kv, std::move(requestId), nowMs);
    SentinelReplayCaptureStatusRow row = co_await std::move(reading);
    co_return row;
  }

  rt::Task<SentinelReplayCaptureStatusRow> readSentinelReplayCaptureStatus(rt::Kv& kv, std::string requestId, double nowMs) {
    if (!replay_observation::isSentinelReplayRequestId(rt::KvValue(requestId))) throw rt::JsError("Sentinel replay request ID is invalid");
    const rt::KvKey statusKey = replay_model::requestStatusKey(requestId);
    const rt::KvEntryMaybe<> entry = co_await kv.get(statusKey);
    if (entry.value.isNull()) {
      std::optional<replay_retention_schema::SentinelReplayLedgerSnapshot> ledger;
      try {
        ledger = co_await replay_retention_schema::readSentinelReplayLedgerSnapshot(kv);
      } catch (...) {
        ledger = std::nullopt;  // `.catch(() => null)`
      }
      const bool pruned = (ledger ? ledger->ledger.status_pruned_records : 0) > 0;
      co_return SentinelReplayCaptureStatusRow{
        .version = 1,
        .request_id = requestId,
        .status = pruned ? "status_not_retained" : "unknown",
        .reason = pruned ? std::string(replay_retention_schema::SENTINEL_REPLAY_STATUS_NOT_RETAINED) : std::string("no_capture_record"),
        .captured_at_ms = nowMs,
        .manifest_key = std::nullopt,
        .fingerprint = std::nullopt,
        .expires_at_ms = std::nullopt,
      };
    }
    if (!replay_observation::isSentinelReplayCaptureStatusRow(entry.value)) throw rt::JsError("Sentinel replay capture status record is invalid");
    SentinelReplayCaptureStatusRow row = statusRowOf(entry.value);
    if (row.expires_at_ms && *row.expires_at_ms <= nowMs) {
      row.status = "expired";  // `{ ...entry.value, status: "expired" }`
      co_return row;
    }
    co_return row;
  }

  rt::Task<EncryptedSentinelRequestReplays> listEncryptedSentinelReplaysByRequestId(rt::Kv& kv, std::string requestId) {
    const double nowMs = rt::dateNow();
    auto listing = listEncryptedSentinelReplaysByRequestId(kv, std::move(requestId), nowMs);
    EncryptedSentinelRequestReplays result = co_await std::move(listing);
    co_return result;
  }

  rt::Task<EncryptedSentinelRequestReplays> listEncryptedSentinelReplaysByRequestId(rt::Kv& kv, std::string requestId, double nowMs) {
    auto reading = readSentinelReplayCaptureStatus(kv, std::move(requestId), nowMs);
    const SentinelReplayCaptureStatusRow status = co_await std::move(reading);
    if (!status.manifest_key || !status.fingerprint) co_return EncryptedSentinelRequestReplays{.captures = {}, .status = status};
    const rt::KvEntryMaybe<> manifestEntry = co_await kv.get(*status.manifest_key);
    const bool usable = manifestEntry.value.truthy() && isSentinelReplayManifest(manifestEntry.value);
    const std::optional<SentinelReplayManifest> manifest = usable ? std::optional(toSentinelReplayManifest(manifestEntry.value)) : std::nullopt;
    if (!manifest || manifest->fingerprint != *status.fingerprint || !manifestMatchesKey(*status.manifest_key, *manifest)) {
      // A missing manifest is either a retention eviction or a TTL/expiry race.
      // The bounded eviction tombstone distinguishes them truthfully; absence of
      // a tombstone keeps the existing expired classification.
      const rt::KvKey tombstoneKey = replay_retention_schema::sentinelReplayEvictionKey(*status.fingerprint);
      const rt::KvEntryMaybe<> tombstone = co_await kv.get(tombstoneKey);
      SentinelReplayCaptureStatusRow next = status;
      if (rt::kvIsObject(tombstone.value) && isText(tombstone.value["reason"], replay_retention_schema::SENTINEL_REPLAY_EVICTION_REASON)) {
        next.status = "evicted";
        next.reason = std::string(replay_retention_schema::SENTINEL_REPLAY_EVICTION_REASON);
        co_return EncryptedSentinelRequestReplays{.captures = {}, .status = next};
      }
      if (rt::kvIsObject(tombstone.value) && isText(tombstone.value["reason"], replay_retention_schema::SENTINEL_REPLAY_EXPIRED_REASON)) {
        next.status = "expired";
        next.reason = std::string(replay_retention_schema::SENTINEL_REPLAY_EXPIRED_REASON);
        co_return EncryptedSentinelRequestReplays{.captures = {}, .status = next};
      }
      next.status = "expired";
      next.reason = "manifest_unavailable";
      co_return EncryptedSentinelRequestReplays{.captures = {}, .status = next};
    }
    auto chunkReading = getChunks(kv, *manifest);
    const std::vector<rt::Bytes> chunks = co_await std::move(chunkReading);
    std::vector<ExportedSentinelReplayCapture> captures;
    captures.push_back(ExportedSentinelReplayCapture{.manifest = exportedManifest(*manifest), .chunks = base64UrlChunks(chunks)});
    co_return EncryptedSentinelRequestReplays{.captures = std::move(captures), .status = status};
  }

  rt::Task<SentinelReplayPlaintext> decryptExportedSentinelReplay(ExportedSentinelReplayCapture exported, rt::Bytes keyBytes) {
    if (static_cast<double>(keyBytes.size()) != replay_model::REPLAY_KEY_BYTES) throw rt::JsError("Sentinel replay key must be 32 bytes");
    if (!isExportedSentinelReplayCapture(rt::KvValue(json::value_from(exported)))) throw rt::JsError("Sentinel replay export is invalid");
    std::vector<rt::Bytes> decodedChunks;
    decodedChunks.reserve(exported.chunks.size());
    for (const std::string& chunk : exported.chunks) decodedChunks.push_back(utils::base64UrlDecode(chunk));
    rt::Bytes ciphertext = replay_model::concatBytes(decodedChunks);
    if (static_cast<double>(ciphertext.size()) != exported.manifest.ciphertext_bytes) {
      zero(ciphertext);
      for (rt::Bytes& chunk : decodedChunks) zero(chunk);
      throw rt::JsError("Sentinel replay ciphertext length does not match its manifest");
    }
    const rt::Bytes iv = utils::base64UrlDecode(exported.manifest.iv);
    rt::Bytes compressed;
    rt::Reason failure;
    try {
      rt::crypto::Algorithm algorithm{
        .name = "AES-GCM", .iv = iv, .additionalData = replay_envelope::encryptionAdditionalData(exported.manifest.fingerprint)};
      auto importing = replay_envelope::importAesKey(keyBytes);
      const rt::crypto::CryptoKey key = co_await std::move(importing);
      compressed = rt::crypto::subtle::decrypt(algorithm, key, ciphertext);
      co_await rt::microtask();  // `await crypto.subtle.decrypt(...)`
    } catch (...) {
      failure = std::current_exception();
    }
    zero(ciphertext);
    for (rt::Bytes& chunk : decodedChunks) zero(chunk);
    if (failure) std::rethrow_exception(failure);
    rt::Bytes decodedPlaintext;
    try {
      auto inflating = replay_envelope::gunzip(compressed);
      decodedPlaintext = co_await std::move(inflating);
    } catch (...) {
      failure = std::current_exception();
    }
    zero(compressed);
    if (failure) std::rethrow_exception(failure);
    std::optional<SentinelReplayPlaintext> plaintext;
    try {
      plaintext = replay_envelope::decodePlaintext(decodedPlaintext);
    } catch (...) {
      failure = std::current_exception();
    }
    zero(decodedPlaintext);
    if (failure) std::rethrow_exception(failure);
    const replay_model::AcceptedSentinelReplayInput accepted{
      .endpoint = plaintext->endpoint,
      .method = plaintext->method,
      .body = plaintext->body,
      .content_type = plaintext->content_type,
      .compatibility_headers = plaintext->compatibility_headers,
      .request_id = plaintext->request_id,
      .git_sha = plaintext->git_sha,
      .deno_revision = plaintext->deno_revision,
    };
    try {
      auto fingerprinting =
        replay_envelope::hmacHex(keyBytes, "fingerprint", replay_envelope::fingerprintParts(accepted, plaintext->failure_signature, "fingerprint", plaintext->upstream));
      const std::string fingerprint = co_await std::move(fingerprinting);
      auto grouping = replay_envelope::hmacHex(keyBytes, "case-group", replay_envelope::fingerprintParts(accepted, plaintext->failure_signature, "case-group"));
      const std::string caseGroupDigest = co_await std::move(grouping);
      if (fingerprint != exported.manifest.fingerprint || caseGroupDigest != exported.manifest.case_group_digest ||
          plaintext->captured_at_ms != exported.manifest.captured_at_ms ||
          replay_observation::sentinelFailureSignature(plaintext->client_observation) != plaintext->failure_signature) {
        throw rt::JsError("Sentinel replay manifest integrity check failed");
      }
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      if (plaintext->body) zero(*plaintext->body);
      std::rethrow_exception(failure);
    }
    co_return std::move(*plaintext);
  }

  SentinelReplayManifest toSentinelReplayManifest(const rt::KvValue& value) {
    const rt::KvValue& storedBytes = value["stored_bytes"];
    const rt::KvValue& requestId = value["request_id"];
    return {
      .version = value["version"].asNumber(),
      .capture_id = value["capture_id"].asString(),
      .fingerprint = value["fingerprint"].asString(),
      .case_group_digest = value["case_group_digest"].asString(),
      .captured_at_ms = value["captured_at_ms"].asNumber(),
      .expires_at_ms = value["expires_at_ms"].asNumber(),
      .algorithm = value["algorithm"].asString(),
      .compression = value["compression"].asString(),
      .iv = value["iv"].asString(),
      .chunk_count = value["chunk_count"].asNumber(),
      .ciphertext_bytes = value["ciphertext_bytes"].asNumber(),
      .stored_bytes = storedBytes.isNumber() ? std::optional<double>(storedBytes.asNumber()) : std::nullopt,
      .request_id = requestId.isString() ? std::optional<std::string>(requestId.asString()) : std::nullopt,
    };
  }
}
