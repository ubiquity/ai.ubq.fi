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

/** FNV-1a 64-bit digest, hex-encoded low 64 bits. */
export const fnv1a64 = (text: string): string => {
  let hash = 0xcbf29ce484222325n;
  for (const byte of new TextEncoder().encode(text)) {
    hash ^= BigInt(byte);
    hash = BigInt.asUintN(64, hash * 0x100000001b3n);
  }
  return hash.toString(16).padStart(16, "0");
};

/** Short stable fingerprint (first 12 hex chars) for summaries and messages. */
export const digestShort = (text: string): string => fnv1a64(text).slice(0, 12);
