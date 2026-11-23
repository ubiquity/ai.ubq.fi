// Supervisor follow polling, split out of src/codex_supervisor.ts.
#include "uos/codex/supervisor_follow.hpp"

#include "uos/codex/supervisor_inventory.hpp"
#include "uos/codex/supervisor_transport.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <cstdint>
#include <exception>
#include <utility>

namespace uos::codex::supervisor_follow {
  using supervisor_config::asFiniteNumber;
  using supervisor_config::asString;
  using supervisor_config::boundedText;
  using supervisor_config::FOLLOW_MAX_COMMAND_CHARS;
  using supervisor_config::FOLLOW_MAX_ENTRIES;
  using supervisor_config::FOLLOW_MAX_TEXT_CHARS;
  using supervisor_config::isRecord;
  using supervisor_config::isUnknownArray;
  using supervisor_config::textOrNull;
  using supervisor_inventory::newestTurnOf;
  using supervisor_inventory::NewestTurn;
  using supervisor_transport::openSupervisorConnection;
  using supervisor_transport::SupervisorCallResult;
  using supervisor_transport::SupervisorConnection;

  std::vector<SupervisorFollowEntry> extractFollowEntries(std::string_view turnId, const json::value* items) {
    if (!isUnknownArray(items)) return {};
    std::vector<SupervisorFollowEntry> entries;
    const json::array& list = items->as_array();
    for (std::size_t position = 0; position < list.size(); ++position) {
      const json::value& item = list[position];
      if (!isRecord(item)) continue;
      const double index = static_cast<double>(position);
      const std::optional<std::string> type = asString(rt::get(item, "type"));
      const std::string id = textOrNull(rt::get(item, "id")).value_or(rt::numberToString(index));
      if (type == "agentMessage") {
        entries.push_back(SupervisorFollowEntry{
          .key = std::string(turnId) + ":" + id,
          .kind = "message",
          .turnId = std::string(turnId),
          .index = index,
          .text = asString(rt::get(item, "text")).value_or(""),
          .command = std::nullopt,
          .status = textOrNull(rt::get(item, "phase")),
          .exitCode = std::nullopt,
          .atMs = std::nullopt,
        });
        continue;
      }
      if (type == "commandExecution") {
        entries.push_back(SupervisorFollowEntry{
          .key = std::string(turnId) + ":" + id,
          .kind = "command",
          .turnId = std::string(turnId),
          .index = index,
          .text = asString(rt::get(item, "aggregatedOutput")).value_or(""),
          .command = asString(rt::get(item, "command")).value_or(""),
          .status = textOrNull(rt::get(item, "status")),
          .exitCode = asFiniteNumber(rt::get(item, "exitCode")),
          .atMs = std::nullopt,
        });
      }
    }
    return entries;
  }

  std::vector<SupervisorFollowEntry> extractFollowEntries(std::string_view turnId, const json::value& items) { return extractFollowEntries(turnId, &items); }

  std::optional<FollowCursor> parseFollowCursor(std::string_view cursor) {
    const std::int64_t separator = rt::lastIndexOf(cursor, ":");
    if (separator <= 0) return std::nullopt;
    const double index = rt::stringToNumber(rt::slice(cursor, separator + 1));
    if (!rt::Number::isSafeInteger(index) || index < -1) return std::nullopt;
    return FollowCursor{rt::slice(cursor, 0, separator), index};
  }

  std::string followEntryRevision(const SupervisorFollowEntry& entry) {
    const std::string& text = entry.text;
    // `hash ^= code; hash = Math.imul(hash, 16777619)` on int32 values, read back with
    // `>>> 0`: the same bits as unsigned 32-bit arithmetic.
    std::uint32_t hash = 2166136261u;
    for (const std::uint16_t unit : rt::toUtf16(text)) {
      hash ^= unit;
      hash *= 16777619u;
    }
    return rt::numberToString(static_cast<double>(rt::length(text))) + ":" + rt::numberToString(static_cast<double>(hash)) + ":" + entry.status.value_or("") + ":" +
           (entry.exitCode ? rt::numberToString(*entry.exitCode) : std::string());
  }

  std::vector<SupervisorFollowEntry> selectFollowUpdates(const std::vector<SupervisorFollowEntry>& entries, double startIndex, rt::OrderedMap<std::string, std::string>& revisions) {
    std::vector<SupervisorFollowEntry> updates;
    for (const SupervisorFollowEntry& entry : entries) {
      const std::string revision = followEntryRevision(entry);
      const std::string* previous = revisions.get(entry.key);
      if (entry.index > startIndex || previous == nullptr || *previous != revision) updates.push_back(entry);
    }
    std::vector<SupervisorFollowEntry> kept;
    for (const SupervisorFollowEntry& entry : updates)
      if (entry.kind == "command" || rt::length(entry.text) > 0) kept.push_back(entry);
    const std::size_t limit = static_cast<std::size_t>(FOLLOW_MAX_ENTRIES);
    std::vector<SupervisorFollowEntry> emitted(kept.size() > limit ? kept.end() - static_cast<std::ptrdiff_t>(limit) : kept.begin(), kept.end());
    for (const SupervisorFollowEntry& entry : emitted) revisions.set(entry.key, followEntryRevision(entry));
    return emitted;
  }

  JsonRecord followEntryPayload(const SupervisorFollowEntry& entry) {
    JsonRecord payload;
    payload["key"] = entry.key;
    payload["kind"] = entry.kind;
    payload["turnId"] = entry.turnId;
    payload["text"] = boundedText(entry.text, FOLLOW_MAX_TEXT_CHARS);
    payload["command"] = entry.command ? json::value(boundedText(*entry.command, FOLLOW_MAX_COMMAND_CHARS)) : json::value(nullptr);
    payload["status"] = entry.status ? json::value(*entry.status) : json::value(nullptr);
    payload["exitCode"] = entry.exitCode ? rt::makeNumber(*entry.exitCode) : json::value(nullptr);
    payload["atMs"] = entry.atMs ? rt::makeNumber(*entry.atMs) : json::value(nullptr);
    payload["revision"] = followEntryRevision(entry);
    return payload;
  }

  namespace {
    std::vector<SupervisorFollowEntry> withText(const std::vector<SupervisorFollowEntry>& entries) {
      std::vector<SupervisorFollowEntry> kept;
      for (const SupervisorFollowEntry& entry : entries)
        if (rt::length(entry.text) > 0) kept.push_back(entry);
      return kept;
    }

    json::object turnsParams(const std::string& threadId, const char* itemsView) {
      json::object params;
      params["threadId"] = threadId;
      params["limit"] = 1;
      params["sortDirection"] = "desc";
      params["itemsView"] = itemsView;
      return params;
    }

    // The try block of readFollowUpdate.
    rt::Task<FollowUpdate> readFromConnection(SupervisorConnection connection, std::string threadId, std::string cursor, rt::AbortSignal signal,
                                              std::shared_ptr<rt::OrderedMap<std::string, std::string>> revisions) {
      const rt::Promise<SupervisorCallResult> summarizing(connection.call("thread/turns/list", turnsParams(threadId, "summary"), signal));
      const SupervisorCallResult summary = co_await summarizing.wait();
      const std::optional<NewestTurn> turn = newestTurnOf(summary ? &*summary : nullptr);
      if (!turn || !turn->id || turn->id->empty()) co_return FollowUpdate{{}, cursor, std::nullopt, std::nullopt, false};
      const std::optional<FollowCursor> previous = parseFollowCursor(cursor);
      const double startIndex = previous && previous->turnId == *turn->id ? previous->index : -1;
      bool enriched = false;
      std::vector<SupervisorFollowEntry> entries = extractFollowEntries(*turn->id, turn->items ? &*turn->items : nullptr);
      std::vector<SupervisorFollowEntry> selectableEntries = entries;
      double lastIndex = !entries.empty() ? entries.back().index : startIndex;
      bool missingContent = false;
      for (const SupervisorFollowEntry& entry : entries)
        if (rt::length(entry.text) == 0) missingContent = true;
      if (missingContent) {
        try {
          const rt::Promise<SupervisorCallResult> reading(connection.call("thread/turns/list", turnsParams(threadId, "full"), signal));
          const SupervisorCallResult full = co_await reading.wait();
          const std::optional<NewestTurn> fullTurn = newestTurnOf(full ? &*full : nullptr);
          if (fullTurn && fullTurn->id == turn->id) {
            enriched = true;
            entries = extractFollowEntries(*turn->id, fullTurn->items ? &*fullTurn->items : nullptr);
            selectableEntries = entries;
            lastIndex = !entries.empty() ? entries.back().index : startIndex;
          } else {
            // A summary item with no text is not a new revision when its full
            // view was unavailable; leave any previously rendered output alone.
            selectableEntries = withText(entries);
          }
        } catch (...) {
          // Do not emit empty summary entries: they would replace known output
          // in the follow panel with its "no recorded output" placeholder.
          selectableEntries = withText(entries);
        }
      }
      std::vector<SupervisorFollowEntry> emitted = selectFollowUpdates(selectableEntries, startIndex, *revisions);
      co_return FollowUpdate{std::move(emitted), *turn->id + ":" + rt::numberToString(lastIndex), turn->id, turn->status, enriched};
    }
  }

  rt::Task<FollowUpdate> readFollowUpdate(SupervisorSource source, std::string threadId, std::string cursor, rt::AbortSignal signal,
                                          std::shared_ptr<rt::OrderedMap<std::string, std::string>> revisions) {
    const rt::Promise<SupervisorConnection> opening(openSupervisorConnection(source.socketPath, signal));
    const SupervisorConnection connection = co_await opening.wait();
    std::optional<FollowUpdate> update;
    rt::Reason failure;
    try {
      auto reading = readFromConnection(connection, threadId, cursor, signal, revisions);
      update = co_await std::move(reading);
    } catch (...) {
      failure = std::current_exception();
    }
    const rt::Promise<void> closing(connection.close());
    co_await closing.wait();
    if (failure) std::rethrow_exception(failure);
    co_return std::move(*update);
  }
}
