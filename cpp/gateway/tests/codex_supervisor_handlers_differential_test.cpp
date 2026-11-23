// Differential cases for src/codex/supervisor.ts (no TS test drives its two handlers): the sessions
// snapshot JSON (member order included), every rejection branch of the output handler, an SSE follow
// aborted by the client, an SSE follow run to the duration limit (enrichment, an unavailable source read
// reported once, a changed command revision, a heartbeat, the end event) and a resample after the
// snapshot cache expired. The inventory is configured through .data/codex-supervisor.json under a
// temporary working directory: one app-server answered by a routed stand-in socket installed through
// the transport's constructor seam, two unreachable sources, one of them local with a SQLite state
// database. Date.now is frozen and moved by the stand-in. Expected values were recorded from Bun 1.4.0
// with the same stand-in written in TS (tests/codex_supervisor_handlers_differential_vectors.inc).
// Links: tests/codex_supervisor_handlers_differential_test.cpp src/codex/supervisor.cpp src/codex/supervisor_config.cpp
//        src/codex/supervisor_follow.cpp src/codex/supervisor_inventory.cpp src/codex/supervisor_transport.cpp
//        src/runtime/websocket.cpp src/http.cpp src/auth/relay.cpp src/config.cpp src/provider/cerebras_rate_limits.cpp
//        src/release.cpp
// Run with TMPDIR set to a scratch directory: the fixture root is created under rt::os::tmpdir().
#include "uos/codex/supervisor.hpp"

#include "uos/codex/supervisor_transport.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/websocket.hpp"

#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace ws = uos::rt::ws;
  namespace supervisor = uos::codex::supervisor;
  using uos::codex::supervisor_transport::setSupervisorWebSocketConstructorForTest;

#include "codex_supervisor_handlers_differential_vectors.inc"

  const json::object& vectors() {
    static const json::object parsed = rt::jsonParse(kCodexSupervisorVectors).as_object();
    return parsed;
  }

  // `Date.now = () => clock`
  double nowClock = 1'800'000'000'000;
  double summaryCalls = 0;

  // `{ result?: unknown; error?: string }`
  struct Reply {
    std::optional<json::value> result;
    std::optional<std::string> error;
  };
  Reply ok(json::value result) { return Reply{std::move(result), std::nullopt}; }
  Reply failure(std::string message) { return Reply{std::nullopt, std::move(message)}; }

  const std::string E_ACUTE = "\xC3\xA9";
  const std::string GRINNING = "\xF0\x9F\x98\x80";

  json::value turnItems(const std::string& output, const std::string& status, std::optional<double> exitCode) {
    json::array items;
    items.push_back(rt::jsonParse(R"({"type":"userMessage","id":"u1","text":"hi"})"));
    json::object message;
    message["type"] = "agentMessage";
    message["id"] = "m1";
    message["text"] = "Hello " + E_ACUTE + GRINNING;
    message["phase"] = "final";
    items.push_back(std::move(message));
    json::object command;
    command["type"] = "commandExecution";
    command["id"] = "c1";
    command["command"] = "ls -la";
    command["status"] = status;
    command["exitCode"] = exitCode ? rt::makeNumber(*exitCode) : json::value(nullptr);
    command["aggregatedOutput"] = output;
    items.push_back(std::move(command));
    return items;
  }

  json::value turnPage(const std::string& id, const std::string& status, json::value items) {
    json::object turn;
    turn["id"] = id;
    turn["status"] = status;
    turn["items"] = std::move(items);
    json::array data;
    data.push_back(std::move(turn));
    json::object page;
    page["data"] = std::move(data);
    return page;
  }

  // The oracle's `route(socketPath, method, params)`.
  Reply route(const std::string& socketPath, const std::string& method, const json::object& params) {
    const std::string threadId(rt::getString(params, "threadId").value_or(""));
    const std::string itemsView(rt::getString(params, "itemsView").value_or(""));
    if (method == "initialize") return ok(json::object());
    if (method == "thread/loaded/list") return ok(rt::jsonParse(R"({"data":["thread-a","not valid!",7,"thread-x"]})"));
    if (method == "thread/list") {
      json::value page = rt::jsonParse(
          R"({"data":[{"id":"thread-a","name":"Alpha","preview":"first","cwd":"/work/a","model":"gpt-5","reasoningEffort":"high","source":"cli","updatedAt":1800000000,"modelProvider":"openai"},)"
          R"({"id":"thread-b","preview":"child task","parentThreadId":"thread-a","updatedAt":1800000100000,"source":"subAgent"},{"id":"thread-c","updatedAt":1700000000},{"id":"bad id"}],"nextCursor":null})");
      page.as_object()["data"].as_array()[2].as_object()["name"] = "Gamma " + E_ACUTE + GRINNING;
      return ok(std::move(page));
    }
    if (method == "thread/read") {
      if (threadId == "thread-a") return ok(rt::jsonParse(R"({"thread":{"id":"thread-a","status":{"type":"active","activeFlags":["waitingOnApproval",3]}}})"));
      if (threadId == "thread-b") return ok(rt::jsonParse(R"({"thread":{"id":"thread-b","name":"Beta","status":{"type":"idle"}}})"));
      if (threadId == "thread-x") return ok(rt::jsonParse(R"({"thread":{"id":"thread-x","name":"Extra","model":"o3","status":{"type":"notLoaded"}}})"));
      return failure("thread not loaded");
    }
    if (method == "thread/turns/list") {
      if (itemsView == "notLoaded") {
        if (threadId == "thread-a") return ok(rt::jsonParse(R"({"data":[{"id":"turn-a1","status":"inProgress"}]})"));
        if (threadId == "thread-b") return ok(rt::jsonParse(R"({"data":[{"id":"turn-b1","status":"completed"}]})"));
        if (threadId == "thread-x") return ok(rt::jsonParse(R"({"data":[{"id":"turn-x1","status":"inProgress"}]})"));
        return ok(rt::jsonParse(R"({"data":[]})"));
      }
      if (threadId == "thread-b") return ok(turnPage("turn-b1", "completed", rt::jsonParse(R"([{"type":"agentMessage","id":"mb","text":"done"}])")));
      if (itemsView == "full") return ok(turnPage("turn-a1", "inProgress", turnItems("partial", "inProgress", std::nullopt)));
      summaryCalls += 1;
      if (summaryCalls == 1) return ok(turnPage("turn-a1", "inProgress", turnItems("", "inProgress", std::nullopt)));
      if (summaryCalls == 2 || summaryCalls == 3) return failure("thread busy");
      if (summaryCalls == 7) nowClock += 600'000;
      return ok(turnPage("turn-a1", "completed", turnItems("file1\nfile2", "completed", 0)));
    }
    if (method == "account/rateLimits/read") {
      return ok(rt::jsonParse(
          R"({"rateLimits":{"primary":{"usedPercent":50}},"rateLimitsByLimitId":{"codex":{"limitId":"codex","limitName":"Codex","primary":{"usedPercent":12.5,"windowDurationMins":300,"resetsAt":1800003600},"planType":"pro"},)"
          R"("other":{"usedPercent":99,"windowDurationMins":10080,"resetsAt":1800500000000,"rateLimitReachedType":"hard"}},"rateLimitResetCredits":{"availableCount":2}})"));
    }
    return failure("unexpected " + method + " on " + socketPath);
  }

  // The oracle's RoutedSocket: opens (or fails, for a "missing" path) on a microtask and answers every
  // request frame through `route` on a microtask. Its emit drops `once` listeners before calling.
  class RoutedSocket final : public ws::WebSocket {
  public:
    static ws::WebSocketConstructor constructor() {
      return [](std::string url, ws::ClientOptions) -> std::shared_ptr<ws::WebSocket> {
        auto socket = std::make_shared<RoutedSocket>(url.substr(10, url.size() - 12));
        rt::queueMicrotask([socket] {
          if (socket->socketPath_.find("missing") != std::string::npos) socket->emit("error");
          else socket->emit("open");
        });
        return socket;
      };
    }

    explicit RoutedSocket(std::string socketPath) : ws::WebSocket(nullptr), socketPath_(std::move(socketPath)) {}

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
      const json::value* params = rt::get(request, "params");
      const Reply reply = route(socketPath_, std::string(rt::getString(request, "method").value_or("undefined")), params && params->is_object() ? params->as_object() : json::object());
      json::object frame;
      frame["id"] = *id;
      if (reply.error) {
        json::object error;
        error["message"] = *reply.error;
        frame["error"] = std::move(error);
      } else if (reply.result) {
        frame["result"] = *reply.result;
      }
      auto self = std::static_pointer_cast<RoutedSocket>(shared_from_this());
      const std::string text = rt::jsonStringify(json::value(std::move(frame)));
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
    std::string socketPath_;
    int readyState_ = 1;
    std::map<std::string, std::vector<Entry>> listeners_;
  };

  struct ScopedCwd {
    std::string previous = rt::process::cwd();
    explicit ScopedCwd(const std::string& directory) {
      if (::chdir(directory.c_str()) != 0) throw std::runtime_error("chdir failed");
    }
    ~ScopedCwd() { if (::chdir(previous.c_str()) != 0) std::terminate(); }
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;
  };

  std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    return text;
  }

  // `{ status, headers: [...response.headers], body (read to its end; abort after the first entries event), now }`
  rt::Task<json::value> describe(rt::Response response, std::shared_ptr<rt::AbortController> abortAfterEntries, std::string root) {
    json::array headers;
    for (const auto& [name, value] : response.headers().entries()) {
      json::array pair;
      pair.push_back(json::value(name));
      pair.push_back(json::value(value));
      headers.push_back(std::move(pair));
    }
    std::string body;
    if (std::optional<rt::ByteStream> stream = response.body()) {
      auto reader = stream->getReader();
      rt::TextDecoder decoder;
      for (;;) {
        auto chunk = co_await reader.read();
        if (chunk.done) break;
        body += decoder.decode(*chunk.value, rt::TextDecodeOptions{.stream = true});
        if (abortAfterEntries && body.find("event: entries") != std::string::npos) abortAfterEntries->abort();
      }
    }
    body = replaceAll(std::move(body), root, "<root>");
    body = replaceAll(std::move(body), "\"hostname\":\"" + rt::os::hostname() + "\"", "\"hostname\":\"<host>\"");
    json::object out;
    out["status"] = response.status();
    out["headers"] = std::move(headers);
    out["body"] = body;
    out["now"] = rt::makeNumber(nowClock);
    co_return json::value(std::move(out));
  }

  rt::Request outputRequest(const std::string& query, const std::shared_ptr<rt::AbortController>& controller) {
    rt::RequestInit init;
    if (controller) init.signal = controller->signal();
    return rt::Request("https://ai.ubq.fi/admin/codex/supervisor/output?" + query, init);
  }

  ws::WebSocketConstructor trackedConstructor(std::weak_ptr<ws::WebSocket>& socket) {
    const auto construct = RoutedSocket::constructor();
    return [&socket, construct](std::string url, ws::ClientOptions options) {
      auto created = construct(std::move(url), std::move(options));
      socket = created;
      return created;
    };
  }
}

ASYNC_TEST("differential: the supervisor admin handlers match Bun over a routed inventory") {
  const std::string root = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "sv-root-"));
  rt::fs::mkdirSync(rt::path::join(root, ".data"));
  rt::fs::mkdirSync(rt::path::join(root, "home"));
  {
    const rt::sqlite::Database database(rt::path::join(root, "home", "state_1.sqlite"));
    for (const json::value& statement : vectors().at("sql").as_array()) database.run(std::string(statement.as_string()));
    database.close();
  }
  json::value config = vectors().at("config");
  for (json::value& source : config.as_object()["sources"].as_array())
    if (const json::value* home = rt::get(source, "codexHome")) source.as_object()["codexHome"] = replaceAll(std::string(home->as_string()), "<root>", root);
  rt::fs::writeFileSync(rt::path::join(root, ".data", "codex-supervisor.json"), rt::jsonStringify(config));
  const ScopedCwd cwd(root);
  nowClock = 1'800'000'000'000;
  summaryCalls = 0;
  rt::globalDateNow() = [] { return nowClock; };
  setSupervisorWebSocketConstructorForTest(RoutedSocket::constructor());
  const auto restore = rt::scopeExit([] { setSupervisorWebSocketConstructorForTest(nullptr); });

  const json::array& expected = vectors().at("steps").as_array();
  std::size_t next = 0;
  const auto check = [&](const std::string& label, const json::value& actual) {
    ASSERT(next < expected.size());
    const json::array& step = expected[next].as_array();
    next += 1;
    EXPECT_EQ(label, std::string(step.at(0).as_string()));
    const json::object& want = step.at(1).as_object();
    EXPECT_EQ(label + " status " + rt::jsonStringify(actual.at("status")), label + " status " + rt::jsonStringify(want.at("status")));
    EXPECT_EQ(label + " headers " + rt::jsonStringify(actual.at("headers")), label + " headers " + rt::jsonStringify(want.at("headers")));
    EXPECT_EQ(label + " body " + std::string(actual.at("body").as_string()), label + " body " + std::string(want.at("body").as_string()));
    EXPECT_EQ(label + " now " + rt::jsonStringify(actual.at("now")), label + " now " + rt::jsonStringify(want.at("now")));
  };
  const auto sessions = [&](std::string label) -> rt::Task<void> {
    auto handling = supervisor::handleAdminCodexSupervisorSessions();
    const rt::Response response = co_await std::move(handling);
    auto describing = describe(response, nullptr, root);
    const json::value actual = co_await std::move(describing);
    check(label, actual);
  };
  const auto output = [&](std::string label, std::string query, std::shared_ptr<rt::AbortController> aborter) -> rt::Task<void> {
    auto handling = supervisor::handleAdminCodexSupervisorOutput(outputRequest(query, aborter));
    const rt::Response response = co_await std::move(handling);
    auto describing = describe(response, aborter, root);
    const json::value actual = co_await std::move(describing);
    check(label, actual);
  };

  co_await sessions(std::string("sessions"));
  co_await output(std::string("invalid source id"), std::string("source=Bad%20Id&id=thread-a"), nullptr);
  co_await output(std::string("invalid thread id"), std::string("source=lab&id=bad%20id!"), nullptr);
  co_await output(std::string("unknown source"), std::string("source=nope&id=thread-a"), nullptr);
  co_await output(std::string("unknown thread"), std::string("source=%20lab%20&id=thread-zzz"), nullptr);
  co_await output(std::string("unavailable source"), std::string("source=home&id=thread-h1"), nullptr);
  co_await output(std::string("source without sessions"), std::string("source=gone&id=thread-a"), nullptr);
  co_await output(std::string("follow aborted"), std::string("source=lab&id=thread-b&cursor=turn-b1:-1"), std::make_shared<rt::AbortController>());
  const std::string longCursorQuery = "source=lab&id=thread-a&cursor=%20" + std::string(300, 'x');
  co_await output(std::string("follow to the duration limit"), longCursorQuery, nullptr);
  co_await sessions(std::string("sessions after resample"));
  EXPECT_EQ(next, expected.size());

  rt::fs::rmSync(root, rt::fs::RmOptions{.recursive = true, .force = true});
}

ASYNC_TEST("supervisor transport releases a closed socket with its retained listeners") {
  std::weak_ptr<ws::WebSocket> socket;
  setSupervisorWebSocketConstructorForTest(trackedConstructor(socket));
  const auto restore = rt::scopeExit([] { setSupervisorWebSocketConstructorForTest(nullptr); });
  {
    const std::string socketPath = "/fixture/supervisor.sock";
    auto opening = uos::codex::supervisor_transport::openSupervisorConnection(socketPath);
    auto connection = co_await std::move(opening);
    EXPECT(!socket.expired());
    auto closing = connection.close();
    co_await std::move(closing);
  }
  EXPECT(socket.expired());
}

ASYNC_TEST("supervisor transport releases a socket when opening fails") {
  std::weak_ptr<ws::WebSocket> socket;
  setSupervisorWebSocketConstructorForTest(trackedConstructor(socket));
  const auto restore = rt::scopeExit([] { setSupervisorWebSocketConstructorForTest(nullptr); });
  bool rejected = false;
  try {
    const std::string socketPath = "/fixture/missing.sock";
    auto opening = uos::codex::supervisor_transport::openSupervisorConnection(socketPath);
    const auto connection = co_await std::move(opening);
    (void)connection;
  } catch (const rt::JsError& error) {
    rejected = error.message == "app-server socket unavailable";
  }
  EXPECT(rejected);
  EXPECT(socket.expired());
}
