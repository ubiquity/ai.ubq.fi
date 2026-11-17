#pragma once
/**
 * Single source of truth for capture-owned size limits and the per-host
 * retention budget. Serialization, encryption/compression bounds, export/decode
 * and the offline reader all import these constants, so one allowed capture
 * cannot be accepted by one layer and rejected by another.
 *
 * Scope of the budget: the encoded KV payload this feature owns (base64-expanded
 * ciphertext chunks plus metadata, status, dedupe and index row overhead) and
 * its durable accounting rows. It is NOT a bound on the shared SQLite database,
 * its WAL, reusable allocated pages, host text logs, the separate incident index
 * namespace, or any other namespace's data.
 *
 * SENTINEL_REPLAY_METADATA_RESERVE_BYTES is carved out of the budget for
 * capture-owned status and eviction/expiry tombstone rows, so payload admissions
 * may use at most `budget - reserve`; see `sentinelReplayMetadataReserve`.
 */
#include "uos/request.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <cstdint>
#include <limits>

namespace uos::sentinel::replay_limits {
  inline constexpr double SENTINEL_REPLAY_MAX_REQUEST_BYTES = static_cast<double>(request::MAX_ACCEPTED_JSON_BODY_BYTES);
  inline constexpr double SENTINEL_REPLAY_MAX_UPSTREAM_BYTES = static_cast<double>(upstream_capture::SENTINEL_UPSTREAM_MAX_BYTES);
  inline constexpr double SENTINEL_REPLAY_MAX_UPSTREAM_CHUNKS = static_cast<double>(upstream_capture::SENTINEL_UPSTREAM_MAX_CHUNKS);
  inline constexpr double SENTINEL_REPLAY_MAX_UPSTREAM_ATTEMPTS = static_cast<double>(upstream_capture::SENTINEL_UPSTREAM_MAX_ATTEMPTS);
  inline constexpr double SENTINEL_REPLAY_MAX_DOWNSTREAM_BYTES = 64 * 1'024;
  inline constexpr double SENTINEL_REPLAY_STORAGE_CHUNK_BYTES = 48 * 1'024;

  /** JSON-string encoding worst case: one input byte becomes a six-character `\uXXXX` escape. */
  inline constexpr double SENTINEL_REPLAY_JSON_ESCAPE_FACTOR = 6;

  // base64Chars is module-private in the TS. It lives in the header because the exported limits below are
  // constant expressions (other modules derive their own constants from them); rt::Math::ceil is not constexpr.
  namespace detail {
    // `Math.ceil(value)` as a constant expression: NaN and the infinities pass through, |value| >= 2^52 is
    // already integral, and a negative fraction above -1 gives -0.
    constexpr double ceil(double value) {
      if (!(value == value) || value == std::numeric_limits<double>::infinity() || value == -std::numeric_limits<double>::infinity()) return value;
      if (value >= 4503599627370496.0 || value <= -4503599627370496.0) return value;
      const double truncated = static_cast<double>(static_cast<std::int64_t>(value));
      if (truncated == value) return value;
      if (value > 0) return truncated + 1;
      return truncated == 0 ? -0.0 : truncated;
    }

    // `const base64Chars = (bytes: number): number => Math.ceil(bytes / 3) * 4;`
    constexpr double base64Chars(double bytes) { return ceil(bytes / 3) * 4; }
  }

  /**
   * Derived upper bound for the metadata JSON that carries the whole upstream
   * trace plus per-chunk timing, safe headers and the bounded downstream body.
   * base64(4 MiB) = 5,592,408; timing 4,096 x 16 = 65,536; per-attempt fields
   * 8 x 8 KiB = 65,536; base64(64 KiB) = 87,384; 64 KiB framing slack
   * => about 5.6 MiB, so the published 8 MiB bound is demonstrably sufficient.
   */
  inline constexpr double SENTINEL_REPLAY_METADATA_DERIVED_BYTES = detail::base64Chars(SENTINEL_REPLAY_MAX_UPSTREAM_BYTES) +
                                                                   SENTINEL_REPLAY_MAX_UPSTREAM_CHUNKS * 16 +
                                                                   SENTINEL_REPLAY_MAX_UPSTREAM_ATTEMPTS * 8 * 1'024 +
                                                                   detail::base64Chars(SENTINEL_REPLAY_MAX_DOWNSTREAM_BYTES) + 64 * 1'024;
  inline constexpr double SENTINEL_REPLAY_MAX_METADATA_BYTES = 8 * 1'024 * 1'024;
  inline constexpr double SENTINEL_REPLAY_MAX_PLAINTEXT_BYTES = SENTINEL_REPLAY_MAX_REQUEST_BYTES + SENTINEL_REPLAY_MAX_METADATA_BYTES + 4;
  inline constexpr double SENTINEL_REPLAY_MAX_CIPHERTEXT_BYTES = SENTINEL_REPLAY_MAX_PLAINTEXT_BYTES + 1'024 * 1'024 + 16;
  inline constexpr double SENTINEL_REPLAY_MAX_STORED_CHUNKS = detail::ceil(SENTINEL_REPLAY_MAX_CIPHERTEXT_BYTES / SENTINEL_REPLAY_STORAGE_CHUNK_BYTES);

  /**
   * Offline reader file bounds. The decoded request body is still bounded by
   * SENTINEL_REPLAY_MAX_REQUEST_BYTES; the envelope allows JSON escaping only.
   */
  inline constexpr double SENTINEL_REPLAY_REQUEST_FILE_MAX_BYTES = SENTINEL_REPLAY_JSON_ESCAPE_FACTOR * SENTINEL_REPLAY_MAX_REQUEST_BYTES + 1'024 * 1'024;
  inline constexpr double SENTINEL_REPLAY_UPSTREAM_FILE_MAX_BYTES = SENTINEL_REPLAY_MAX_METADATA_BYTES + 64 * 1'024;

  /** Fixed per-host budget for capture-owned retained data, and a hard record-count bound. */
  inline constexpr double SENTINEL_REPLAY_BUDGET_BYTES = 1'024 * 1'024 * 1'024;
  inline constexpr double SENTINEL_REPLAY_MAX_RECORDS = 50'000;
  /**
   * Fixed metadata reserve inside the 1 GiB budget: capture-owned status and
   * eviction-tombstone rows may never consume more than this, so payload
   * admissions always have `budget - reserve` available. See
   * `sentinelReplayMetadataReserve` for how a smaller injectable test budget
   * scales it while the production budget keeps exactly this value.
   */
  inline constexpr double SENTINEL_REPLAY_METADATA_RESERVE_BYTES = 64 * 1'024 * 1'024;
  /** Hard bound on capture-owned status + tombstone rows, pruned oldest-first. */
  inline constexpr double SENTINEL_REPLAY_MAX_STATUS_RECORDS = 50'000;

  /** Conservative fixed overhead per retained capture (manifest, status, dedupe and index rows). */
  inline constexpr double SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES = 8 * 1'024;
  /** Legacy captures predate stored_bytes; their metadata was bounded by the old 256 KiB cap. */
  inline constexpr double SENTINEL_REPLAY_LEGACY_METADATA_BYTES = 256 * 1'024;

  /** Charge the encoded KV payload: base64-expanded ciphertext plus metadata and row overhead. */
  double sentinelReplayStoredCharge(double ciphertextBytes, double metadataBytes);

  /** Conservative pre-encryption reservation for a known plaintext size. */
  double sentinelReplayReservationBytes(double plaintextBytes, double metadataBytes);
}
