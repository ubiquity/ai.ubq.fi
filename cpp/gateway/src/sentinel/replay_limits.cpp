#include "uos/sentinel/replay_limits.hpp"

#include "uos/rt/number.hpp"

namespace uos::sentinel::replay_limits {
  double sentinelReplayStoredCharge(double ciphertextBytes, double metadataBytes) {
    return detail::base64Chars(rt::Math::max(0, ciphertextBytes)) + rt::Math::max(0, metadataBytes) + SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES;
  }

  double sentinelReplayReservationBytes(double plaintextBytes, double metadataBytes) {
    return sentinelReplayStoredCharge(rt::Math::min(SENTINEL_REPLAY_MAX_CIPHERTEXT_BYTES, rt::Math::max(0, plaintextBytes) + 64 * 1'024 + 16), metadataBytes);
  }
}
