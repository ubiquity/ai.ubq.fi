#pragma once
/**
 * Trusted recorded-upstream replay transport for sentinel regression tests.
 *
 * It converts an already-validated SentinelUpstreamTrace (see
 * src/sentinel_upstream_capture.ts) into a fetch transport that reproduces the
 * recorded upstream attempt sequence: the exact provider route per attempt in
 * recorded order, the exact recorded chunk boundaries, and the recorded
 * terminal semantics (eof, read_error, fetch_error, cancelled). It never
 * invokes the global fetch and never falls through to a real service: a
 * request whose URL is not the exact route of the next recorded attempt is a
 * permanent failure and rejects with a TypeError.
 *
 * Every failure is a fixed static message: no URL, body, token or header value
 * is ever included in an error, snapshot or assertion text. The helper performs
 * no sanitization and no causal-success attestation; replayed raw bytes remain
 * restricted capture evidence.
 */
#include "uos/rt/fetch.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <cstdint>
#include <functional>

namespace uos::tests::helpers::sentinel_recorded_upstream {
  struct RecordedUpstreamReplaySnapshot {
    std::int64_t attemptsDispatched{};
    std::int64_t attemptsCompleted{};
    std::int64_t chunksConsumed{};
    std::int64_t chunksTotal{};
    bool failed{};

    friend bool operator==(const RecordedUpstreamReplaySnapshot&, const RecordedUpstreamReplaySnapshot&) = default;
  };

  struct RecordedUpstreamReplay {
    rt::Fetch fetch;
    std::function<void()> assertComplete;
    std::function<RecordedUpstreamReplaySnapshot()> snapshot;
  };

  /**
   * Build the replayed upstream transport for one parsed trace. The trace is
   * cloned and re-validated with the real exported parser; the caller's input
   * objects and buffers are never mutated. `routes` must map the recorded
   * provider enums to distinct exact HTTPS endpoint strings (there must be no
   * ambiguity where one endpoint could belong to two providers).
   *
   * The returned transport is unavailable (creation throws) for a trace with no
   * attempts, truncation flags, or any pending attempt, and for attempts whose
   * evidence cannot be reproduced exactly (headerless or unsupported MIME,
   * informational final status, inconsistent bodyless terminal).
   *
   * `routes` is the Readonly<Record<SentinelUpstreamProvider, string>> object.
   */
  RecordedUpstreamReplay createRecordedUpstreamReplay(const sentinel::upstream_capture::SentinelUpstreamTrace& trace, const json::object& routes);
}
