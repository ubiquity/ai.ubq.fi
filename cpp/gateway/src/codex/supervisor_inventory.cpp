// Supervisor session inventory and sampling, split out of src/codex_supervisor.ts.

/**
 * Read-only Codex supervisor inventory.
 *
 * This module answers one question for a super admin: which Codex sessions are
 * running where, right now, and what is each one waiting on. It never steers a
 * session: it connects to existing Codex app-server control sockets, calls only
 * allowlisted read methods, and reports `unknown` whenever a read fails instead
 * of guessing.
 *
 * Optional configuration lives in `.data/codex-supervisor.json` relative to the
 * runtime working directory:
 *
 *   { "sources": [{ "id": "local", "name": "This Mac", "socketPath": "/…/app-server-control.sock", "codexHome": "/…/.codex" }] }
 *
 * Live thread fields (title, model, effort, cwd, lineage, runtime state) come
 * from each machine's own app-server for local and remote sources alike. Only a
 * source that names an explicit `codexHome` may additionally read that machine's
 * read-only state database for persisted branch and token usage. Sources are
 * trusted local configuration: HTTP input can select a configured source id, but
 * can never add a path.
 */
#include "uos/codex/supervisor_inventory.hpp"
#include "uos/rt/memory.hpp"

#include "uos/codex/supervisor_transport.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/string.hpp"

#include <exception>
#include <memory>
#include <utility>
#include <variant>

namespace uos::codex::supervisor_inventory {
  using supervisor_config::asBoolean;
  using supervisor_config::asFiniteNumber;
  using supervisor_config::asString;
  using supervisor_config::EXTRA_LOADED_LIMIT;
  using supervisor_config::INVENTORY_LIMIT;
  using supervisor_config::isRecord;
  using supervisor_config::isUnknownArray;
  using supervisor_config::joinPath;
  using supervisor_config::JsonRecord;
  using supervisor_config::LIST_PAGE_LIMIT;
  using supervisor_config::LIST_SOURCE_KINDS;
  using supervisor_config::LIVE_PROBE_LIMIT;
  using supervisor_config::mapWithConcurrency;
  using supervisor_config::MAX_LIST_PAGES;
  using supervisor_config::METADATA_COLUMNS;
  using supervisor_config::normalizeEpochMs;
  using supervisor_config::nowMs;
  using supervisor_config::OVERALL_DEADLINE_MS;
  using supervisor_config::PROBE_CONCURRENCY;
  using supervisor_config::readHostname;
  using supervisor_config::RECENT_METADATA_LIMIT;
  using supervisor_config::resolveSupervisorConfig;
  using supervisor_config::SNAPSHOT_TTL_MS;
  using supervisor_config::SOURCE_CONCURRENCY;
  using supervisor_config::STATE_DATABASE_PATTERN;
  using supervisor_config::SupervisorConfig;
  using supervisor_config::SupervisorQuotaBucket;
  using supervisor_config::SupervisorSourceView;
  using supervisor_config::TERMINAL_TURN_STATES;
  using supervisor_config::textOrNull;
  using supervisor_config::THREAD_ID_PATTERN;
  using supervisor_config::uniqueStrings;
  using supervisor_transport::openSupervisorConnection;
  using supervisor_transport::SupervisorCallResult;
  using supervisor_transport::SupervisorConnection;

  namespace {
    // `array.join(separator)` of strings.

    // The `unknown` a call resolves with, as the pointer the `unknown` helpers take.
    const json::value* valueOf(const SupervisorCallResult& result) { return result ? &*result : nullptr; }

    // A bun:sqlite row as the `Record<string, unknown>` the TS reads it as. A bigint or
    // Uint8Array column (neither string, number nor boolean, so every reader here sees
    // null) becomes an empty object.
    json::object recordOfRow(const rt::sqlite::Row& row) {
      json::object record;
      for (const auto& [column, value] : row) {
        if (std::holds_alternative<std::nullptr_t>(value)) rt::objectDefine(record, column, json::value(nullptr));
        else if (const double* number = std::get_if<double>(&value)) rt::objectDefine(record, column, rt::makeNumber(*number));
        else if (const std::string* text = std::get_if<std::string>(&value)) rt::objectDefine(record, column, json::value(*text));
        else rt::objectDefine(record, column, json::value(json::object()));
      }
      return record;
    }

    rt::Task<std::optional<rt::sqlite::Database>> openReadOnlySqlite(std::string path) {
      // `await import(SQLITE_MODULE_SPECIFIER)`: bun:sqlite is rt::sqlite, linked in, so the
      // module and its `Database` constructor are always present.
      co_await rt::microtask();
      try {
        rt::sqlite::DatabaseOptions options;
        options.readonly = true;
        options.create = false;
        co_return rt::sqlite::Database(path, options);
      } catch (...) {
        co_return std::nullopt;
      }
    }

    rt::Task<std::optional<std::string>> findStateDatabase(std::string codexHome) {
      struct Best {
        std::string path;
        double version;
      };
      std::optional<Best> best;
      try {
        const std::vector<rt::fs::Dirent> entries = rt::fs::promises::readdir(codexHome, rt::fs::withFileTypes);
        co_await rt::microtask();
        for (const rt::fs::Dirent& entry : entries) {
          if (!entry.isFile()) continue;
          const std::optional<rt::RegexMatch> match = STATE_DATABASE_PATTERN.exec(entry.name);
          if (!match) continue;
          const double version = rt::stringToNumber((*match)[1].value_or(""));
          if (!rt::Number::isSafeInteger(version)) continue;
          if (!best || version > best->version) best = Best{joinPath(codexHome, entry.name), version};
        }
      } catch (...) {
        co_return std::nullopt;
      }
      co_return best ? std::optional<std::string>(best->path) : std::nullopt;
    }

    struct Lineage {
      std::optional<std::string> parentThreadId;
      std::optional<std::string> agentNickname;
    };

    Lineage lineageFromSourceJson(const json::value* value) {
      const std::optional<std::string> raw = textOrNull(value);
      if (!raw) return {std::nullopt, std::nullopt};
      try {
        const json::value parsed = rt::jsonParse(*raw);
        if (!isRecord(parsed)) return {std::nullopt, std::nullopt};
        const json::value* nested = isRecord(rt::get(parsed, "subagent")) ? rt::get(parsed, "subagent") : nullptr;
        std::optional<std::string> parent = textOrNull(rt::get(parsed, "parent_thread_id"));
        if (!parent) parent = nested ? textOrNull(rt::get(*nested, "parent_thread_id")) : std::nullopt;
        std::optional<std::string> nickname = textOrNull(rt::get(parsed, "agent_nickname"));
        if (!nickname) nickname = nested ? textOrNull(rt::get(*nested, "agent_nickname")) : std::nullopt;
        return {parent, nickname};
      } catch (...) {
        return {std::nullopt, std::nullopt};
      }
    }

    template <class T> std::optional<T> orElse(std::optional<T> first, const std::optional<T>& second) { return first ? first : second; }

    ThreadMetadata metadataFromRow(const JsonRecord& row) {
      const Lineage lineage = lineageFromSourceJson(rt::get(row, "source"));
      return ThreadMetadata{
        .title = orElse(textOrNull(rt::get(row, "name")), textOrNull(rt::get(row, "title"))),
        .preview = textOrNull(rt::get(row, "preview")),
        .cwd = textOrNull(rt::get(row, "cwd")),
        .model = textOrNull(rt::get(row, "model")),
        .effort = textOrNull(rt::get(row, "reasoning_effort")),
        .tokensUsed = asFiniteNumber(rt::get(row, "tokens_used")),
        .gitBranch = textOrNull(rt::get(row, "git_branch")),
        .updatedAtMs = orElse(normalizeEpochMs(rt::get(row, "updated_at_ms")), normalizeEpochMs(rt::get(row, "updated_at"))),
        .archived = asBoolean(rt::get(row, "archived")),
        .threadSource = textOrNull(rt::get(row, "thread_source")),
        .parentThreadId = lineage.parentThreadId,
        .agentNickname = lineage.agentNickname,
      };
    }

    // { database: string | null; rows: Map<string, ThreadMetadata>; reason: string | null }
    struct ThreadMetadataRead {
      std::optional<std::string> database;
      rt::OrderedMap<std::string, ThreadMetadata> rows;
      std::optional<std::string> reason;
    };

    /**
     * Reads persisted thread metadata with a read-only connection.
     *
     * `ids === null` returns the most recently updated threads, which is the honest
     * fallback when app-server cannot be reached. History databases and rollout
     * files are never opened.
     */
    // `ids: readonly string[] | null` (std::nullopt is null).
    rt::Task<ThreadMetadataRead> readThreadMetadata(std::string codexHome, std::optional<std::vector<std::string>> ids) {
      ThreadMetadataRead read;
      if (ids && ids->empty()) co_return read;
      const rt::Promise<std::optional<std::string>> finding(findStateDatabase(codexHome));
      const std::optional<std::string> databasePath = co_await finding.wait();
      if (!databasePath || databasePath->empty()) {
        read.reason = "state database not found";
        co_return read;
      }
      const rt::Promise<std::optional<rt::sqlite::Database>> opening(openReadOnlySqlite(*databasePath));
      const std::optional<rt::sqlite::Database> database = co_await opening.wait();
      read.database = databasePath;
      if (!database) {
        read.reason = "state database could not be opened read-only";
        co_return read;
      }
      try {
        const rt::sqlite::Statement statement = !ids ? database->prepare("SELECT " + std::string(METADATA_COLUMNS) + " FROM threads ORDER BY updated_at DESC LIMIT ?")
                                                     : database->prepare("SELECT " + std::string(METADATA_COLUMNS) + " FROM threads WHERE id IN (" + [&ids] {
                                                         const std::vector<std::string> marks(ids->size(), "?");
                                                         return rt::join(marks, ",");
                                                       }() + ")");
        std::vector<rt::sqlite::Binding> bindings;
        if (!ids) bindings.emplace_back(RECENT_METADATA_LIMIT);
        else
          for (const std::string& id : *ids) bindings.emplace_back(id);
        const std::vector<rt::sqlite::Row> result = statement.all(bindings);
        for (const rt::sqlite::Row& row : result) {
          // A bun:sqlite row is always an object (`isRecord(value)`).
          const JsonRecord value = recordOfRow(row);
          const std::optional<std::string> id = textOrNull(rt::get(value, "id"));
          if (!id) continue;
          read.rows.set(*id, metadataFromRow(value));
        }
      } catch (...) {
        read.reason = "state database query failed";
      }
      try {
        database->close();
      } catch (...) {
        // A close failure cannot change the result already read.
      }
      co_return read;
    }

    /* ---------------------------------------------------------------- sampling */

    std::optional<ListedThread> resolvedLiveThreadOf(const std::optional<ListedThread>& listed, const std::optional<ListedThread>& probed) {
      if (!listed) return probed;
      if (!probed) return listed;
      return ListedThread{
        .id = listed->id,
        .name = orElse(listed->name, probed->name),
        .preview = orElse(listed->preview, probed->preview),
        .cwd = orElse(listed->cwd, probed->cwd),
        .model = orElse(listed->model, probed->model),
        .effort = orElse(listed->effort, probed->effort),
        .sourceKind = orElse(listed->sourceKind, probed->sourceKind),
        .parentThreadId = orElse(listed->parentThreadId, probed->parentThreadId),
        .updatedAtMs = orElse(listed->updatedAtMs, probed->updatedAtMs),
        .modelProvider = orElse(listed->modelProvider, probed->modelProvider),
      };
    }

    std::optional<ListedThread> listedThreadOf(const json::value* value) {
      if (!isRecord(value)) return std::nullopt;
      const std::optional<std::string> id = textOrNull(rt::get(*value, "id"));
      if (!id || !THREAD_ID_PATTERN.test(*id)) return std::nullopt;
      return ListedThread{
        .id = *id,
        .name = textOrNull(rt::get(*value, "name")),
        .preview = textOrNull(rt::get(*value, "preview")),
        .cwd = textOrNull(rt::get(*value, "cwd")),
        .model = textOrNull(rt::get(*value, "model")),
        .effort = textOrNull(rt::get(*value, "reasoningEffort")),
        .sourceKind = textOrNull(rt::get(*value, "source")),
        .parentThreadId = textOrNull(rt::get(*value, "parentThreadId")),
        .updatedAtMs = normalizeEpochMs(rt::get(*value, "updatedAt")),
        .modelProvider = textOrNull(rt::get(*value, "modelProvider")),
      };
    }
  }

  std::optional<NewestTurn> newestTurnOf(const json::value* result) {
    if (!isRecord(result) || !isUnknownArray(rt::get(*result, "data"))) return std::nullopt;
    const json::value* first = rt::get(rt::get(*result, "data")->as_array(), std::size_t{0});
    if (!isRecord(first)) return std::nullopt;
    const json::value* items = rt::get(*first, "items");
    return NewestTurn{textOrNull(rt::get(*first, "id")), textOrNull(rt::get(*first, "status")), items ? std::optional<json::value>(*items) : std::nullopt};
  }

  std::optional<NewestTurn> newestTurnOf(const json::value& result) { return newestTurnOf(&result); }

  namespace {
    JsonRecord listThreadParams(const std::optional<std::string>& cursor) {
      JsonRecord params;
      params["limit"] = rt::makeNumber(LIST_PAGE_LIMIT);
      params["useStateDbOnly"] = true;
      json::array sourceKinds;
      for (const std::string& kind : LIST_SOURCE_KINDS) sourceKinds.push_back(json::value(kind));
      params["sourceKinds"] = std::move(sourceKinds);
      params["sortKey"] = "updated_at";
      params["sortDirection"] = "desc";
      if (cursor && !cursor->empty()) params["cursor"] = *cursor;
      return params;
    }

    void appendListedThreads(std::vector<ListedThread>& threads, const json::value* result) {
      if (!isRecord(result) || !isUnknownArray(rt::get(*result, "data"))) return;
      for (const json::value& entry : rt::get(*result, "data")->as_array()) {
        std::optional<ListedThread> thread = listedThreadOf(&entry);
        if (thread) threads.push_back(std::move(*thread));
      }
    }

    // { threads: ListedThread[]; truncated: boolean }
    struct ThreadListing {
      std::vector<ListedThread> threads;
      bool truncated = false;
    };

    rt::Task<ThreadListing> listThreads(SupervisorConnection connection, rt::AbortSignal signal) {
      ThreadListing listing;
      std::optional<std::string> cursor;
      for (double page = 0; page < MAX_LIST_PAGES; page += 1) {
        const rt::Promise<SupervisorCallResult> calling(connection.call("thread/list", listThreadParams(cursor), signal));
        const SupervisorCallResult result = co_await calling.wait();
        appendListedThreads(listing.threads, valueOf(result));
        cursor = isRecord(valueOf(result)) ? textOrNull(rt::get(*result, "nextCursor")) : std::nullopt;
        if (!cursor || static_cast<double>(listing.threads.size()) >= INVENTORY_LIMIT) break;
      }
      if (cursor) listing.truncated = true;
      if (static_cast<double>(listing.threads.size()) > INVENTORY_LIMIT) {
        listing.threads.resize(static_cast<std::size_t>(INVENTORY_LIMIT));
        listing.truncated = true;
      }
      co_return listing;
    }

    rt::Task<ThreadProbe> probeThread(SupervisorConnection connection, std::string threadId, rt::AbortSignal signal) {
      std::optional<std::string> runtimeStatus;
      std::vector<std::string> activeFlags;
      std::optional<ListedThread> threadSummary;
      bool sampled = true;
      try {
        json::object params;
        params["threadId"] = threadId;
        params["includeTurns"] = false;
        const rt::Promise<SupervisorCallResult> calling(connection.call("thread/read", params, signal));
        const SupervisorCallResult result = co_await calling.wait();
        const json::value* thread = isRecord(valueOf(result)) && isRecord(rt::get(*result, "thread")) ? rt::get(*result, "thread") : nullptr;
        threadSummary = thread ? listedThreadOf(thread) : std::nullopt;
        const json::value* status = thread && isRecord(rt::get(*thread, "status")) ? rt::get(*thread, "status") : nullptr;
        runtimeStatus = status ? asString(rt::get(*status, "type")) : std::nullopt;
        if (status && isUnknownArray(rt::get(*status, "activeFlags"))) {
          for (const json::value& flag : rt::get(*status, "activeFlags")->as_array())
            if (flag.is_string()) activeFlags.emplace_back(flag.as_string());
        }
      } catch (...) {
        sampled = false;
      }
      std::optional<std::string> turnStatus;
      try {
        json::object params;
        params["threadId"] = threadId;
        params["limit"] = 1;
        params["sortDirection"] = "desc";
        params["itemsView"] = "notLoaded";
        const rt::Promise<SupervisorCallResult> calling(connection.call("thread/turns/list", params, signal));
        const SupervisorCallResult result = co_await calling.wait();
        const std::optional<NewestTurn> turn = newestTurnOf(valueOf(result));
        turnStatus = turn ? turn->status : std::nullopt;
      } catch (...) {
        sampled = false;
      }
      co_return ThreadProbe{runtimeStatus, activeFlags, turnStatus, threadSummary, sampled};
    }

    bool isTerminalTurnState(const std::optional<std::string>& turnStatus) { return turnStatus && TERMINAL_TURN_STATES.contains(*turnStatus); }
  }

  SupervisorState classifySupervisorState(const ClassifySupervisorStateInput& input) {
    const std::optional<std::string>& runtime = input.runtimeStatus;
    if (runtime == "active") return SupervisorState::active;
    if (runtime == "systemError") return SupervisorState::system_error;
    if (runtime == "idle") return SupervisorState::idle;
    if (runtime == "notLoaded") {
      if (input.turnStatus && !isTerminalTurnState(input.turnStatus)) return SupervisorState::stale;
      return input.turnStatus && isTerminalTurnState(input.turnStatus) ? SupervisorState::idle : SupervisorState::unknown;
    }
    if (input.turnStatus && !isTerminalTurnState(input.turnStatus)) return input.loaded ? SupervisorState::active : SupervisorState::stale;
    return SupervisorState::unknown;
  }

  namespace {
    std::optional<SupervisorQuotaBucket> quotaBucketOf(const json::value* value, const std::string& fallbackId) {
      if (!isRecord(value)) return std::nullopt;
      const json::value* primary = isRecord(rt::get(*value, "primary")) ? rt::get(*value, "primary") : nullptr;
      return SupervisorQuotaBucket{
        .limitId = textOrNull(rt::get(*value, "limitId")).value_or(fallbackId),
        .limitName = textOrNull(rt::get(*value, "limitName")),
        .usedPercent = orElse(asFiniteNumber(rt::get(*value, "usedPercent")), primary ? asFiniteNumber(rt::get(*primary, "usedPercent")) : std::nullopt),
        .windowDurationMins = orElse(asFiniteNumber(rt::get(*value, "windowDurationMins")), primary ? asFiniteNumber(rt::get(*primary, "windowDurationMins")) : std::nullopt),
        .resetsAtMs = orElse(normalizeEpochMs(rt::get(*value, "resetsAt")), primary ? normalizeEpochMs(rt::get(*primary, "resetsAt")) : std::nullopt),
        .planType = textOrNull(rt::get(*value, "planType")),
        .reachedType = textOrNull(rt::get(*value, "rateLimitReachedType")),
      };
    }

    std::vector<SupervisorQuotaBucket> quotaBucketsOf(const json::value* value) {
      if (!isRecord(value)) return {};
      std::vector<SupervisorQuotaBucket> buckets;
      for (const json::key_value_pair* entry : rt::objectEntries(value->as_object())) {
        std::optional<SupervisorQuotaBucket> bucket = quotaBucketOf(&entry->value(), std::string(entry->key()));
        if (bucket) buckets.push_back(std::move(*bucket));
      }
      return buckets;
    }
  }

  std::optional<SupervisorQuota> quotaFromRateLimits(const json::value* result) {
    const json::value* root = isRecord(result) && isRecord(rt::get(*result, "rateLimits")) ? rt::get(*result, "rateLimits") : nullptr;
    if (!root) return std::nullopt;
    std::vector<SupervisorQuotaBucket> buckets = quotaBucketsOf(isRecord(result) ? rt::get(*result, "rateLimitsByLimitId") : nullptr);
    if (buckets.empty()) {
      std::optional<SupervisorQuotaBucket> fallback = quotaBucketOf(root, "codex");
      if (fallback) buckets.push_back(std::move(*fallback));
    }
    const json::value* resetCredits = isRecord(result) ? rt::get(*result, "rateLimitResetCredits") : nullptr;
    const std::optional<double> credits = isRecord(resetCredits) ? asFiniteNumber(rt::get(*resetCredits, "availableCount")) : std::nullopt;
    if (buckets.empty()) return std::nullopt;
    return SupervisorQuota{.accountScope = "shared_codex_account", .buckets = std::move(buckets), .resetCreditsAvailable = credits};
  }

  std::optional<SupervisorQuota> quotaFromRateLimits(const json::value& result) { return quotaFromRateLimits(&result); }

  namespace {
    rt::Task<std::optional<SupervisorQuota>> readQuota(SupervisorConnection connection, rt::AbortSignal signal) {
      std::optional<SupervisorQuota> quota;
      try {
        const json::object params;
        const rt::Promise<SupervisorCallResult> calling(connection.call("account/rateLimits/read", params, signal));
        const SupervisorCallResult result = co_await calling.wait();
        quota = quotaFromRateLimits(valueOf(result));
      } catch (...) {
        co_return std::nullopt;
      }
      co_return quota;
    }

    // { title: string | null; titleSource: "thread" | "metadata" | null }
    struct SupervisorTitle {
      std::optional<std::string> title;
      std::optional<std::string> titleSource;
    };

    SupervisorTitle supervisorTitleOf(const std::optional<ListedThread>& live, const std::optional<ThreadMetadata>& metadata) {
      const std::optional<std::string> threadTitle = live ? orElse(live->name, live->preview) : std::nullopt;
      if (threadTitle && !threadTitle->empty()) return {threadTitle, "thread"};
      const std::optional<std::string> metadataTitle = metadata ? orElse(metadata->title, metadata->preview) : std::nullopt;
      return metadataTitle && !metadataTitle->empty() ? SupervisorTitle{metadataTitle, "metadata"} : SupervisorTitle{std::nullopt, std::nullopt};
    }

    struct SupervisorLiveFields {
      std::optional<std::string> cwd;
      std::optional<std::string> branch;
      std::optional<std::string> model;
      std::optional<std::string> effort;
      std::optional<std::string> provider;
      std::optional<std::string> sourceKind;
      std::optional<std::string> parentThreadId;
      std::optional<double> lastActivityAtMs;
      std::optional<double> tokensUsed;
      std::optional<std::string> usageSource;  // "state_db" | null
    };

    SupervisorLiveFields supervisorLiveFieldsOf(const std::optional<ListedThread>& live, const std::optional<ThreadMetadata>& metadata) {
      const std::optional<double> tokensUsed = metadata ? metadata->tokensUsed : std::nullopt;
      return SupervisorLiveFields{
        .cwd = orElse(live ? live->cwd : std::nullopt, metadata ? metadata->cwd : std::nullopt),
        .branch = metadata ? metadata->gitBranch : std::nullopt,
        .model = orElse(live ? live->model : std::nullopt, metadata ? metadata->model : std::nullopt),
        .effort = orElse(live ? live->effort : std::nullopt, metadata ? metadata->effort : std::nullopt),
        .provider = live ? live->modelProvider : std::nullopt,
        .sourceKind = orElse(live ? live->sourceKind : std::nullopt, metadata ? metadata->threadSource : std::nullopt),
        .parentThreadId = orElse(live ? live->parentThreadId : std::nullopt, metadata ? metadata->parentThreadId : std::nullopt),
        .lastActivityAtMs = orElse(live ? live->updatedAtMs : std::nullopt, metadata ? metadata->updatedAtMs : std::nullopt),
        .tokensUsed = tokensUsed,
        .usageSource = tokensUsed ? std::optional<std::string>("state_db") : std::nullopt,
      };
    }

    struct SupervisorWaits {
      bool waitingOnApproval;
      bool waitingOnUserInput;
    };

    SupervisorWaits supervisorWaitsOf(const std::vector<std::string>& activeFlags) {
      static const rt::Regex APPROVAL("approval", "i");
      static const rt::Regex USER_INPUT("user.?input|input.?request|needsinput", "i");
      SupervisorWaits waits{false, false};
      for (const std::string& flag : activeFlags)
        if (APPROVAL.test(flag)) waits.waitingOnApproval = true;
      for (const std::string& flag : activeFlags)
        if (USER_INPUT.test(flag)) waits.waitingOnUserInput = true;
      return waits;
    }
  }

  SupervisorSession sessionFromParts(const SessionFromPartsInput& input) {
    const std::optional<ThreadMetadata>& metadata = input.metadata;
    const std::optional<ListedThread>& listed = input.listed;
    const std::optional<ThreadProbe>& probe = input.probe;
    const std::optional<ListedThread> live = resolvedLiveThreadOf(listed, probe ? probe->thread : std::nullopt);
    const SupervisorTitle title = supervisorTitleOf(live, metadata);
    const SupervisorLiveFields fields = supervisorLiveFieldsOf(live, metadata);
    const std::vector<std::string> activeFlags = probe ? probe->activeFlags : std::vector<std::string>();
    const SupervisorWaits waits = supervisorWaitsOf(activeFlags);
    SupervisorSession session;
    session.id = input.id;
    session.sourceId = input.source.id;
    session.machine = input.source.name;
    session.title = title.title;
    session.titleSource = title.titleSource;
    session.cwd = fields.cwd;
    session.branch = fields.branch;
    session.model = fields.model;
    session.effort = fields.effort;
    session.provider = fields.provider;
    session.sourceKind = fields.sourceKind;
    session.parentThreadId = fields.parentThreadId;
    session.lastActivityAtMs = fields.lastActivityAtMs;
    session.tokensUsed = fields.tokensUsed;
    session.usageSource = fields.usageSource;
    session.state = probe ? classifySupervisorState({probe->runtimeStatus, probe->turnStatus, input.loaded}) : SupervisorState::unknown;
    session.activeFlags = activeFlags;
    session.loaded = input.loaded;
    session.waitingOnApproval = waits.waitingOnApproval;
    session.waitingOnUserInput = waits.waitingOnUserInput;
    session.childIds = input.childIds;
    session.lastSampledAtMs = input.sampledAtMs;
    session.sampled = probe && probe->sampled;
    session.unavailable = input.unavailable;
    return session;
  }

  namespace {
    // { view: SupervisorSourceView; sessions: SupervisorSession[] }
    struct SourceSample {
      SupervisorSourceView view;
      std::vector<SupervisorSession> sessions;
    };

    bool hasCodexHome(const SupervisorSource& source) { return source.codexHome && !source.codexHome->empty(); }

    SupervisorSourceView unavailableSourceView(const SupervisorSource& source, const std::string& reason, double sampledAtMs, double durationMs, SupervisorSourceView::Metadata metadata,
                                               SupervisorSourceView::Inventory inventory, const std::string& lineage) {
      SupervisorSourceView view;
      view.id = source.id;
      view.name = source.name;
      view.kind = hasCodexHome(source) ? "local" : "remote";
      view.hostname = hasCodexHome(source) ? readHostname() : std::nullopt;
      view.state = "unavailable";
      view.reason = reason;
      view.metadata = std::move(metadata);
      view.inventory = inventory;
      view.lineage = lineage;
      view.quota = std::nullopt;
      view.quotaReason = "source unavailable";
      view.durationMs = durationMs;
      view.sampledAtMs = sampledAtMs;
      return view;
    }

    SupervisorSourceView remoteUnavailableView(const SupervisorSource& source, const std::string& reason, double sampledAtMs, double durationMs) {
      return unavailableSourceView(source, reason, sampledAtMs, durationMs, SupervisorSourceView::Metadata{false, std::nullopt, "remote source has no local metadata database"},
                                   SupervisorSourceView::Inventory{0, 0, 0, false}, "unavailable");
    }

    std::vector<std::string> loadedThreadIdsOf(const json::value* raw) {
      std::vector<std::string> ids;
      if (!isRecord(raw) || !isUnknownArray(rt::get(*raw, "data"))) return ids;
      for (const json::value& value : rt::get(*raw, "data")->as_array())
        if (value.is_string() && THREAD_ID_PATTERN.test(std::string(value.as_string()))) ids.emplace_back(value.as_string());
      return ids;
    }

    rt::Task<ThreadMetadataRead> metadataForSource(SupervisorSource source, std::vector<std::string> ids) {
      if (!hasCodexHome(source)) {
        ThreadMetadataRead read;
        read.reason = "remote source has no local metadata database";
        co_return read;
      }
      const rt::Promise<ThreadMetadataRead> reading(readThreadMetadata(*source.codexHome, std::optional<std::vector<std::string>>(std::move(ids))));
      ThreadMetadataRead read = co_await reading.wait();
      co_return read;
    }

    std::vector<std::string> probeIdsFor(const std::vector<std::string>& listedIds, const std::vector<std::string>& loadedIds) {
      const rt::OrderedSet<std::string> listedIdSet(listedIds.begin(), listedIds.end());
      std::vector<std::string> ids;
      for (std::size_t index = 0; index < listedIds.size() && static_cast<double>(index) < LIVE_PROBE_LIMIT; ++index) ids.push_back(listedIds[index]);
      double extra = 0;
      for (const std::string& id : loadedIds) {
        if (listedIdSet.has(id)) continue;
        if (extra >= EXTRA_LOADED_LIMIT) break;
        ids.push_back(id);
        extra += 1;
      }
      return uniqueStrings(ids);
    }

    rt::OrderedMap<std::string, std::vector<std::string>> childrenByParent(const std::vector<std::string>& rowIds, const std::function<std::optional<ListedThread>(const std::string&)>& liveThreadOf,
                                                                            const rt::OrderedMap<std::string, ThreadMetadata>& metadataRows) {
      rt::OrderedMap<std::string, std::vector<std::string>> childrenOf;
      for (const std::string& threadId : rowIds) {
        const std::optional<ListedThread> live = liveThreadOf(threadId);
        std::optional<std::string> parent = live ? live->parentThreadId : std::nullopt;
        if (!parent) {
          const ThreadMetadata* row = metadataRows.get(threadId);
          parent = row ? row->parentThreadId : std::nullopt;
        }
        if (!parent || parent->empty()) continue;
        std::vector<std::string>* existing = childrenOf.get(*parent);
        if (existing) existing->push_back(threadId);
        else childrenOf.set(*parent, std::vector<std::string>{threadId});
      }
      return childrenOf;
    }

    std::vector<std::string> sessionUnavailableNotes(const SupervisorSource& source, bool sampled, const std::optional<std::string>& metadataReason) {
      std::vector<std::string> notes;
      if (!sampled) notes.emplace_back("live status not sampled in this pass");
      if (!source.codexHome) notes.emplace_back("token usage is not reported by the source");
      else if (metadataReason) notes.push_back("metadata unavailable: " + *metadataReason);
      return notes;
    }

    // The try block of sampleSource: every read of one connected source.
    rt::Task<SourceSample> sampleConnected(SupervisorSource source, rt::AbortSignal signal, SupervisorConnection client, double sampledAtMs, double startedAt) {
      const json::object noParams;
      const rt::Promise<SupervisorCallResult> loading(client.call("thread/loaded/list", noParams, signal));
      const SupervisorCallResult loaded = co_await loading.wait();
      const std::vector<std::string> loadedIds = loadedThreadIdsOf(valueOf(loaded));
      const rt::OrderedSet<std::string> loadedSet(loadedIds.begin(), loadedIds.end());
      const rt::Promise<ThreadListing> listingThreads(listThreads(client, signal));
      const ThreadListing listing = co_await listingThreads.wait();
      std::vector<std::string> listedIds;
      for (const ListedThread& thread : listing.threads) listedIds.push_back(thread.id);
      std::vector<std::string> metadataIds = listedIds;
      for (std::size_t index = 0; index < loadedIds.size() && static_cast<double>(index) < EXTRA_LOADED_LIMIT; ++index) metadataIds.push_back(loadedIds[index]);
      const rt::Promise<ThreadMetadataRead> readingMetadata(metadataForSource(source, uniqueStrings(metadataIds)));
      const ThreadMetadataRead metadata = co_await readingMetadata.wait();
      const std::vector<std::string> probeIds = probeIdsFor(listedIds, loadedIds);
      const auto probeOne = [client, signal](const std::string& threadId) { return probeThread(client, threadId, signal); };
      auto mapping = mapWithConcurrency(probeIds, PROBE_CONCURRENCY, probeOne);
      const std::vector<ThreadProbe> probes = co_await std::move(mapping);
      rt::OrderedMap<std::string, ThreadProbe> probeById;
      for (std::size_t index = 0; index < probeIds.size(); ++index) probeById.set(probeIds[index], probes[index]);

      rt::OrderedMap<std::string, ListedThread> listedById;
      for (const ListedThread& thread : listing.threads) listedById.set(thread.id, thread);
      std::vector<std::string> rowCandidates = listedIds;
      rowCandidates.insert(rowCandidates.end(), probeIds.begin(), probeIds.end());
      const std::vector<std::string> rowIds = uniqueStrings(rowCandidates);
      const std::function<std::optional<ListedThread>(const std::string&)> liveThreadOf = [&listedById, &probeById](const std::string& threadId) {
        const ListedThread* listed = listedById.get(threadId);
        const ThreadProbe* probe = probeById.get(threadId);
        return resolvedLiveThreadOf(listed ? std::optional<ListedThread>(*listed) : std::nullopt, probe ? probe->thread : std::nullopt);
      };
      const rt::OrderedMap<std::string, std::vector<std::string>> childrenOf = childrenByParent(rowIds, liveThreadOf, metadata.rows);
      bool anyProbedThread = false;
      for (const ThreadProbe& probe : probes)
        if (probe.thread) anyProbedThread = true;
      const bool lineageAvailable = !listing.threads.empty() || anyProbedThread || (source.codexHome && !metadata.reason);
      std::vector<SupervisorSession> sessions;
      for (const std::string& threadId : rowIds) {
        const ThreadProbe* probe = probeById.get(threadId);
        const ThreadMetadata* row = metadata.rows.get(threadId);
        const ListedThread* listed = listedById.get(threadId);
        const std::vector<std::string>* children = childrenOf.get(threadId);
        sessions.push_back(sessionFromParts(SessionFromPartsInput{
          .id = threadId,
          .source = source,
          .metadata = row ? std::optional<ThreadMetadata>(*row) : std::nullopt,
          .listed = listed ? std::optional<ListedThread>(*listed) : std::nullopt,
          .probe = probe ? std::optional<ThreadProbe>(*probe) : std::nullopt,
          .loaded = loadedSet.has(threadId),
          .sampledAtMs = sampledAtMs,
          .childIds = lineageAvailable ? std::optional<std::vector<std::string>>(children ? *children : std::vector<std::string>()) : std::nullopt,
          .unavailable = sessionUnavailableNotes(source, probe != nullptr, metadata.reason),
        }));
      }
      const rt::Promise<std::optional<SupervisorQuota>> readingQuota(readQuota(client, signal));
      const std::optional<SupervisorQuota> quota = co_await readingQuota.wait();
      SupervisorSourceView view;
      view.id = source.id;
      view.name = source.name;
      view.kind = hasCodexHome(source) ? "local" : "remote";
      view.hostname = hasCodexHome(source) ? readHostname() : std::nullopt;
      view.state = "ok";
      view.reason = std::nullopt;
      view.metadata = SupervisorSourceView::Metadata{!metadata.reason, metadata.database, metadata.reason};
      view.inventory = SupervisorSourceView::Inventory{
        static_cast<std::int64_t>(listing.threads.size()),
        static_cast<std::int64_t>(probeIds.size()),
        static_cast<std::int64_t>(sessions.size()),
        listing.truncated || probeIds.size() < rowIds.size(),
      };
      view.lineage = lineageAvailable ? "available" : "unavailable";
      view.quota = quota;
      view.quotaReason = quota ? std::nullopt : std::optional<std::string>("account quota read unavailable");
      view.durationMs = nowMs() - startedAt;
      view.sampledAtMs = sampledAtMs;
      co_return SourceSample{std::move(view), std::move(sessions)};
    }

    // The catch block of sampleSource.
    rt::Task<SourceSample> sampleUnavailable(SupervisorSource source, std::string reason, double sampledAtMs, double startedAt) {
      if (!source.codexHome) co_return SourceSample{remoteUnavailableView(source, reason, sampledAtMs, nowMs() - startedAt), {}};
      const rt::Promise<ThreadMetadataRead> reading(readThreadMetadata(*source.codexHome, std::nullopt));
      const ThreadMetadataRead fallback = co_await reading.wait();
      std::vector<SupervisorSession> sessions;
      for (const auto& [threadId, row] : fallback.rows) {
        sessions.push_back(sessionFromParts(SessionFromPartsInput{
          .id = threadId,
          .source = source,
          .metadata = row,
          .listed = std::nullopt,
          .probe = std::nullopt,
          .loaded = false,
          .sampledAtMs = sampledAtMs,
          .childIds = std::nullopt,
          .unavailable = {"live status unavailable: " + reason, "persisted metadata shown; it may be stale"},
        }));
      }
      SupervisorSourceView view = unavailableSourceView(
        source, reason, sampledAtMs, nowMs() - startedAt, SupervisorSourceView::Metadata{!fallback.reason, fallback.database, fallback.reason},
        SupervisorSourceView::Inventory{static_cast<std::int64_t>(fallback.rows.size()), 0, static_cast<std::int64_t>(sessions.size()), static_cast<double>(fallback.rows.size()) >= RECENT_METADATA_LIMIT},
        !fallback.reason ? "available" : "unavailable");
      co_return SourceSample{std::move(view), std::move(sessions)};
    }

    rt::Task<SourceSample> sampleSource(SupervisorSource source, rt::AbortSignal signal) {
      const double startedAt = nowMs();
      const double sampledAtMs = startedAt;
      std::optional<SupervisorConnection> connection;
      std::optional<SourceSample> sample;
      rt::Reason failure;
      try {
        const rt::Promise<SupervisorConnection> opening(openSupervisorConnection(source.socketPath, signal));
        const SupervisorConnection client = co_await opening.wait();
        connection = client;
        auto sampling = sampleConnected(source, signal, client, sampledAtMs, startedAt);
        sample = co_await std::move(sampling);
      } catch (...) {
        failure = std::current_exception();
      }
      rt::Reason fallbackFailure;
      if (failure) {
        const std::string reason = rt::isError(failure) ? rt::errorMessage(failure).value_or("") : "app-server read failed";
        try {
          auto falling = sampleUnavailable(source, reason, sampledAtMs, startedAt);
          sample = co_await std::move(falling);
        } catch (...) {
          fallbackFailure = std::current_exception();
        }
      }
      if (connection) {
        const rt::Promise<void> closing(connection->close());
        co_await closing.wait();
      }
      if (fallbackFailure) std::rethrow_exception(fallbackFailure);
      co_return std::move(*sample);
    }

    double stateOrder(SupervisorState state) {
      switch (state) {
        case SupervisorState::active:
          return 0;
        case SupervisorState::stale:
          return 1;
        case SupervisorState::idle:
          return 2;
        case SupervisorState::unknown:
          return 3;
        case SupervisorState::system_error:
          return 4;
      }
      return 3;
    }

    rt::Task<SupervisorSnapshot> collectSupervisorSnapshot() {
      const rt::Promise<SupervisorConfig> resolving(resolveSupervisorConfig());
      const SupervisorConfig config = co_await resolving.wait();
      const rt::AbortSignal timeout = rt::AbortSignal::timeout(OVERALL_DEADLINE_MS);
      const auto sampleOne = [timeout](const SupervisorSource& source) { return sampleSource(source, timeout); };
      auto mapping = mapWithConcurrency(config.sources, SOURCE_CONCURRENCY, sampleOne);
      const std::vector<SourceSample> samples = co_await std::move(mapping);
      std::vector<SupervisorSession> sessions;
      for (const SourceSample& sample : samples) sessions.insert(sessions.end(), sample.sessions.begin(), sample.sessions.end());
      rt::sort(sessions, [](const SupervisorSession& left, const SupervisorSession& right) {
        const double byState = stateOrder(left.state) - stateOrder(right.state);
        if (byState != 0) return byState;
        return right.lastActivityAtMs.value_or(0) - left.lastActivityAtMs.value_or(0);
      });
      SupervisorSnapshot::Counts counts;
      counts.total = static_cast<std::int64_t>(sessions.size());
      for (const SupervisorSession& session : sessions) {
        if (session.state == SupervisorState::active) counts.active += 1;
        if (session.waitingOnApproval || session.waitingOnUserInput) counts.waiting += 1;
        if (session.state == SupervisorState::idle) counts.idle += 1;
        if (session.state == SupervisorState::stale) counts.stale += 1;
        if (session.state == SupervisorState::unknown) counts.unknown += 1;
        if (session.state == SupervisorState::system_error) counts.systemError += 1;
      }
      std::int64_t listed = 0;
      std::int64_t sampled = 0;
      for (const SourceSample& sample : samples) listed += sample.view.inventory.listed;
      for (const SourceSample& sample : samples) sampled += sample.view.inventory.sampled;
      std::vector<std::string> notes = config.notes;
      std::vector<std::string> unavailableIds;
      for (const SourceSample& sample : samples)
        if (sample.view.state == "unavailable") unavailableIds.push_back(sample.view.id);
      if (!unavailableIds.empty())
        notes.push_back(std::to_string(unavailableIds.size()) + " of " + std::to_string(samples.size()) + " sources unavailable: " + rt::join(unavailableIds, ", "));
      bool truncated = false;
      for (const SourceSample& sample : samples)
        if (sample.view.inventory.truncated) truncated = true;
      if (truncated) notes.emplace_back("inventory was truncated; the panel lists the most recently updated threads per source");
      SupervisorSnapshot snapshot;
      snapshot.sampledAtMs = nowMs();
      snapshot.view = "codex_app_server_read_only";
      for (const SourceSample& sample : samples) snapshot.sources.push_back(sample.view);
      snapshot.sessions = std::move(sessions);
      snapshot.counts = counts;
      snapshot.coverage = SupervisorSnapshot::Coverage{listed, sampled, truncated, std::move(notes)};
      co_return snapshot;
    }

    // { at: number; value: SupervisorSnapshot } | null
    struct SnapshotCache {
      double at;
      SupervisorSnapshot value;
    };
    std::optional<SnapshotCache> snapshotCache;
    std::optional<rt::Promise<SupervisorSnapshot>> snapshotInFlight;
  }

  rt::Task<SupervisorSnapshot> ensureSupervisorSnapshot() {
    const double now = nowMs();
    if (snapshotCache && now - snapshotCache->at < SNAPSHOT_TTL_MS) co_return snapshotCache->value;
    if (!snapshotInFlight) {
      snapshotInFlight = rt::Promise<SupervisorSnapshot>(collectSupervisorSnapshot())
                           .then([](const SupervisorSnapshot& value) {
                             std::size_t bytes = sizeof(SnapshotCache) + rt::memory::bytes(value.view) +
                               value.sources.capacity() * sizeof(value.sources.front()) + value.sessions.capacity() * sizeof(value.sessions.front()) +
                               rt::memory::bytes(value.coverage.notes);
                             snapshotCache = SnapshotCache{nowMs(), value};
                             rt::memory::setCache(rt::memory::Cache::codex_supervisor_inventory, value.sessions.size(), bytes);
                             return value;
                           })
                           .finally_([] { snapshotInFlight = std::nullopt; });
    }
    const rt::Promise<SupervisorSnapshot> inFlight = *snapshotInFlight;
    SupervisorSnapshot value = co_await inFlight.wait();
    co_return value;
  }
}
