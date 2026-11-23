#pragma once
// Port of src/codex/supervisor-brief.ts.
/**
 * On-demand "catch me up" brief for one supervised session.
 *
 * This is the only supervised path that calls a model. It runs solely when a
 * super admin clicks the per-session button: the normal inventory poll never
 * reaches it. It reads a bounded, redacted transcript through the same
 * read-only app-server allowlist the rest of the panel uses, treats the
 * recorded log as untrusted data, and asks the existing Cerebras client
 * for one JSON object with no tools. It never writes to, resumes, or steers the
 * monitored session.
 */
#include "uos/codex/supervisor_config.hpp"
#include "uos/codex/supervisor_log.hpp"
#include "uos/codex/supervisor_secret.hpp"
#include "uos/codex/supervisor_transport.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::codex::supervisor_brief {
  /* ---------------------------------------------------------------- redaction */

  /** Credential-shaped text is removed before any transcript reaches the model. */
  // `export const redactBriefText = redactSupervisorSecrets` (a renamed re-export).
  inline supervisor_secret::RedactedText redactBriefText(std::string_view text) { return supervisor_secret::redactSupervisorSecrets(text); }

  /* -------------------------------------------------------------- transcript */

  // Module-private `BriefTranscriptItem`, exposed through SupervisorBriefContext.
  struct BriefTranscriptItem {
    std::string type;
    std::string text;
    friend bool operator==(const BriefTranscriptItem&, const BriefTranscriptItem&) = default;
  };

  // Module-private `BriefTranscriptTurn`, exposed through SupervisorBriefContext.
  struct BriefTranscriptTurn {
    std::string id;
    std::optional<std::string> status;
    std::optional<double> startedAtMs;
    std::optional<double> completedAtMs;
    std::vector<BriefTranscriptItem> items;
    std::int64_t droppedItems;
    /** True for the synthetic turn built from the fresh local rollout tail. */
    std::optional<bool> freshTail{};
    friend bool operator==(const BriefTranscriptTurn&, const BriefTranscriptTurn&) = default;
  };

  // Module-private `BriefThreadMeta`, the first part of SupervisorBriefContext.
  struct BriefThreadMeta {
    std::optional<std::string> title;
    std::optional<std::string> cwd;
    std::optional<std::string> model;
    std::optional<std::string> provider;
    std::optional<std::string> effort;
    std::optional<std::string> state;
    std::vector<std::string> activeFlags;
    std::optional<double> updatedAtMs;
    std::optional<std::string> rolloutPath;
    friend bool operator==(const BriefThreadMeta&, const BriefThreadMeta&) = default;
  };

  // `BriefThreadMeta & { ... }`. A derived aggregate: value-initialize it (`{}`) and assign the members.
  struct SupervisorBriefContext : BriefThreadMeta {
    std::string sourceId;
    std::string machine;
    std::string threadId;
    std::vector<BriefTranscriptTurn> turns;
    bool transcriptAvailable;
    bool truncated;
    std::int64_t redactions;
    std::int64_t contextBytes;
    /** Normalized events taken from the local rollout tail (0 for remote sources). */
    std::int64_t rolloutTailEvents;
    /** Positive when the projected history is older than the fresh runtime metadata. */
    std::optional<double> projectedHistoryBehindMs;
    friend bool operator==(const SupervisorBriefContext&, const SupervisorBriefContext&) = default;
  };

  /* --------------------------------------------------------------- assembly */

  // Module-private `BriefAssembly`, the result of assembleBriefContext.
  struct BriefAssembly {
    std::vector<BriefTranscriptTurn> turns;
    bool transcriptAvailable;
    bool truncated;
    std::int64_t redactions;
    std::int64_t contextBytes;
    friend bool operator==(const BriefAssembly&, const BriefAssembly&) = default;
  };

  /**
   * Spends the 32 KiB budget on the freshest turns first and the opening turn
   * last, so an opening turn's older records can never displace recent progress.
   * Output order stays chronological; item-limit drops mark truncation.
   */
  BriefAssembly assembleBriefContext(const std::vector<BriefTranscriptTurn>& inputTurns);

  // The `input` of appendRolloutTailTurn: `{ threadId; state: string | null; events }`.
  struct AppendRolloutTailTurnInput {
    std::string threadId;
    std::optional<std::string> state;
    std::vector<supervisor_log::SupervisorLogEvent> events;
  };

  /**
   * Appends the fresh local rollout events as one tail turn. Events already
   * represented by the projected history are dropped, every text is redacted
   * before it is bounded, and the newest events win the per-turn item cap.
   */
  std::vector<BriefTranscriptTurn> appendRolloutTailTurn(const std::vector<BriefTranscriptTurn>& turns, const AppendRolloutTailTurnInput& input);

  /** Collects one bounded, redacted transcript through the read-only allowlist. */
  // A plain function returning the coroutine's Task: the aggregates are copied into the coroutine
  // here, so a caller may write the call inside a co_await expression.
  rt::Task<SupervisorBriefContext> collectBriefTranscript(const supervisor_transport::SupervisorConnection& connection, const supervisor_config::SupervisorSource& source,
                                                          std::string threadId, rt::AbortSignal signal);

  /** Opens the source's read-only socket, collects the transcript, and always closes it. */
  rt::Task<SupervisorBriefContext> collectSupervisorBriefContext(const supervisor_config::SupervisorSource& source, std::string threadId, rt::AbortSignal signal);

  /* ------------------------------------------------------------------ prompt */

  std::string buildSupervisorBriefPrompt(const SupervisorBriefContext& context);

  /** The exact no-tools JSON-mode request the summarizer sends to the DeepSeek client. */
  json::object buildSupervisorBriefRequestBody(const SupervisorBriefContext& context);

  /* ------------------------------------------------------------------ output */

  // `{ about: string; status: string }`
  struct SupervisorBriefOutput {
    std::string about;
    std::string status;
    friend bool operator==(const SupervisorBriefOutput&, const SupervisorBriefOutput&) = default;
  };

  /** Validates the model's `{about,status}` JSON object; null means unusable output. */
  // `content: unknown`: nullptr is undefined.
  std::optional<SupervisorBriefOutput> parseSupervisorBriefOutput(const json::value& content);
  std::optional<SupervisorBriefOutput> parseSupervisorBriefOutput(const json::value* content);

  /* ------------------------------------------------------------------- route */

  /**
   * POST /admin/codex/supervisor/brief — one bounded summarizer call per click.
   * The route is registered super-admin only, validates the selected session
   * against the trusted inventory, and never runs on the inventory poll.
   */
  rt::Task<rt::Response> handleAdminCodexSupervisorBrief(rt::Request req);
}
