// Port of tests/codex-supervisor-followups.test.ts (every case). The brief cases (issues 402/403) drive
// src/codex/supervisor-brief.ts through the TS `briefConnection` fake.
// The last case is differential (followEntryPayload has no TS test): values recorded from Bun
// in tests/codex_supervisor_followups_vectors.inc.
// Links: tests/codex_supervisor_followups_test.cpp src/codex/supervisor_config.cpp src/codex/supervisor_follow.cpp
//        src/codex/supervisor_inventory.cpp src/codex/supervisor_transport.cpp src/runtime/websocket.cpp src/codex/supervisor_brief.cpp
//        src/codex/supervisor_log.cpp src/codex/supervisor_secret.cpp src/runtime/http.cpp src/runtime/http_body.cpp
//        src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/http.cpp src/auth/relay.cpp
//        src/config.cpp src/release.cpp src/provider/cerebras.cpp src/provider/cerebras_rate_limits.cpp src/inference_deadline.cpp
//        src/sentinel/upstream_capture.cpp src/utils.cpp
#include "uos/codex/supervisor_follow.hpp"

#include "uos/codex/supervisor_brief.hpp"

#include "uos/codex/supervisor_config.hpp"
#include "uos/codex/supervisor_transport.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/websocket.hpp"

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace ws = uos::rt::ws;
  using namespace uos::codex::supervisor_follow;
  using uos::codex::supervisor_config::FOLLOW_MAX_ENTRIES;
  using uos::codex::supervisor_config::SupervisorSource;
  using uos::codex::supervisor_transport::openSupervisorConnection;
  using uos::codex::supervisor_transport::setSupervisorWebSocketConstructorForTest;
  using uos::codex::supervisor_transport::SupervisorConnection;

  const SupervisorSource SOURCE{.id = "local", .name = "m1.local", .socketPath = "/var/lib/uos-supervisor/local.sock", .codexHome = std::nullopt};

  using Revisions = rt::OrderedMap<std::string, std::string>;

#include "tests/codex_supervisor_followups_vectors.inc"
}

/* ------------------------------------------------------------------ issue 400 */

TEST("follow updates re-emit one command index as its output, status and exit code change") {
  const std::vector<SupervisorFollowEntry> partial = extractFollowEntries("turn-1", rt::jsonParse(R"([
    { "type": "commandExecution", "id": "c1", "command": "deno task test", "status": "in_progress", "exitCode": null, "aggregatedOutput": "running" }
  ])"));
  Revisions revisions;
  const std::vector<SupervisorFollowEntry> first = selectFollowUpdates(partial, -1, revisions);
  ASSERT(!first.empty());
  const SupervisorFollowEntry firstEntry = first[0];
  EXPECT_EQ(firstEntry.key, "turn-1:c1");
  EXPECT_EQ(firstEntry.text, "running");
  EXPECT_EQ(selectFollowUpdates(partial, 0, revisions).size(), 0u);  // an unchanged poll must not churn

  const std::vector<SupervisorFollowEntry> changed = extractFollowEntries("turn-1", rt::jsonParse(R"([
    { "type": "commandExecution", "id": "c1", "command": "deno task test", "status": "in_progress", "exitCode": null, "aggregatedOutput": "running\nmore" }
  ])"));
  const std::vector<SupervisorFollowEntry> second = selectFollowUpdates(changed, 0, revisions);
  ASSERT(!second.empty());
  const SupervisorFollowEntry secondEntry = second[0];
  EXPECT_EQ(secondEntry.text, "running\nmore");  // later output at the same index must be emitted
  EXPECT(followEntryRevision(secondEntry) != followEntryRevision(firstEntry));

  const std::vector<SupervisorFollowEntry> completed = extractFollowEntries("turn-1", rt::jsonParse(R"([
    { "type": "commandExecution", "id": "c1", "command": "deno task test", "status": "completed", "exitCode": 0, "aggregatedOutput": "running\nmore\nok" }
  ])"));
  const std::vector<SupervisorFollowEntry> third = selectFollowUpdates(completed, 0, revisions);
  ASSERT(!third.empty());
  const SupervisorFollowEntry thirdEntry = third[0];
  EXPECT_EQ(thirdEntry.status, std::optional<std::string>("completed"));  // the final status must be emitted
  EXPECT_EQ(thirdEntry.exitCode, std::optional<double>(0));  // the final exit code must be emitted
  EXPECT_EQ(thirdEntry.text, "running\nmore\nok");

  Revisions reconnect;
  EXPECT_EQ(selectFollowUpdates(completed, 0, reconnect).size(), 1u);  // a reconnect re-baselines the current item once
  EXPECT_EQ(selectFollowUpdates(completed, 0, reconnect).size(), 0u);  // the reconnected stream then stays quiet
}

TEST("follow retains a completed entry that falls outside a sampled update batch") {
  const double count = FOLLOW_MAX_ENTRIES + 2;
  const std::vector<SupervisorFollowEntry> partial = extractFollowEntries("turn-gap", rt::jsonParse(R"([
    { "type": "commandExecution", "id": "c0", "command": "command 0", "status": "in_progress", "exitCode": null, "aggregatedOutput": "partial 0" }
  ])"));
  Revisions revisions;
  const std::vector<SupervisorFollowEntry> first = selectFollowUpdates(partial, -1, revisions);
  EXPECT_EQ(first.size(), 1u);
  ASSERT(!first.empty());
  EXPECT_EQ(first[0].key, "turn-gap:c0");

  json::array items;
  for (int index = 0; index < static_cast<int>(count); ++index) {
    json::object item;
    item["type"] = "commandExecution";
    item["id"] = "c" + std::to_string(index);
    item["command"] = "command " + std::to_string(index);
    item["status"] = "completed";
    item["exitCode"] = index == 0 ? 7 : 0;
    item["aggregatedOutput"] = index == 0 ? std::string("final output") : "final " + std::to_string(index);
    items.push_back(item);
  }
  const std::vector<SupervisorFollowEntry> completed = extractFollowEntries("turn-gap", json::value(items));
  const std::vector<SupervisorFollowEntry> second = selectFollowUpdates(completed, 0, revisions);
  EXPECT_EQ(static_cast<double>(second.size()), FOLLOW_MAX_ENTRIES);
  bool includesFirst = false;
  for (const SupervisorFollowEntry& entry : second)
    if (entry.key == "turn-gap:c0") includesFirst = true;
  EXPECT_EQ(includesFirst, false);  // the completed entry is outside this oversized update batch
  const std::string* kept = revisions.get("turn-gap:c0");
  ASSERT(kept != nullptr);
  EXPECT_EQ(*kept, followEntryRevision(partial[0]));  // a dropped revision must remain uncommitted

  const std::vector<SupervisorFollowEntry> delivered = selectFollowUpdates(completed, count - 1, revisions);
  ASSERT(!delivered.empty());
  EXPECT_EQ(delivered[0].key, "turn-gap:c0");
  EXPECT_EQ(delivered[0].text, "final output");
  EXPECT_EQ(delivered[0].status, std::optional<std::string>("completed"));
  EXPECT_EQ(delivered[0].exitCode, std::optional<double>(7));
}

/* ------------------------------------------------------------------ issue 401 */

namespace {
  /**
   * Minimal structural stand-in for the ws client. It opens on a microtask, then
   * answers the initialize frame on demand so the transport's failure cleanup can
   * be observed without a live socket.
   */
  // Its emit drops every listener of the event before calling them (once and on alike).
  class FakeSupervisorSocket final : public ws::WebSocket {
  public:
    static std::vector<std::shared_ptr<FakeSupervisorSocket>>& created() {
      static std::vector<std::shared_ptr<FakeSupervisorSocket>> sockets;
      return sockets;
    }
    static ws::WebSocketConstructor constructor() {
      return [](std::string, ws::ClientOptions) -> std::shared_ptr<ws::WebSocket> {
        auto socket = std::make_shared<FakeSupervisorSocket>();
        created().push_back(socket);
        rt::queueMicrotask([socket] { socket->emit("open"); });
        return socket;
      };
    }

    std::vector<std::string> frames;
    int readyState_ = 1;
    bool terminated = false;
    bool closed = false;

    FakeSupervisorSocket() : ws::WebSocket(nullptr) {}

    /** Typed accessor so the newest fake is never narrowed to a mutable singleton. */
    static std::shared_ptr<FakeSupervisorSocket> latest() { return created().empty() ? nullptr : created().back(); }
    static void reset() { created().clear(); }

    using ws::WebSocket::emit;
    EventEmitter& on(std::string_view event, ws::Listener listener) override {
      listeners_[std::string(event)].push_back(std::move(listener));
      return *this;
    }
    EventEmitter& once(std::string_view event, ws::Listener listener) override {
      listeners_[std::string(event)].push_back(std::move(listener));
      return *this;
    }
    int readyState() const override { return readyState_; }
    void send(ws::Data data, ws::SendOptions, ws::Callback) override { frames.push_back(std::get<std::string>(data)); }
    void close(std::optional<int>, std::optional<ws::Data>) override {
      closed = true;
      readyState_ = 3;
      emit("close");
    }
    void terminate() override {
      terminated = true;
      readyState_ = 3;
    }
    bool emit(std::string_view event, const ws::EventArgs& args) override {
      const std::vector<ws::Listener> list = listeners_[std::string(event)];
      listeners_[std::string(event)].clear();
      for (const ws::Listener& listener : list) listener(args);
      return !list.empty();
    }

    void rejectInitialize(const std::string& message) {
      const std::string* frame = nullptr;
      for (const std::string& value : frames)
        if (value.find("\"initialize\"") != std::string::npos) {
          frame = &value;
          break;
        }
      ASSERT(frame != nullptr);  // the transport must send an initialize frame
      const json::value id = *rt::get(rt::jsonParse(*frame), "id");
      json::object reply;
      reply["id"] = id;
      reply["error"] = json::object{{"message", message}};
      ws::EventArgs args;
      args.data = rt::jsonStringify(json::value(reply));
      emit("message", args);
    }

  private:
    std::map<std::string, std::vector<ws::Listener>> listeners_;
  };

  // setTimeout(resolve, 0)
  rt::Task<void> tick() { co_await rt::sleep(0); }

  template <class T> rt::Task<std::optional<std::string>> rejectionOf(rt::Promise<T> promise) {
    rt::Reason failure;
    try {
      co_await promise.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    if (!failure) co_return std::nullopt;
    co_return rt::errorMessage(failure).value_or(rt::reasonToString(failure));
  }

  bool matches(const std::optional<std::string>& message, const std::string& pattern) { return message && rt::Regex(pattern, "").test(*message); }

  struct ConstructorSeam {
    explicit ConstructorSeam(ws::WebSocketConstructor constructor) { setSupervisorWebSocketConstructorForTest(std::move(constructor)); }
    ~ConstructorSeam() { setSupervisorWebSocketConstructorForTest(nullptr); }
    ConstructorSeam(const ConstructorSeam&) = delete;
    ConstructorSeam& operator=(const ConstructorSeam&) = delete;
  };
}

ASYNC_TEST("a rejected initialize terminates the exact socket, settles callbacks, and rethrows the cause") {
  FakeSupervisorSocket::reset();
  const ConstructorSeam seam(FakeSupervisorSocket::constructor());
  const rt::Promise<SupervisorConnection> pending(openSupervisorConnection("/var/lib/uos-supervisor/rejected.sock"));
  co_await tick();
  const std::shared_ptr<FakeSupervisorSocket> socket = FakeSupervisorSocket::latest();
  ASSERT(socket != nullptr);  // the transport must construct its own socket
  socket->rejectInitialize("initialize rejected");
  const std::optional<std::string> rejection = co_await rejectionOf(pending);
  EXPECT(matches(rejection, "initialize rejected"));
  EXPECT_EQ(socket->terminated, true);  // the exact failed socket must be terminated

  // A later sampling attempt must build a fresh socket instead of reusing the failed one.
  const rt::Promise<SupervisorConnection> retry(openSupervisorConnection("/var/lib/uos-supervisor/retry.sock"));
  co_await tick();
  const std::shared_ptr<FakeSupervisorSocket> second = FakeSupervisorSocket::latest();
  ASSERT(second != nullptr);
  EXPECT(second != socket);
  second->rejectInitialize("initialize rejected");
  const std::optional<std::string> retryRejection = co_await rejectionOf(retry);
  EXPECT(matches(retryRejection, "initialize rejected"));
  EXPECT_EQ(second->terminated, true);
}

ASYNC_TEST("a stalled initialize terminates the exact socket at the call deadline and rethrows") {
  FakeSupervisorSocket::reset();
  const ConstructorSeam seam(FakeSupervisorSocket::constructor());
  const rt::Promise<SupervisorConnection> pending(openSupervisorConnection("/var/lib/uos-supervisor/stalled.sock"));
  co_await tick();
  const std::shared_ptr<FakeSupervisorSocket> socket = FakeSupervisorSocket::latest();
  ASSERT(socket != nullptr);
  const std::optional<std::string> rejection = co_await rejectionOf(pending);
  EXPECT(matches(rejection, "timed out"));
  EXPECT_EQ(socket->terminated, true);  // the stalled socket must not be left open
}

/* -------------------------------------------------------------- issues 402/403 */

namespace {
  // { result?: unknown; error?: string }
  struct FollowSocketReply {
    std::optional<json::value> result;
    std::optional<std::string> error;
  };

  class ScriptedFollowSocket final : public ws::WebSocket {
  public:
    static std::deque<std::deque<FollowSocketReply>>& scripts() {
      static std::deque<std::deque<FollowSocketReply>> queued;
      return queued;
    }
    static void setScripts(std::vector<std::vector<FollowSocketReply>> values) {
      scripts().clear();
      for (auto& script : values) scripts().emplace_back(script.begin(), script.end());
    }
    static ws::WebSocketConstructor constructor() {
      return [](std::string, ws::ClientOptions) -> std::shared_ptr<ws::WebSocket> {
        auto socket = std::make_shared<ScriptedFollowSocket>();
        rt::queueMicrotask([socket] { socket->emit("open"); });
        return socket;
      };
    }

    int readyState_ = 1;

    ScriptedFollowSocket() : ws::WebSocket(nullptr) {
      if (!scripts().empty()) {
        script_ = scripts().front();
        scripts().pop_front();
      }
    }

    using ws::WebSocket::emit;
    EventEmitter& on(std::string_view event, ws::Listener listener) override {
      listeners_[std::string(event)].push_back(Entry{std::move(listener), false});
      return *this;
    }
    EventEmitter& once(std::string_view event, ws::Listener listener) override {
      listeners_[std::string(event)].push_back(Entry{std::move(listener), true});
      return *this;
    }
    int readyState() const override { return readyState_; }

    void send(ws::Data data, ws::SendOptions, ws::Callback) override {
      const json::value request = rt::jsonParse(std::get<std::string>(data));
      const json::value* id = rt::get(request, "id");
      if (id == nullptr || !rt::isNumber(*id)) return;
      if (script_.empty()) throw rt::JsError("missing scripted supervisor reply");
      const FollowSocketReply reply = script_.front();
      script_.pop_front();
      json::object frame;
      frame["id"] = *id;
      if (reply.error) frame["error"] = json::object{{"message", *reply.error}};
      else if (reply.result) frame["result"] = *reply.result;
      auto self = std::static_pointer_cast<ScriptedFollowSocket>(shared_from_this());
      const std::string text = rt::jsonStringify(json::value(frame));
      rt::queueMicrotask([self, text] {
        ws::EventArgs args;
        args.data = text;
        self->emit("message", args);
      });
    }
    void close(std::optional<int>, std::optional<ws::Data>) override {
      readyState_ = 3;
      emit("close");
    }
    void terminate() override { readyState_ = 3; }
    bool emit(std::string_view event, const ws::EventArgs& args) override {
      const std::vector<Entry> list = listeners_[std::string(event)];
      std::vector<Entry> kept;
      for (const Entry& entry : list)
        if (!entry.once) kept.push_back(entry);
      listeners_[std::string(event)] = kept;
      for (const Entry& entry : list) entry.listener(args);
      return !list.empty();
    }

  private:
    struct Entry {
      ws::Listener listener;
      bool once = false;
    };
    std::deque<FollowSocketReply> script_;
    std::map<std::string, std::vector<Entry>> listeners_;
  };
}

ASYNC_TEST("a failed follow enrichment does not replace an already-rendered command output") {
  const json::value summary = rt::jsonParse(R"({
    "data": [
      {
        "id": "turn-follow",
        "status": "inProgress",
        "items": [{ "type": "commandExecution", "id": "command-1", "command": "printf output", "status": "in_progress", "exitCode": null, "aggregatedOutput": "" }]
      }
    ]
  })");
  const json::value full = rt::jsonParse(R"({
    "data": [
      {
        "id": "turn-follow",
        "status": "inProgress",
        "items": [
          { "type": "commandExecution", "id": "command-1", "command": "printf output", "status": "in_progress", "exitCode": null, "aggregatedOutput": "known output" }
        ]
      }
    ]
  })");
  ScriptedFollowSocket::setScripts({
    {{json::object(), std::nullopt}, {summary, std::nullopt}, {full, std::nullopt}},
    {{json::object(), std::nullopt}, {summary, std::nullopt}, {std::nullopt, "frame cap"}},
  });
  const ConstructorSeam seam(ScriptedFollowSocket::constructor());
  auto revisions = std::make_shared<Revisions>();
  const rt::AbortSignal signal = rt::AbortController().signal();
  auto reading = readFollowUpdate(SOURCE, "thread-follow", "", signal, revisions);
  const FollowUpdate first = co_await std::move(reading);
  ASSERT(!first.entries.empty());  // the successful enrichment must emit the command
  const SupervisorFollowEntry firstEntry = first.entries[0];
  EXPECT_EQ(first.enriched, true);
  EXPECT_EQ(firstEntry.text, "known output");

  auto readingAgain = readFollowUpdate(SOURCE, "thread-follow", first.cursor, signal, revisions);
  const FollowUpdate second = co_await std::move(readingAgain);
  std::string renderedOutput = firstEntry.text;
  for (const SupervisorFollowEntry& entry : second.entries)
    if (entry.key == firstEntry.key && entry.kind == "command") renderedOutput = entry.text;
  EXPECT_EQ(second.enriched, false);
  EXPECT_EQ(second.entries.size(), 0u);  // the failed full read must not emit the empty summary entry
  EXPECT_EQ(renderedOutput, "known output");  // the card keeps output from the successful enrichment
  const std::string* revision = revisions->get(firstEntry.key);
  ASSERT(revision != nullptr);
  EXPECT_EQ(*revision, followEntryRevision(firstEntry));
}

/* -------------------------------------------------------------- issues 402/403 */

namespace {
  using uos::codex::supervisor_brief::BriefTranscriptItem;
  using uos::codex::supervisor_brief::BriefTranscriptTurn;
  using uos::codex::supervisor_brief::buildSupervisorBriefPrompt;
  using uos::codex::supervisor_brief::collectBriefTranscript;
  using uos::codex::supervisor_brief::SupervisorBriefContext;
  using uos::codex::supervisor_transport::SupervisorCallResult;

  struct BriefPages {
    std::optional<json::value> read{};
    std::optional<json::array> asc{};
    std::optional<json::array> desc{};
    /** Full-view pages, consumed in order. Every fixture must use `desc` + `limit: 1`. */
    std::optional<json::array> full{};
    /** Rejects every full-view call with this message, as a frame-cap socket error does. */
    std::optional<std::string> fullError{};
  };

  struct BriefRequest {
    std::string view;
    std::string direction;
    std::optional<double> limit;
    std::optional<std::string> cursor;
  };

  struct BriefFixture {
    SupervisorConnection connection;
    std::shared_ptr<std::vector<std::string>> calls;
    std::shared_ptr<std::vector<BriefRequest>> requests;
  };

  rt::Task<SupervisorCallResult> rejectedBriefCall(std::string message) {
    if (!message.empty()) throw rt::JsError(std::move(message));
    co_return std::nullopt;
  }

  // `pages?.[index] ?? pages?.at(-1)`
  std::optional<json::value> pageAt(const std::optional<json::array>& pages, std::size_t index) {
    if (!pages || pages->empty()) return std::nullopt;
    return index < pages->size() ? (*pages)[index] : pages->back();
  }

  BriefFixture briefConnection(BriefPages pages) {
    struct State {
      BriefPages pages;
      std::size_t ascIndex = 0;
      std::size_t descIndex = 0;
      std::size_t fullIndex = 0;
      std::shared_ptr<std::vector<std::string>> calls = std::make_shared<std::vector<std::string>>();
      std::shared_ptr<std::vector<BriefRequest>> requests = std::make_shared<std::vector<BriefRequest>>();
    };
    const auto state = std::make_shared<State>();
    state->pages = std::move(pages);
    SupervisorConnection connection;
    connection.call = [state](std::string method, json::object params, std::optional<rt::AbortSignal>) -> rt::Task<SupervisorCallResult> {
      const std::string view(rt::getString(params, "itemsView").value_or(""));
      const std::string direction(rt::getString(params, "sortDirection").value_or(""));
      state->calls->push_back(method + ":" + view + ":" + direction);
      const std::optional<std::string_view> cursor = rt::getString(params, "cursor");
      state->requests->push_back(BriefRequest{
        .view = view,
        .direction = direction,
        .limit = rt::getNumber(params, "limit"),
        .cursor = cursor ? std::optional<std::string>(std::string(*cursor)) : std::nullopt,
      });
      const json::value empty = json::object{{"data", json::array{}}};
      if (method == "thread/read") return rt::fulfilledTask<SupervisorCallResult>(state->pages.read.value_or(json::object{}));
      if (view == "summary") {
        if (direction == "asc") {
          const std::optional<json::value> value = pageAt(state->pages.asc, state->ascIndex);
          state->ascIndex += 1;
          return rt::fulfilledTask<SupervisorCallResult>(value.value_or(empty));
        }
        const std::optional<json::value> value = pageAt(state->pages.desc, state->descIndex);
        state->descIndex += 1;
        return rt::fulfilledTask<SupervisorCallResult>(value.value_or(empty));
      }
      if (view == "full") {
        if (state->pages.fullError) return rejectedBriefCall(*state->pages.fullError);
        const std::optional<json::value> value =
          state->pages.full && state->fullIndex < state->pages.full->size() ? std::optional<json::value>((*state->pages.full)[state->fullIndex]) : std::nullopt;
        state->fullIndex += 1;
        return rt::fulfilledTask<SupervisorCallResult>(value.value_or(empty));
      }
      return rt::fulfilledTask<SupervisorCallResult>(json::object{});
    };
    connection.close = [] { return rt::fulfilledTask(); };
    return BriefFixture{.connection = std::move(connection), .calls = state->calls, .requests = state->requests};
  }

  const json::value turnRead = rt::jsonParse(R"({"thread":{"id":"thread-1","name":"Ordering","status":{"type":"active"},"updatedAt":1790000900}})");

  json::value page(json::array data) { return json::object{{"data", std::move(data)}}; }

  rt::Task<SupervisorBriefContext> collectBrief(const SupervisorConnection& connection) {
    rt::AbortController controller;
    return collectBriefTranscript(connection, SOURCE, "thread-1", controller.signal());
  }

  std::vector<std::string> turnIds(const SupervisorBriefContext& context) {
    std::vector<std::string> ids;
    for (const BriefTranscriptTurn& turn : context.turns) ids.push_back(turn.id);
    return ids;
  }

  const BriefTranscriptTurn* findTurn(const SupervisorBriefContext& context, std::string_view id) {
    for (const BriefTranscriptTurn& turn : context.turns)
      if (turn.id == id) return &turn;
    return nullptr;
  }

  bool anyItemIncludes(const BriefTranscriptTurn& turn, std::string_view text) {
    for (const BriefTranscriptItem& item : turn.items)
      if (rt::includes(item.text, text)) return true;
    return false;
  }

  std::size_t countCalls(const std::vector<std::string>& calls, std::string_view part) {
    std::size_t count = 0;
    for (const std::string& call : calls)
      if (rt::includes(call, part)) count += 1;
    return count;
  }

  std::vector<BriefRequest> fullRequests(const std::vector<BriefRequest>& requests) {
    std::vector<BriefRequest> out;
    for (const BriefRequest& request : requests)
      if (request.view == "full") out.push_back(request);
    return out;
  }

  bool promptMatches(const SupervisorBriefContext& context, const char* pattern) { return rt::Regex(pattern, "").test(buildSupervisorBriefPrompt(context)); }
}

ASYNC_TEST("brief turns are deduplicated and presented chronologically") {
  const BriefFixture fixture = briefConnection(BriefPages{
    .read = turnRead,
    .asc = json::array{page(rt::jsonParse(R"([{"id":"turn-a","status":"completed","startedAt":1790000100,"completedAt":1790000110,
      "items":[{"type":"userMessage","id":"u0","content":[{"type":"text","text":"opening request"}]}]}])").as_array())},
    .desc = json::array{page(rt::jsonParse(R"([
      {"id":"turn-c","status":"inProgress","startedAt":1790000300,"items":[{"type":"agentMessage","id":"a2","text":"newest progress"}]},
      {"id":"turn-b","status":"completed","startedAt":1790000200,"completedAt":1790000210,"items":[{"type":"agentMessage","id":"a1","text":"middle progress"}]},
      {"id":"turn-a","status":"completed","startedAt":1790000100,"completedAt":1790000110,"items":[{"type":"agentMessage","id":"a0","text":"opening tail"}]}])")
                                     .as_array())},
  });
  auto collecting = collectBrief(fixture.connection);
  const SupervisorBriefContext context = co_await std::move(collecting);
  EXPECT_EQ(turnIds(context), (std::vector<std::string>{"turn-a", "turn-b", "turn-c"}));  // newest-first summaries must be presented chronologically
  std::size_t openings = 0;
  for (const BriefTranscriptTurn& turn : context.turns)
    if (turn.id == "turn-a") openings += 1;
  EXPECT_EQ(openings, std::size_t{1});  // the repeated opening turn is deduplicated
  EXPECT(promptMatches(context, "opening request"));  // the opening request survives the head/tail merge
  EXPECT(promptMatches(context, "opening tail"));  // the newest tail of the repeated turn survives
  EXPECT(promptMatches(context, "newest progress"));
}

ASYNC_TEST("brief turns stay chronological by page order when every timestamp is missing") {
  const BriefFixture fixture = briefConnection(BriefPages{
    .read = rt::jsonParse(R"({"thread":{"id":"thread-1","name":"No timestamps","status":{"type":"active"}}})"),
    .asc = json::array{page(rt::jsonParse(R"([{"id":"turn-opening","status":"completed",
      "items":[{"type":"userMessage","id":"u0","content":[{"type":"text","text":"opening request"}]}]}])").as_array())},
    .desc = json::array{page(rt::jsonParse(R"([
      {"id":"turn-newest","status":"inProgress","items":[{"type":"agentMessage","id":"n1","text":"newest progress"}]},
      {"id":"turn-middle","status":"completed","items":[{"type":"agentMessage","id":"m1","text":"middle progress"}]}])")
                                     .as_array())},
  });
  auto collecting = collectBrief(fixture.connection);
  const SupervisorBriefContext context = co_await std::move(collecting);
  EXPECT_EQ(turnIds(context), (std::vector<std::string>{"turn-opening", "turn-middle", "turn-newest"}));  // page order alone must produce a chronological transcript
  const std::string prompt = buildSupervisorBriefPrompt(context);
  EXPECT(rt::indexOf(prompt, "middle progress") < rt::indexOf(prompt, "newest progress"));  // the prompt stays chronological
}

ASYNC_TEST("the newest brief turn survives when the transcript budget trims older material") {
  const auto heavyTurn = [](const std::string& id, double startedAt) {
    json::array items;
    for (int index = 0; index < 12; index += 1) {
      items.push_back(json::object{{"type", "agentMessage"}, {"id", id + "-" + std::to_string(index)}, {"text", std::string(1'500, 'x')}});
    }
    return json::object{{"id", id}, {"status", "completed"}, {"startedAt", rt::makeNumber(startedAt)}, {"completedAt", rt::makeNumber(startedAt + 1)}, {"items", items}};
  };
  json::array recent{rt::jsonParse(R"({"id":"turn-newest","status":"inProgress","startedAt":1790000500,
    "items":[{"type":"agentMessage","id":"n1","text":"newest tail survives"}]})")};
  recent.push_back(heavyTurn("turn-heavy-b", 1'790'000'400));
  recent.push_back(heavyTurn("turn-heavy-a", 1'790'000'300));
  const BriefFixture fixture = briefConnection(BriefPages{
    .read = turnRead,
    .asc = json::array{page(json::array{heavyTurn("turn-oldest", 1'790'000'000)})},
    .desc = json::array{page(std::move(recent))},
  });
  auto collecting = collectBrief(fixture.connection);
  const SupervisorBriefContext context = co_await std::move(collecting);
  EXPECT(context.truncated);  // the heavy older turns must mark truncation
  ASSERT(!context.turns.empty());
  EXPECT_EQ(context.turns.back().id, "turn-newest");  // the newest turn stays in the chronological output
  EXPECT(promptMatches(context, "newest tail survives"));
}

ASYNC_TEST("the newest empty turn is enriched from one desc limit-1 full read") {
  const BriefFixture fixture = briefConnection(BriefPages{
    .read = turnRead,
    .asc = json::array{page(rt::jsonParse(R"([{"id":"turn-opening","status":"completed","startedAt":1790000000,"completedAt":1790000005,"items":[]}])").as_array())},
    .desc = json::array{page(rt::jsonParse(R"([
      {"id":"turn-recent","status":"inProgress","startedAt":1790000900,"items":[]},
      {"id":"turn-mid","status":"completed","startedAt":1790000500,"completedAt":1790000510,"items":[{"type":"agentMessage","id":"m1","text":"middle summary"}]}])")
                                     .as_array())},
    .full = json::array{page(rt::jsonParse(R"([{"id":"turn-recent","status":"inProgress","startedAt":1790000900,
      "items":[{"type":"agentMessage","id":"r1","text":"recent full content"}]}])").as_array())},
  });
  auto collecting = collectBrief(fixture.connection);
  const SupervisorBriefContext context = co_await std::move(collecting);
  EXPECT_EQ(countCalls(*fixture.calls, ":full:"), std::size_t{1});  // one frame-safe full read per brief
  const std::vector<BriefRequest> full = fullRequests(*fixture.requests);
  ASSERT(!full.empty());
  EXPECT_EQ(full[0].direction, "desc");  // the full read is descending
  EXPECT_EQ(full[0].limit, std::optional<double>(1));  // the full read is a single turn
  EXPECT_EQ(full[0].cursor, std::nullopt);  // no cursor walk is issued
  const BriefTranscriptTurn* recent = findTurn(context, "turn-recent");
  ASSERT(recent != nullptr);
  EXPECT_EQ(anyItemIncludes(*recent, "recent full content"), true);  // the single newest turn is enriched
  // The older empty opening is out of reach of a desc limit-1 read, so it keeps
  // its empty summary and the budget assembly omits it instead of substituting it.
  EXPECT_EQ(findTurn(context, "turn-opening") != nullptr, false);  // an unreachable opening keeps its honest absence
  EXPECT_EQ(turnIds(context), (std::vector<std::string>{"turn-mid", "turn-recent"}));  // the returned order stays chronological
}

ASYNC_TEST("an unmatched enrichment id is never substituted") {
  const BriefFixture fixture = briefConnection(BriefPages{
    .read = turnRead,
    .asc = json::array{page(rt::jsonParse(R"([{"id":"turn-missing","status":"completed","startedAt":1790000100,"completedAt":1790000110,"items":[]}])").as_array())},
    .desc = json::array{page(rt::jsonParse(R"([{"id":"turn-recent","status":"inProgress","startedAt":1790000300,"items":[]}])").as_array())},
    .full = json::array{page(rt::jsonParse(R"([{"id":"turn-recent","status":"inProgress","startedAt":1790000300,
      "items":[{"type":"agentMessage","id":"r1","text":"recent full content"}]}])").as_array())},
  });
  auto collecting = collectBrief(fixture.connection);
  const SupervisorBriefContext context = co_await std::move(collecting);
  EXPECT_EQ(fullRequests(*fixture.requests).size(), std::size_t{1});  // one full read, no retry for the missing id
  // The unmatched turn's summary is empty and stays unmatched: no substitute
  // content may be fabricated for it, and the budget assembly intentionally
  // omits content-free turns, so it is absent rather than empty.
  EXPECT_EQ(findTurn(context, "turn-missing") != nullptr, false);  // no unmatched turn may be substituted
  EXPECT_EQ(turnIds(context), (std::vector<std::string>{"turn-recent"}));
  const BriefTranscriptTurn* recent = findTurn(context, "turn-recent");
  ASSERT(recent != nullptr);
  EXPECT_EQ(anyItemIncludes(*recent, "recent full content"), true);  // the matched replacement is kept
}

namespace {
  // `{ id, status, ...stamp(value), items }`
  json::value stampedTurn(const std::string& id, const std::string& status, bool withTimestamps, double value, json::array items) {
    json::object turn{{"id", id}, {"status", status}};
    if (withTimestamps) {
      turn["startedAt"] = rt::makeNumber(value);
      turn["completedAt"] = rt::makeNumber(value + 1);
    }
    turn["items"] = std::move(items);
    return turn;
  }

  json::array agentItem(const std::string& id, const std::string& text) { return json::array{json::object{{"type", "agentMessage"}, {"id", id}, {"text", text}}}; }

  rt::Task<void> runOpeningEnrichment(bool withTimestamps) {
    // A single-turn thread: the opening turn is also the newest turn, so the one
    // permitted full shape can reach it.
    const BriefFixture single = briefConnection(BriefPages{
      .read = turnRead,
      .asc = json::array{page(json::array{stampedTurn("turn-opening", "completed", withTimestamps, 1'790'000'100, json::array{})})},
      .desc = json::array{page(json::array{stampedTurn("turn-opening", "completed", withTimestamps, 1'790'000'100, json::array{})})},
      .full = json::array{page(json::array{stampedTurn("turn-opening", "completed", withTimestamps, 1'790'000'100, agentItem("o1", "opening context"))})},
    });
    auto collecting = collectBrief(single.connection);
    const SupervisorBriefContext context = co_await std::move(collecting);
    EXPECT_EQ(fullRequests(*single.requests).size(), std::size_t{1});  // the opening is fetched exactly once
    const BriefTranscriptTurn* opening = findTurn(context, "turn-opening");
    ASSERT(opening != nullptr);  // the opening turn must keep its own full data
    EXPECT_EQ(anyItemIncludes(*opening, "opening context"), true);
    EXPECT(promptMatches(context, "opening context"));

    // An older empty opening is out of reach: the newest turn is the only target
    // the safe read can return, so the opening keeps its recorded summary
    // instead of a cursor walk or a wider full page.
    const BriefFixture older = briefConnection(BriefPages{
      .read = turnRead,
      .asc = json::array{page(json::array{stampedTurn("turn-opening", "completed", withTimestamps, 1'790'000'100, json::array{})})},
      .desc = json::array{page(json::array{
        stampedTurn("turn-newest", "inProgress", withTimestamps, 1'790'000'300, json::array{}),
        stampedTurn("turn-older", "completed", withTimestamps, 1'790'000'200, json::array{}),
      })},
      .full = json::array{page(json::array{stampedTurn("turn-newest", "inProgress", withTimestamps, 1'790'000'300, agentItem("n1", "newest progress survives"))})},
    });
    auto limiting = collectBrief(older.connection);
    const SupervisorBriefContext limited = co_await std::move(limiting);
    EXPECT_EQ(fullRequests(*older.requests).size(), std::size_t{1});  // no second page or retry is attempted
    EXPECT_EQ(turnIds(limited), (std::vector<std::string>{"turn-newest"}));  // only the reachable newest turn is enriched
    EXPECT_EQ(findTurn(limited, "turn-opening") != nullptr || findTurn(limited, "turn-older") != nullptr, false);  // unreachable empty summaries are never substituted
    EXPECT(promptMatches(limited, "newest progress survives"));
  }
}

ASYNC_TEST("the opening turn is enriched only when the single full read returns it") {
  auto withoutTimestamps = runOpeningEnrichment(false);
  co_await std::move(withoutTimestamps);
  auto withTimestamps = runOpeningEnrichment(true);
  co_await std::move(withTimestamps);
}

ASYNC_TEST("every full-view read is a single newest turn: limit 1 and descending") {
  const BriefFixture fixture = briefConnection(BriefPages{
    .read = turnRead,
    .asc = json::array{page(rt::jsonParse(R"([{"id":"turn-opening","status":"completed","items":[]}])").as_array())},
    .desc = json::array{page(rt::jsonParse(R"([{"id":"turn-newest","status":"inProgress","items":[]}])").as_array())},
    .full = json::array{page(rt::jsonParse(R"([{"id":"turn-newest","status":"inProgress","items":[{"type":"agentMessage","id":"n1","text":"newest"}]}])").as_array())},
  });
  auto collecting = collectBrief(fixture.connection);
  (void)co_await std::move(collecting);
  const std::vector<BriefRequest> full = fullRequests(*fixture.requests);
  EXPECT(!full.empty());  // the fixture must exercise the full-view path
  EXPECT(full.size() <= 2);  // at most two full reads per brief
  for (const BriefRequest& request : full) {
    EXPECT_EQ(request.direction, "desc");  // full views are always descending
    EXPECT_EQ(request.limit, std::optional<double>(1));  // a full view is always exactly one turn
    EXPECT_EQ(request.cursor, std::nullopt);  // no cursor walk is ever issued
  }
  bool ascendingFull = false;
  for (const BriefRequest& request : *fixture.requests)
    if (request.view == "full" && request.direction == "asc") ascendingFull = true;
  EXPECT_EQ(ascendingFull, false);  // no ascending full view is ever issued
}

ASYNC_TEST("a failed full read degrades to the recorded summary instead of failing the brief") {
  const BriefFixture fixture = briefConnection(BriefPages{
    .read = turnRead,
    .asc = json::array{page(rt::jsonParse(R"([{"id":"turn-opening","status":"completed","startedAt":1790000100,"completedAt":1790000110,
      "items":[{"type":"userMessage","id":"u0","content":[{"type":"text","text":"recorded summary request"}]}]}])").as_array())},
    .desc = json::array{page(rt::jsonParse(R"([{"id":"turn-newest","status":"inProgress","startedAt":1790000300,"items":[]}])").as_array())},
    // The frame cap errors the socket and rejects every pending call, which is
    // exactly how a too-large full read takes the connection down.
    .fullError = "app-server connection failed",
  });
  auto collecting = collectBrief(fixture.connection);
  const SupervisorBriefContext context = co_await std::move(collecting);
  EXPECT_EQ(countCalls(*fixture.calls, ":full:"), std::size_t{1});  // the fixture must exercise the failing full read
  EXPECT_EQ(fullRequests(*fixture.requests).size(), std::size_t{1});
  EXPECT_EQ(context.transcriptAvailable, true);  // the brief still returns the recorded history
  const BriefTranscriptTurn* opening = findTurn(context, "turn-opening");
  ASSERT(opening != nullptr);
  EXPECT_EQ(anyItemIncludes(*opening, "recorded summary request"), true);  // the recorded summary turn survives the failed enrichment
  EXPECT_EQ(findTurn(context, "turn-newest") != nullptr, false);  // the failed enrichment leaves no substitute content
  EXPECT(promptMatches(context, "recorded summary request"));
}

TEST("differential: follow payloads bound text and command in UTF-16 units and carry the revision") {
  json::array out;
  for (const SupervisorFollowEntry& entry : extractFollowEntries("turn:9", rt::jsonParse(FOLLOW_ITEMS))) {
    json::object item;
    item["payload"] = followEntryPayload(entry);
    item["revision"] = followEntryRevision(entry);
    out.push_back(item);
  }
  // The recording is ASCII-escaped JSON: compare both as JSON.stringify writes them.
  EXPECT_EQ(rt::jsonStringify(json::value(out)), rt::jsonStringify(rt::jsonParse(FOLLOW_PAYLOADS)));
}
