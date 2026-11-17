#pragma once
// Supervisor session inventory and sampling, split out of src/codex_supervisor.ts.
#include "uos/codex/supervisor_config.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uos::codex::supervisor_inventory {
  using supervisor_config::SupervisorQuota;
  using supervisor_config::SupervisorSession;
  using supervisor_config::SupervisorSnapshot;
  using supervisor_config::SupervisorSource;
  using supervisor_config::SupervisorState;

  // The record shapes sessionFromParts takes (module-private types of the TS).
  struct ThreadMetadata {
    std::optional<std::string> title;
    std::optional<std::string> preview;
    std::optional<std::string> cwd;
    std::optional<std::string> model;
    std::optional<std::string> effort;
    std::optional<double> tokensUsed;
    std::optional<std::string> gitBranch;
    std::optional<double> updatedAtMs;
    std::optional<bool> archived;
    std::optional<std::string> threadSource;
    std::optional<std::string> parentThreadId;
    std::optional<std::string> agentNickname;
    friend bool operator==(const ThreadMetadata&, const ThreadMetadata&) = default;
  };

  struct ListedThread {
    std::string id;
    std::optional<std::string> name;
    std::optional<std::string> preview;
    std::optional<std::string> cwd;
    std::optional<std::string> model;
    std::optional<std::string> effort;
    std::optional<std::string> sourceKind;
    std::optional<std::string> parentThreadId;
    std::optional<double> updatedAtMs;
    std::optional<std::string> modelProvider;
    friend bool operator==(const ListedThread&, const ListedThread&) = default;
  };

  struct ThreadProbe {
    std::optional<std::string> runtimeStatus;
    std::vector<std::string> activeFlags;
    std::optional<std::string> turnStatus;
    std::optional<ListedThread> thread;
    bool sampled;
    friend bool operator==(const ThreadProbe&, const ThreadProbe&) = default;
  };

  // { runtimeStatus: string | null; turnStatus: string | null; loaded: boolean }
  struct ClassifySupervisorStateInput {
    std::optional<std::string> runtimeStatus;
    std::optional<std::string> turnStatus;
    bool loaded;
  };

  /**
   * The single state decision. Runtime status from `thread/read` is authoritative:
   * recency is never treated as activity, and a failed read is `unknown`, never `idle`.
   */
  SupervisorState classifySupervisorState(const ClassifySupervisorStateInput& input);

  /**
   * Projects the shared account quota from one `account/rateLimits/read` result.
   * One bucket per limit id; the same account-wide window is reported by every
   * machine signed in to the account, so callers must never sum buckets from
   * different sources.
   */
  // `result: unknown` (nullptr is undefined); std::nullopt is null.
  std::optional<SupervisorQuota> quotaFromRateLimits(const json::value& result);
  std::optional<SupervisorQuota> quotaFromRateLimits(const json::value* result);

  // The input object of sessionFromParts.
  struct SessionFromPartsInput {
    std::string id;
    SupervisorSource source;
    std::optional<ThreadMetadata> metadata;
    std::optional<ListedThread> listed;
    std::optional<ThreadProbe> probe;
    bool loaded;
    double sampledAtMs;
    std::optional<std::vector<std::string>> childIds;
    std::vector<std::string> unavailable;
  };

  /**
   * Projects one row from the verified live thread fields first. A local state
   * database only enriches what the live thread omitted (and always owns the
   * persisted branch and token usage); it never replaces live values.
   */
  // The JS object this returns lists its members in this order (JSON output of a
  // session must keep it): id, sourceId, machine, title, titleSource, cwd, branch, model,
  // effort, provider, sourceKind, parentThreadId, lastActivityAtMs, tokensUsed,
  // usageSource, state, activeFlags, loaded, waitingOnApproval, waitingOnUserInput,
  // childIds, lastSampledAtMs, sampled, unavailable.
  SupervisorSession sessionFromParts(const SessionFromPartsInput& input);

  /** Single-flight, short-lived cache so several UI polls share one sampling round. */
  // Every caller of one sampling round receives an equal snapshot (JS: the same object).
  rt::Task<SupervisorSnapshot> ensureSupervisorSnapshot();

  // `{ id: string | null; status: string | null; items: unknown }`; `items` std::nullopt is undefined.
  struct NewestTurn {
    std::optional<std::string> id;
    std::optional<std::string> status;
    std::optional<json::value> items;
    friend bool operator==(const NewestTurn&, const NewestTurn&) = default;
  };

  // export { newestTurnOf }: `result: unknown` (nullptr is undefined); std::nullopt is null.
  std::optional<NewestTurn> newestTurnOf(const json::value& result);
  std::optional<NewestTurn> newestTurnOf(const json::value* result);
}
