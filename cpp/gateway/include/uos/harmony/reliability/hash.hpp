#pragma once
/**
 * Deterministic content digests for the reliability layer (plan m05).
 *
 * All detection rules (duplicate/loop identity, state contracts, compaction
 * proofs) compare content through stable digests so evidence is reproducible
 * across runs, machines and formats.  FNV-1a (64-bit) is used on purpose:
 * it is synchronous, allocation-light, dependency-free and deterministic for
 * arbitrary UTF-8 input, which keeps the modules pure and testable offline.
 * It is NOT a security primitive — it only fingerprints content.
 */
#include <string>
#include <string_view>

namespace uos::harmony::reliability::hash {
  /** FNV-1a 64-bit digest, hex-encoded low 64 bits. */
  std::string fnv1a64(std::string_view text);

  /** Short stable fingerprint (first 12 hex chars) for summaries and messages. */
  std::string digestShort(std::string_view text);
}
