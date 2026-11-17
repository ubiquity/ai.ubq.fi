#include "uos/codex/supervisor.hpp"

#include "uos/codex/supervisor_config.hpp"
#include "uos/codex/supervisor_follow.hpp"
#include "uos/codex/supervisor_inventory.hpp"
#include "uos/http.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::codex::supervisor {
  namespace {
    using supervisor_config::boundedText;
    using supervisor_config::FOLLOW_HEARTBEAT_EVERY;
    using supervisor_config::FOLLOW_MAX_DURATION_MS;
    using supervisor_config::FOLLOW_POLL_MS;
    using supervisor_config::nowMs;
    using supervisor_config::resolveSupervisorConfig;
    using supervisor_config::SOURCE_ID_PATTERN;
    using supervisor_config::SupervisorConfig;
    using supervisor_config::SupervisorQuota;
    using supervisor_config::SupervisorQuotaBucket;
    using supervisor_config::SupervisorSession;
    using supervisor_config::SupervisorSnapshot;
    using supervisor_config::SupervisorSource;
    using supervisor_config::SupervisorSourceView;
    using supervisor_config::THREAD_ID_PATTERN;
    using supervisor_config::waitForPoll;
    using supervisor_follow::followEntryPayload;
    using supervisor_follow::readFollowUpdate;
    using supervisor_inventory::ensureSupervisorSnapshot;

    rt::Response supervisorError(double status, std::string message) { return http::openaiError(status, std::move(message), std::string("invalid_request_error")); }

    // `JSON.stringify(snapshot)`: the members in the order the objects src/codex/supervisor-inventory.ts
    // builds write them.
    json::value textOrNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }

    json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }

    json::value textArray(const std::vector<std::string>& values) {
      json::array out;
      for (const std::string& value : values) out.push_back(json::value(value));
      return out;
    }

    json::value quotaJson(const std::optional<SupervisorQuota>& quota) {
      if (!quota) return nullptr;
      json::array buckets;
      for (const SupervisorQuotaBucket& bucket : quota->buckets) {
        json::object item;
        item["limitId"] = bucket.limitId;
        item["limitName"] = textOrNull(bucket.limitName);
        item["usedPercent"] = numberOrNull(bucket.usedPercent);
        item["windowDurationMins"] = numberOrNull(bucket.windowDurationMins);
        item["resetsAtMs"] = numberOrNull(bucket.resetsAtMs);
        item["planType"] = textOrNull(bucket.planType);
        item["reachedType"] = textOrNull(bucket.reachedType);
        buckets.push_back(std::move(item));
      }
      json::object out;
      out["accountScope"] = quota->accountScope;
      out["buckets"] = std::move(buckets);
      out["resetCreditsAvailable"] = numberOrNull(quota->resetCreditsAvailable);
      return out;
    }

    json::value sourceViewJson(const SupervisorSourceView& view) {
      json::object metadata;
      metadata["available"] = view.metadata.available;
      metadata["database"] = textOrNull(view.metadata.database);
      metadata["reason"] = textOrNull(view.metadata.reason);
      json::object inventory;
      inventory["listed"] = view.inventory.listed;
      inventory["sampled"] = view.inventory.sampled;
      inventory["returned"] = view.inventory.returned;
      inventory["truncated"] = view.inventory.truncated;
      json::object out;
      out["id"] = view.id;
      out["name"] = view.name;
      out["kind"] = view.kind;
      out["hostname"] = textOrNull(view.hostname);
      out["state"] = view.state;
      out["reason"] = textOrNull(view.reason);
      out["metadata"] = std::move(metadata);
      out["inventory"] = std::move(inventory);
      out["lineage"] = view.lineage;
      out["quota"] = quotaJson(view.quota);
      out["quotaReason"] = textOrNull(view.quotaReason);
      out["durationMs"] = rt::makeNumber(view.durationMs);
      out["sampledAtMs"] = rt::makeNumber(view.sampledAtMs);
      return out;
    }

    // sessionFromParts: `{ id, sourceId, machine, ...title, ...fields, state, activeFlags, loaded, ...waits,
    // childIds, lastSampledAtMs, sampled, unavailable }`.
    json::value sessionJson(const SupervisorSession& session) {
      json::object out;
      out["id"] = session.id;
      out["sourceId"] = session.sourceId;
      out["machine"] = session.machine;
      out["title"] = textOrNull(session.title);
      out["titleSource"] = textOrNull(session.titleSource);
      out["cwd"] = textOrNull(session.cwd);
      out["branch"] = textOrNull(session.branch);
      out["model"] = textOrNull(session.model);
      out["effort"] = textOrNull(session.effort);
      out["provider"] = textOrNull(session.provider);
      out["sourceKind"] = textOrNull(session.sourceKind);
      out["parentThreadId"] = textOrNull(session.parentThreadId);
      out["lastActivityAtMs"] = numberOrNull(session.lastActivityAtMs);
      out["tokensUsed"] = numberOrNull(session.tokensUsed);
      out["usageSource"] = textOrNull(session.usageSource);
      out["state"] = std::string(supervisor_config::toString(session.state));
      out["activeFlags"] = textArray(session.activeFlags);
      out["loaded"] = session.loaded ? json::value(*session.loaded) : json::value(nullptr);
      out["waitingOnApproval"] = session.waitingOnApproval;
      out["waitingOnUserInput"] = session.waitingOnUserInput;
      out["childIds"] = session.childIds ? textArray(*session.childIds) : json::value(nullptr);
      out["lastSampledAtMs"] = rt::makeNumber(session.lastSampledAtMs);
      out["sampled"] = session.sampled;
      out["unavailable"] = textArray(session.unavailable);
      return out;
    }

    json::value snapshotJson(const SupervisorSnapshot& snapshot) {
      json::array sources;
      for (const SupervisorSourceView& view : snapshot.sources) sources.push_back(sourceViewJson(view));
      json::array sessions;
      for (const SupervisorSession& session : snapshot.sessions) sessions.push_back(sessionJson(session));
      json::object counts;
      counts["total"] = snapshot.counts.total;
      counts["active"] = snapshot.counts.active;
      counts["waiting"] = snapshot.counts.waiting;
      counts["idle"] = snapshot.counts.idle;
      counts["stale"] = snapshot.counts.stale;
      counts["unknown"] = snapshot.counts.unknown;
      counts["systemError"] = snapshot.counts.systemError;
      json::object coverage;
      coverage["listed"] = snapshot.coverage.listed;
      coverage["sampled"] = snapshot.coverage.sampled;
      coverage["truncated"] = snapshot.coverage.truncated;
      coverage["notes"] = textArray(snapshot.coverage.notes);
      json::object out;
      out["sampledAtMs"] = rt::makeNumber(snapshot.sampledAtMs);
      out["view"] = snapshot.view;
      out["sources"] = std::move(sources);
      out["sessions"] = std::move(sessions);
      out["counts"] = std::move(counts);
      out["coverage"] = std::move(coverage);
      return out;
    }

    // The closure state of one follow stream (`closed`, `polls`, `lastUnavailable`, `unavailablePolls`,
    // `cursor`, `revisions`, `startedAt` and the stream's controller). The abort listener and the
    // stream's cancel hold it weakly; the polling task owns it.
    struct FollowState {
      FollowState(SupervisorSource source_, std::string sourceId_, std::string threadId_, std::string cursor_, rt::AbortSignal signal_, double startedAt_)
          : source(std::move(source_)), sourceId(std::move(sourceId_)), threadId(std::move(threadId_)), cursor(std::move(cursor_)), signal(std::move(signal_)),
            startedAt(startedAt_) {}

      SupervisorSource source;
      std::string sourceId;
      std::string threadId;
      bool closed = false;
      double polls = 0;
      std::string lastUnavailable;
      double unavailablePolls = 0;
      std::string cursor;
      std::shared_ptr<rt::OrderedMap<std::string, std::string>> revisions = std::make_shared<rt::OrderedMap<std::string, std::string>>();
      rt::AbortSignal signal;
      double startedAt;
      std::optional<rt::ReadableStreamDefaultController<rt::ByteChunk>> controller;

      void send(std::string_view event, const json::value& data) {
        if (closed) return;
        try {
          controller->enqueue(rt::encodeUtf8("event: " + std::string(event) + "\ndata: " + rt::jsonStringify(data) + "\n\n"));
        } catch (...) {
          closed = true;
        }
      }

      void stop() {
        if (closed) return;
        closed = true;
        try {
          controller->close();
        } catch (...) {
          // The client may already have gone away.
        }
      }
    };

    rt::Task<void> follow(std::shared_ptr<FollowState> state) {
      json::object ready;
      ready["sourceId"] = state->sourceId;
      ready["threadId"] = state->threadId;
      ready["machine"] = state->source.name;
      ready["mode"] = "recorded";
      ready["note"] = "Updates as Codex records output (about every 2s), not a token-level stream. Only assistant messages and command output are shown.";
      state->send("ready", json::value(std::move(ready)));
      while (!state->closed && !state->signal.aborted() && nowMs() - state->startedAt < FOLLOW_MAX_DURATION_MS) {
        try {
          auto reading = readFollowUpdate(state->source, state->threadId, state->cursor, state->signal, state->revisions);
          const supervisor_follow::FollowUpdate update = co_await std::move(reading);
          state->cursor = update.cursor;
          state->polls += 1;
          if (!update.entries.empty()) {
            json::array entries;
            for (const supervisor_follow::SupervisorFollowEntry& entry : update.entries) entries.push_back(followEntryPayload(entry));
            json::object data;
            data["entries"] = std::move(entries);
            data["cursor"] = update.cursor;
            data["turnId"] = textOrNull(update.turnId);
            data["turnStatus"] = textOrNull(update.turnStatus);
            data["enriched"] = update.enriched;
            data["sampledAtMs"] = rt::makeNumber(nowMs());
            state->send("entries", json::value(std::move(data)));
          } else if (rt::mod(state->polls, FOLLOW_HEARTBEAT_EVERY) == 0) {
            json::object data;
            data["turnId"] = textOrNull(update.turnId);
            data["turnStatus"] = textOrNull(update.turnStatus);
            data["sampledAtMs"] = rt::makeNumber(nowMs());
            state->send("heartbeat", json::value(std::move(data)));
          }
        } catch (...) {
          const rt::Reason error = std::current_exception();
          state->unavailablePolls += 1;
          const std::string message = rt::isError(error) ? boundedText(rt::errorMessage(error).value_or(""), 200) : "output read failed";
          if (message != state->lastUnavailable || rt::mod(state->unavailablePolls, FOLLOW_HEARTBEAT_EVERY) == 0) {
            state->lastUnavailable = message;
            json::object data;
            data["message"] = message;
            data["sampledAtMs"] = rt::makeNumber(nowMs());
            state->send("unavailable", json::value(std::move(data)));
          }
        }
        const rt::Promise<void> waiting = waitForPoll(FOLLOW_POLL_MS, state->signal);
        co_await waiting.wait();
      }
      json::object end;
      end["reason"] = state->signal.aborted() ? "client closed" : "follow duration limit reached";
      state->send("end", json::value(std::move(end)));
      state->stop();
    }
  }

  rt::Task<rt::Response> handleAdminCodexSupervisorSessions() {
    auto sampling = ensureSupervisorSnapshot();
    const SupervisorSnapshot snapshot = co_await std::move(sampling);
    rt::Headers headers;
    headers.set("Cache-Control", "no-store");
    co_return http::json(200, snapshotJson(snapshot), headers);
  }

  rt::Task<rt::Response> handleAdminCodexSupervisorOutput(rt::Request req) {
    rt::URL url(req.url());
    const std::string sourceId = rt::trim(url.searchParams().get("source").value_or(""));
    const std::string threadId = rt::trim(url.searchParams().get("id").value_or(""));
    const std::string initialCursor = rt::slice(rt::trim(url.searchParams().get("cursor").value_or("")), 0, 240);
    if (!SOURCE_ID_PATTERN.test(sourceId)) co_return supervisorError(400, "Invalid supervisor source id");
    if (!THREAD_ID_PATTERN.test(threadId)) co_return supervisorError(400, "Invalid thread id");
    auto resolving = resolveSupervisorConfig();
    const SupervisorConfig config = co_await std::move(resolving);
    const auto configured = std::find_if(config.sources.begin(), config.sources.end(), [&sourceId](const SupervisorSource& entry) { return entry.id == sourceId; });
    if (configured == config.sources.end()) co_return supervisorError(400, "Unknown supervisor source");
    const SupervisorSource source = *configured;
    auto sampling = ensureSupervisorSnapshot();
    const SupervisorSnapshot snapshot = co_await std::move(sampling);
    const bool known = std::any_of(snapshot.sessions.begin(), snapshot.sessions.end(),
                                   [&sourceId, &threadId](const SupervisorSession& session) { return session.sourceId == sourceId && session.id == threadId; });
    if (!known) co_return supervisorError(409, "Session is not in the sampled inventory for this source; refresh the panel and select a listed session");
    const auto sourceView = std::find_if(snapshot.sources.begin(), snapshot.sources.end(), [&sourceId](const SupervisorSourceView& entry) { return entry.id == sourceId; });
    if (sourceView != snapshot.sources.end() && sourceView->state == "unavailable")
      co_return supervisorError(409, "Source " + sourceId + " is unavailable: " + sourceView->reason.value_or("unknown reason"));

    const auto state = std::make_shared<FollowState>(source, sourceId, threadId, initialCursor, req.signal(), nowMs());
    const std::weak_ptr<FollowState> weakState = state;
    rt::UnderlyingSource<rt::ByteChunk> underlying;
    underlying.start = [state](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      state->controller = controller;
      const std::weak_ptr<FollowState> listenerState = state;
      const rt::EventListener stop([listenerState] {
        if (const std::shared_ptr<FollowState> live = listenerState.lock()) live->stop();
      });
      state->signal.addEventListener("abort", stop, {.once = true});
      rt::spawn(follow(state));
    };
    underlying.cancel = [weakState](rt::Reason) {
      if (const std::shared_ptr<FollowState> live = weakState.lock()) live->closed = true;
    };
    const rt::ReadableStream<rt::ByteChunk> stream(underlying);
    rt::ResponseInit init;
    init.headers = rt::Headers{{"Content-Type", "text/event-stream; charset=utf-8"}, {"Cache-Control", "no-store"}, {"X-Accel-Buffering", "no"}};
    co_return rt::Response(rt::BodyInit(stream), init);
  }
}
