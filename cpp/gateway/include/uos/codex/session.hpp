#pragma once
// Port of src/codex/session.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::codex::session {
  /**
   * Session activity is an observation only. It must not be used as evidence
   * that a person is asleep, and it must not change request priority by itself.
   */
  inline constexpr double CODEX_SESSION_CONTINUATION_THRESHOLD_MS = 60 * 60 * 1000;
  inline constexpr double CODEX_SESSION_ACTIVITY_TTL_MS = 24 * 60 * 60 * 1000;
  inline const rt::KvKey CODEX_SESSION_ACTIVITY_PREFIX{"uos_ai", "codex_session_activity"};

  // "unknown" | "new" | "continuation"
  using CodexSessionObservationState = std::string;

  struct CodexSessionObservation {
    bool metadata_present;
    bool session_id_present;
    std::optional<std::string> session_id_hash;
    CodexSessionObservationState state;
    std::optional<double> continuation_age_ms;
    bool continuation_only_candidate;
    friend bool operator==(const CodexSessionObservation&, const CodexSessionObservation&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexSessionObservation& value);
  CodexSessionObservation tag_invoke(const json::value_to_tag<CodexSessionObservation>&, const json::value& value);

  /**
   * Observe one Responses request. Missing API-key identity, missing session
   * metadata, unavailable KV, and malformed prior state all fail closed to
   * `unknown`; the request itself is never rejected for telemetry reasons.
   */
  // `keyId: string | null | undefined` (std::nullopt for both); `clientMetadata: unknown`
  // (std::nullopt is undefined); `nowMs = Date.now()`: the two-argument overload.
  [[nodiscard]] rt::Task<CodexSessionObservation> observeCodexSession(std::optional<std::string> keyId, std::optional<json::value> clientMetadata);
  [[nodiscard]] rt::Task<CodexSessionObservation> observeCodexSession(std::optional<std::string> keyId, std::optional<json::value> clientMetadata, double nowMs);
}
