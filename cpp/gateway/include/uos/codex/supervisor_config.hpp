#pragma once
// Supervisor configuration, shared types and small helpers, split out of src/codex_supervisor.ts.
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/regex.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uos::codex::supervisor_config {
  inline constexpr double SOURCE_CONCURRENCY = 2;
  inline const rt::Regex SOURCE_ID_PATTERN{"^[a-z0-9][a-z0-9_-]{0,31}$"};
  inline const rt::Regex THREAD_ID_PATTERN{"^[A-Za-z0-9._:-]{1,200}$"};

  inline constexpr double INVENTORY_LIMIT = 60;
  inline constexpr double LIST_PAGE_LIMIT = 50;
  inline constexpr double MAX_LIST_PAGES = 2;
  inline constexpr double LIVE_PROBE_LIMIT = 24;
  inline constexpr double EXTRA_LOADED_LIMIT = 8;
  inline constexpr double PROBE_CONCURRENCY = 4;
  inline constexpr double OVERALL_DEADLINE_MS = 9'000;

  inline constexpr double SNAPSHOT_TTL_MS = 4'000;
  inline constexpr double RECENT_METADATA_LIMIT = 60;
  inline const rt::Regex STATE_DATABASE_PATTERN{"^state_(\\d+)\\.sqlite$"};
  inline constexpr std::string_view SQLITE_MODULE_SPECIFIER = "bun:sqlite";

  inline constexpr double FOLLOW_POLL_MS = 2'000;
  inline constexpr double FOLLOW_HEARTBEAT_EVERY = 5;
  inline constexpr double FOLLOW_MAX_DURATION_MS = 600'000;
  inline constexpr double FOLLOW_MAX_ENTRIES = 120;
  inline constexpr double FOLLOW_MAX_TEXT_CHARS = 4'000;
  inline constexpr double FOLLOW_MAX_COMMAND_CHARS = 400;

  /** Every non-interactive source kind, requested explicitly so subagents are never hidden. */
  inline const std::vector<std::string> LIST_SOURCE_KINDS{
    "cli",
    "vscode",
    "exec",
    "appServer",
    "subAgent",
    "subAgentReview",
    "subAgentCompact",
    "subAgentThreadSpawn",
    "subAgentOther",
    "unknown",
  };

  inline constexpr std::string_view METADATA_COLUMNS =
    "id, title, name, preview, cwd, model, reasoning_effort, tokens_used, git_branch, updated_at, updated_at_ms, archived, thread_source, source";

  inline const std::unordered_set<std::string> TERMINAL_TURN_STATES{"completed", "interrupted", "failed", "declined", "cancelled", "canceled", "errored", "aborted"};

  // "active" | "idle" | "stale" | "unknown" | "system_error"
  enum class SupervisorState { active, idle, stale, unknown, system_error };
  std::string_view toString(SupervisorState state);
  std::optional<SupervisorState> parseSupervisorState(std::string_view text);

  struct SupervisorSource {
    std::string id{};
    std::string name{};
    std::string socketPath{};
    std::optional<std::string> codexHome{};
    friend bool operator==(const SupervisorSource&, const SupervisorSource&) = default;
  };

  struct SupervisorConfig {
    std::vector<SupervisorSource> sources{};
    std::string configPath{};
    bool configLoaded = false;
    std::vector<std::string> notes{};
  };

  struct SupervisorQuotaBucket {
    std::string limitId{};
    std::optional<std::string> limitName{};
    std::optional<double> usedPercent{};
    std::optional<double> windowDurationMins{};
    std::optional<double> resetsAtMs{};
    std::optional<std::string> planType{};
    std::optional<std::string> reachedType{};
    friend bool operator==(const SupervisorQuotaBucket&, const SupervisorQuotaBucket&) = default;
  };

  struct SupervisorQuota {
    std::string accountScope = "shared_codex_account";  // "shared_codex_account"
    std::vector<SupervisorQuotaBucket> buckets{};
    std::optional<double> resetCreditsAvailable{};
    friend bool operator==(const SupervisorQuota&, const SupervisorQuota&) = default;
  };

  struct SupervisorSession {
    std::string id{};
    std::string sourceId{};
    std::string machine{};
    std::optional<std::string> title{};
    std::optional<std::string> titleSource{};  // "thread" | "metadata" | null
    std::optional<std::string> cwd{};
    std::optional<std::string> branch{};
    std::optional<std::string> model{};
    std::optional<std::string> effort{};
    std::optional<std::string> provider{};
    SupervisorState state = SupervisorState::unknown;
    std::vector<std::string> activeFlags{};
    std::optional<bool> loaded{};
    bool waitingOnApproval = false;
    bool waitingOnUserInput = false;
    std::optional<std::string> sourceKind{};
    std::optional<std::string> parentThreadId{};
    std::optional<std::vector<std::string>> childIds{};
    std::optional<double> lastActivityAtMs{};
    double lastSampledAtMs = 0;
    std::optional<double> tokensUsed{};
    std::optional<std::string> usageSource{};  // "state_db" | null
    bool sampled = false;
    std::vector<std::string> unavailable{};
    friend bool operator==(const SupervisorSession&, const SupervisorSession&) = default;
  };

  struct SupervisorSourceView {
    struct Metadata {
      bool available = false;
      std::optional<std::string> database{};
      std::optional<std::string> reason{};
      friend bool operator==(const Metadata&, const Metadata&) = default;
    };
    struct Inventory {
      std::int64_t listed = 0;
      std::int64_t sampled = 0;
      std::int64_t returned = 0;
      bool truncated = false;
      friend bool operator==(const Inventory&, const Inventory&) = default;
    };
    std::string id{};
    std::string name{};
    std::string kind{};  // "local" | "remote"
    std::optional<std::string> hostname{};
    std::string state{};  // "ok" | "unavailable"
    std::optional<std::string> reason{};
    Metadata metadata{};
    Inventory inventory{};
    std::string lineage{};  // "available" | "unavailable"
    std::optional<SupervisorQuota> quota{};
    std::optional<std::string> quotaReason{};
    double durationMs = 0;
    double sampledAtMs = 0;
    friend bool operator==(const SupervisorSourceView&, const SupervisorSourceView&) = default;
  };

  struct SupervisorSnapshot {
    struct Counts {
      std::int64_t total = 0;
      std::int64_t active = 0;
      std::int64_t waiting = 0;
      std::int64_t idle = 0;
      std::int64_t stale = 0;
      std::int64_t unknown = 0;
      std::int64_t systemError = 0;
      friend bool operator==(const Counts&, const Counts&) = default;
    };
    struct Coverage {
      std::int64_t listed = 0;
      std::int64_t sampled = 0;
      bool truncated = false;
      std::vector<std::string> notes{};
      friend bool operator==(const Coverage&, const Coverage&) = default;
    };
    double sampledAtMs = 0;
    std::string view = "codex_app_server_read_only";  // "codex_app_server_read_only"
    std::vector<SupervisorSourceView> sources{};
    std::vector<SupervisorSession> sessions{};
    Counts counts{};
    Coverage coverage{};
    friend bool operator==(const SupervisorSnapshot&, const SupervisorSnapshot&) = default;
  };

  using JsonRecord = json::object;

  // `unknown` arguments: a value, or a pointer to a member that may be absent (nullptr is undefined).
  bool isRecord(const json::value& value);
  bool isRecord(const json::value* value);

  bool isUnknownArray(const json::value& value);
  bool isUnknownArray(const json::value* value);

  std::optional<std::string> asString(const json::value& value);
  std::optional<std::string> asString(const json::value* value);

  std::optional<double> asFiniteNumber(const json::value& value);
  std::optional<double> asFiniteNumber(const json::value* value);

  std::optional<bool> asBoolean(const json::value& value);
  std::optional<bool> asBoolean(const json::value* value);

  std::string joinPath(std::string_view base, std::string_view leaf);

  /** Accepts either Unix seconds or Unix milliseconds and returns milliseconds. */
  std::optional<double> normalizeEpochMs(const json::value& value);
  std::optional<double> normalizeEpochMs(const json::value* value);

  std::optional<std::string> textOrNull(const json::value& value);
  std::optional<std::string> textOrNull(const json::value* value);

  std::vector<std::string> uniqueStrings(const std::vector<std::string>& values);

  // Lengths are UTF-16 code units, as in JavaScript.
  std::string boundedText(std::string_view value, double limit);

  double nowMs();

  std::optional<std::string> readHostname();

  // A promise (not an async function): resolves after `ms`, or at once when `signal`
  // aborts. A signal that is already aborted never calls its listener, so the wait
  // then lasts the full `ms`, as in JavaScript.
  rt::Promise<void> waitForPoll(double ms, rt::AbortSignal signal);

  namespace detail {
    template <class T> struct SettledValue;
    template <class R> struct SettledValue<rt::Task<R>> {
      using type = R;
    };
    template <class R> struct SettledValue<rt::Promise<R>> {
      using type = R;
    };
    template <class TItem, class Run> using MapResult = typename SettledValue<std::invoke_result_t<Run&, TItem&>>::type;
  }

  // `run(item)` returns rt::Task<TResult> or rt::Promise<TResult>. Workers start in
  // order and each takes the next item when its previous one settles; results keep
  // the item order. A `limit` of NaN starts no worker (the result is then empty).
  template <class TItem, class Run>
  rt::Task<std::vector<detail::MapResult<TItem, Run>>> mapWithConcurrency(std::vector<TItem> items, double limit, Run run) {
    using TResult = detail::MapResult<TItem, Run>;
    struct State {
      std::vector<TItem> items;
      Run run;
      std::vector<std::optional<TResult>> results;
      std::size_t next = 0;
    };
    const std::size_t itemCount = items.size();
    auto state = std::make_shared<State>(State{std::move(items), std::move(run), std::vector<std::optional<TResult>>(itemCount), 0});
    const double workerCount = rt::Math::max(1, rt::Math::min(limit, static_cast<double>(itemCount)));
    std::vector<rt::Promise<void>> workers;
    for (double index = 0; index < workerCount; index += 1) {
      workers.push_back(rt::Promise<void>(rt::async([state]() -> rt::Task<void> {
        while (state->next < state->items.size()) {
          const std::size_t current = state->next;
          state->next += 1;
          rt::Promise<TResult> pending = state->run(state->items[current]);
          TResult value = co_await pending.wait();
          state->results[current] = std::move(value);
        }
      })));
    }
    co_await rt::all(std::move(workers)).wait();
    std::vector<TResult> results;
    for (std::optional<TResult>& slot : state->results)
      if (slot) results.push_back(std::move(*slot));
    co_return results;
  }

  // { sources: SupervisorSource[]; notes: string[] }
  struct ParsedSupervisorConfig {
    std::vector<SupervisorSource> sources{};
    std::vector<std::string> notes{};
  };

  /** Validates the optional configuration file. Pure, so tests can drive it directly. */
  ParsedSupervisorConfig parseSupervisorConfig(const json::value& raw);

  // `(name: string) => string | undefined`
  using EnvironmentReader = std::function<std::optional<std::string>(std::string_view name)>;

  /** The fallback source: this machine's own Codex home, with no configuration file at all. */
  std::optional<SupervisorSource> defaultSupervisorSource(const EnvironmentReader& env, std::vector<std::string>& notes);

  // { cwd?: string; env?: (name: string) => string | undefined }; an empty `env` is undefined.
  struct ResolveSupervisorConfigOptions {
    std::optional<std::string> cwd{};
    EnvironmentReader env{};
  };

  // Bind the options to a named local before `co_await resolveSupervisorConfig(options)`
  // (GCC 12 destroys aggregate temporaries inside a co_await expression twice).
  rt::Task<SupervisorConfig> resolveSupervisorConfig(ResolveSupervisorConfigOptions options);
  rt::Task<SupervisorConfig> resolveSupervisorConfig();
}
