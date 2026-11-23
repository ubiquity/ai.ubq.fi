#pragma once
// Port of src/codex/supervisor-log.ts.
/**
 * Bounded, read-only tail reader for a local Codex rollout file.
 *
 * The app-server's projected turn history can lag an active session, while the
 * rollout JSONL it is derived from keeps growing. This module reads only the
 * last 256 KiB of one already-selected session's rollout, validates the path
 * against the configured Codex home, and normalizes only visible user/assistant
 * messages and safe tool progress. Reasoning, system, and developer payloads
 * are never returned, and no Codex file, database, or session is written.
 */
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::codex::supervisor_log {
  struct SupervisorLogEvent {
    double atMs = 0;
    std::string kind = {};  // "user" | "assistant" | "tool"
    std::string text = {};
    friend bool operator==(const SupervisorLogEvent&, const SupervisorLogEvent&) = default;
  };

  /**
   * Accepts only a rollout path inside the configured Codex home's session trees
   * whose file name carries the selected thread id. Paths never come from HTTP
   * input; this is the lexical check, and the reader re-checks after realpath.
   */
  // `rolloutPath: unknown`: nullptr is undefined.
  std::optional<std::string> resolveSupervisorRolloutPath(std::string_view codexHome, std::string_view threadId, const json::value* rolloutPath);
  std::optional<std::string> resolveSupervisorRolloutPath(std::string_view codexHome, std::string_view threadId, std::string_view rolloutPath);

  /**
   * Parses rollout JSONL text, keeping visible messages and safe tool progress
   * only. Raw text is never clipped here: the caller redacts first and applies
   * the final field caps, and the newest 200 events are the ones that survive.
   */
  std::vector<SupervisorLogEvent> parseSupervisorRolloutTail(std::string_view text);

  // The `input` argument of readSupervisorRolloutTail.
  struct ReadSupervisorRolloutTailInput {
    std::optional<std::string> codexHome = std::nullopt;
    std::string threadId = {};
    std::optional<json::value> rolloutPath = std::nullopt;  // unknown; std::nullopt is undefined
  };

  /**
   * Reads at most the last 256 KiB of the selected session's rollout and returns
   * its normalized events. A partial leading JSONL record is discarded. Any
   * validation, permission, or read failure returns an empty list so the brief
   * degrades to the projected history instead of failing.
   */
  rt::Task<std::vector<SupervisorLogEvent>> readSupervisorRolloutTail(ReadSupervisorRolloutTailInput input);
}
