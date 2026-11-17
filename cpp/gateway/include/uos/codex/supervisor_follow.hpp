#pragma once
// Supervisor follow polling, split out of src/codex_supervisor.ts.
#include "uos/codex/supervisor_config.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::codex::supervisor_follow {
  using supervisor_config::JsonRecord;
  using supervisor_config::SupervisorSource;

  struct SupervisorFollowEntry {
    std::string key;
    std::string kind;  // "message" | "command"
    std::string turnId;
    double index;
    std::string text;
    std::optional<std::string> command;
    std::optional<std::string> status;
    std::optional<double> exitCode;
    std::optional<double> atMs;
    friend bool operator==(const SupervisorFollowEntry&, const SupervisorFollowEntry&) = default;
  };

  /**
   * Extracts only what a super admin may see: assistant messages and command
   * output. Reasoning, system, developer, user, and tool-internal items are never
   * converted into panel entries.
   */
  // `items: unknown` (nullptr is undefined).
  std::vector<SupervisorFollowEntry> extractFollowEntries(std::string_view turnId, const json::value& items);
  std::vector<SupervisorFollowEntry> extractFollowEntries(std::string_view turnId, const json::value* items);

  // { turnId: string; index: number }
  struct FollowCursor {
    std::string turnId;
    double index;
    friend bool operator==(const FollowCursor&, const FollowCursor&) = default;
  };

  // std::nullopt is null.
  std::optional<FollowCursor> parseFollowCursor(std::string_view cursor);

  /**
   * Content revision for one follow entry. Command output, status and exit code
   * mutate in place at a stable item index, so the index alone cannot tell the
   * client that an already-rendered entry changed.
   */
  std::string followEntryRevision(const SupervisorFollowEntry& entry);

  /**
   * Chooses the entries one poll must emit: new indices plus any entry whose
   * content revision changed since the previous poll, including entries at or
   * below the cursor. `revisions` is per SSE connection, so unchanged polls emit
   * nothing and later output or completion reaches the already-rendered item.
   * Only entries that survive the emission filter and cap are committed, so a
   * dropped revision remains eligible for a later poll.
   */
  std::vector<SupervisorFollowEntry> selectFollowUpdates(const std::vector<SupervisorFollowEntry>& entries, double startIndex, rt::OrderedMap<std::string, std::string>& revisions);

  // export { followEntryPayload }: members key, kind, turnId, text, command, status, exitCode, atMs, revision.
  JsonRecord followEntryPayload(const SupervisorFollowEntry& entry);

  // { entries: SupervisorFollowEntry[]; cursor: string; turnId: string | null; turnStatus: string | null; enriched: boolean }
  struct FollowUpdate {
    std::vector<SupervisorFollowEntry> entries;
    std::string cursor;
    std::optional<std::string> turnId;
    std::optional<std::string> turnStatus;
    bool enriched;
  };

  // export { readFollowUpdate }. `revisions` is the caller's Map (shared, updated by the poll).
  rt::Task<FollowUpdate> readFollowUpdate(SupervisorSource source, std::string threadId, std::string cursor, rt::AbortSignal signal,
                                          std::shared_ptr<rt::OrderedMap<std::string, std::string>> revisions);
}
