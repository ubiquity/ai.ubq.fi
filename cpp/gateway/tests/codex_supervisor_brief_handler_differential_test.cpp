// Differential cases for handleAdminCodexSupervisorBrief (src/codex/supervisor-brief.ts) beyond the TS tests,
// which stop at request validation: every response branch after it (a session outside the sampled inventory,
// a transcript read failure bounded to 200 characters, the unavailable-transcript brief, a generated brief
// from plain and fenced output, an HTTP 500, unusable output, no choices, a rejected fetch and a missing key
// mapped through CerebrasError) plus the exact summarizer request body. The inventory is configured through
// .data/codex-supervisor.json under a temporary working directory; one app-server is answered by a routed
// stand-in socket installed through the transport's constructor seam (its later connections fail every
// thread/read in the "collect failure" scenario). Date.now is frozen and advanced past the snapshot cache
// between scenarios. CEREBRAS_API_KEY holds the synthetic value "synthetic-key" (or nothing) per scenario;
// HOME, CODEX_HOME and CODEX_AUTH_JSON_B64 are cleared. Expected values were recorded from Bun 1.4.0 with the
// same stand-in written in TS (tests/codex_supervisor_brief_handler_differential_vectors.inc).
// Run with TMPDIR set to a scratch directory: the fixture root is created under rt::os::tmpdir().
// Links: tests/codex_supervisor_brief_handler_differential_test.cpp src/codex/supervisor_brief.cpp src/codex/supervisor_config.cpp
//        src/codex/supervisor_inventory.cpp src/codex/supervisor_log.cpp src/codex/supervisor_secret.cpp src/codex/supervisor_transport.cpp
//        src/runtime/websocket.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/http.cpp src/auth/relay.cpp src/config.cpp src/release.cpp src/provider/cerebras.cpp
//        src/provider/cerebras_rate_limits.cpp src/inference_deadline.cpp src/sentinel/upstream_capture.cpp src/utils.cpp
#include "uos/codex/supervisor_brief.hpp"

#include "uos/codex/supervisor_transport.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
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
  using uos::codex::supervisor_brief::handleAdminCodexSupervisorBrief;
  using uos::codex::supervisor_transport::setSupervisorWebSocketConstructorForTest;

#include "codex_supervisor_brief_handler_differential_vectors.inc"

  const json::array& vectors() {
    static const json::value parsed = rt::jsonParse(BRIEF_HANDLER_VECTORS);
    return parsed.as_array();
  }

  // `Date.now = () => clock`
  double nowClock = 1'800'000'000'000;
  int socketsOpened = 0;
  bool failBriefReads = false;

  // `{ result?: unknown; error?: string }`
  struct Reply {
    std::optional<json::value> result;
    std::optional<std::string> error;
  };
  Reply ok(json::value result) { return Reply{std::move(result), std::nullopt}; }
  Reply failure(std::string message) { return Reply{std::nullopt, std::move(message)}; }

  // The oracle's `route(ordinal, method, params)`.
  Reply route(int ordinal, const std::string& method, const json::object& params) {
    const std::string threadId(rt::getString(params, "threadId").value_or(""));
    const std::string view(rt::getString(params, "itemsView").value_or(""));
    const std::string direction(rt::getString(params, "sortDirection").value_or(""));
    if (method == "initialize") return ok(json::object());
    if (method == "thread/loaded/list") return ok(rt::jsonParse(R"({"data":["thread-a","thread-b"]})"));
    if (method == "thread/list") {
      return ok(rt::jsonParse(R"({"data":[{"id":"thread-a","name":"Alpha","preview":"first","cwd":"/work/a","model":"gpt-5","reasoningEffort":"high","source":"cli",)"
                              R"("updatedAt":1800000000,"modelProvider":"openai"},{"id":"thread-b","name":"Beta","updatedAt":1799999000,"source":"cli"}],"nextCursor":null})"));
    }
    if (method == "thread/read") {
      if (failBriefReads && ordinal >= 2) return failure("transcript store unavailable " + std::string(300, 'x'));
      if (threadId == "thread-a") {
        return ok(rt::jsonParse(R"({"thread":{"id":"thread-a","name":"Alpha","cwd":"/work/a","model":"gpt-5","modelProvider":"openai","reasoningEffort":"high",)"
                                R"("status":{"type":"active","activeFlags":["waitingOnApproval",3]},"updatedAt":1800000000}})"));
      }
      if (threadId == "thread-b") return ok(rt::jsonParse(R"({"thread":{"id":"thread-b","name":"Beta","status":{"type":"idle"}}})"));
      return failure("thread not loaded");
    }
    if (method == "thread/turns/list") {
      if (view == "notLoaded") {
        if (threadId == "thread-a") return ok(rt::jsonParse(R"({"data":[{"id":"turn-a2","status":"inProgress"}]})"));
        if (threadId == "thread-b") return ok(rt::jsonParse(R"({"data":[{"id":"turn-b1","status":"completed"}]})"));
        return ok(rt::jsonParse(R"({"data":[]})"));
      }
      if (threadId == "thread-b") return ok(rt::jsonParse(R"({"data":[]})"));
      if (view == "full") {
        return ok(rt::jsonParse(R"({"data":[{"id":"turn-a2","status":"inProgress","startedAt":1799999900,"items":[{"type":"agentMessage","text":"full newest progress"}]}]})"));
      }
      if (direction == "asc") {
        return ok(rt::jsonParse(R"({"data":[{"id":"turn-a1","status":"completed","startedAt":1799999000,"completedAt":1799999100,"items":[)"
                                R"({"type":"userMessage","content":[{"type":"text","text":"Ship the brief route. token=Bearer abcdefghijklmnopqrst"}]},)"
                                R"({"type":"commandExecution","command":"deno task test","status":"completed","exitCode":0,"aggregatedOutput":"ok é😀"}]}]})"));
      }
      return ok(rt::jsonParse(R"({"data":[{"id":"turn-a2","status":"inProgress","startedAt":1799999900,"items":[]},)"
                              R"({"id":"turn-a1","status":"completed","startedAt":1799999000,"completedAt":1799999100,"items":[{"type":"agentMessage","text":"opening tail é😀 café"}]}]})"));
    }
    if (method == "account/rateLimits/read") return ok(json::object());
    return failure("unexpected " + method);
  }

  // The oracle's RoutedSocket: opens on a microtask and answers every request frame through `route` on a
  // microtask. Its emit drops `once` listeners before calling.
  class RoutedSocket final : public ws::WebSocket {
  public:
    static ws::WebSocketConstructor constructor() {
      return [](std::string, ws::ClientOptions) -> std::shared_ptr<ws::WebSocket> {
        socketsOpened += 1;
        auto socket = std::make_shared<RoutedSocket>(socketsOpened);
        rt::queueMicrotask([socket] { socket->emit("open"); });
        return socket;
      };
    }

    explicit RoutedSocket(int ordinal) : ws::WebSocket(nullptr), ordinal_(ordinal) {}

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
      const json::value* method = rt::get(request, "method");
      const Reply reply = route(ordinal_, rt::toString(method), params && params->is_object() ? params->as_object() : json::object());
      json::object frame;
      frame["id"] = *id;
      if (reply.error) frame["error"] = json::object{{"message", *reply.error}};
      else frame["result"] = reply.result ? *reply.result : json::value(nullptr);
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
    int ordinal_;
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

  // The configured root: `.data/codex-supervisor.json` naming the one app-server.
  struct ConfiguredRoot {
    std::string path = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "brief-handler-"));
    ConfiguredRoot() {
      rt::fs::mkdirSync(rt::path::join(path, ".data"));
      const std::string config = R"({"sources":[{"id":"app","name":"App server","socketPath":"/var/lib/uos-supervisor/app.sock"}]})";
      rt::fs::writeFileSync(rt::path::join(path, ".data", "codex-supervisor.json"), config);
    }
    ~ConfiguredRoot() { rt::fs::rmSync(path, {.recursive = true, .force = true}); }
    ConfiguredRoot(const ConfiguredRoot&) = delete;
    ConfiguredRoot& operator=(const ConfiguredRoot&) = delete;
  };

  rt::Task<rt::Response> rejectWith(std::string name, std::string message) {
    if (name == "TypeError") throw rt::TypeError(std::move(message));
    throw rt::JsError(std::move(message));
    co_return rt::Response();
  }

  rt::Response completion(const json::value& content) {
    json::object message{{"role", "assistant"}};
    message["content"] = content;
    json::object choice{{"index", 0}};
    choice["message"] = std::move(message);
    choice["finish_reason"] = "stop";
    json::object payload{{"id", "chatcmpl-synthetic"}, {"object", "chat.completion"}, {"created", 1}, {"model", "gpt-oss-120b"}};
    payload["choices"] = json::array{std::move(choice)};
    return rt::Response::json(json::value(std::move(payload)));
  }

  // The oracle's `respondWith(respond)`; a missing descriptor is "unexpected summarizer call".
  rt::Task<rt::Response> respondWith(const json::value& respond) {
    if (respond.is_null()) return rejectWith("Error", "unexpected summarizer call");
    const std::string kind(respond.at("kind").as_string());
    if (kind == "completion") return rt::fulfilledTask(completion(respond.at("content")));
    if (kind == "status") {
      rt::ResponseInit init;
      init.status = *rt::getNumber(respond, "status");
      return rt::fulfilledTask(rt::Response(rt::BodyInit(std::string(respond.at("text").as_string())), init));
    }
    if (kind == "json") return rt::fulfilledTask(rt::Response::json(respond.at("value")));
    return rejectWith(std::string(respond.at("error").as_string()), std::string(respond.at("message").as_string()));
  }

  rt::Request briefRequest(const std::string& body) {
    rt::RequestInit init;
    init.method = "POST";
    init.body = rt::BodyInit(body);
    init.headers = rt::Headers{{"Content-Type", "application/json"}};
    return rt::Request("https://ai.ubq.fi/admin/codex/supervisor/brief", std::move(init));
  }

  // `{ name, status, headers: [...response.headers], body, outbound, now }`
  rt::Task<json::value> record(std::string name, rt::Response response, std::shared_ptr<std::vector<std::string>> outbound) {
    json::array headers;
    for (const auto& [header, value] : response.headers().entries()) headers.push_back(json::array{json::value(header), json::value(value)});
    auto reading = response.text();
    const std::string body = co_await std::move(reading);
    json::array sent;
    for (const std::string& entry : *outbound) sent.push_back(json::value(entry));
    json::object out;
    out["name"] = name;
    out["status"] = response.status();
    out["headers"] = std::move(headers);
    out["body"] = body;
    out["outbound"] = std::move(sent);
    out["now"] = rt::makeNumber(nowClock);
    co_return json::value(std::move(out));
  }
}

ASYNC_TEST("differential: the brief route matches Bun over a routed inventory") {
  const rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}, {"CEREBRAS_API_KEY", std::nullopt}};
  const ConfiguredRoot root;
  const ScopedCwd cwd(root.path);
  rt::globalDateNow() = [] { return nowClock; };
  setSupervisorWebSocketConstructorForTest(RoutedSocket::constructor());
  const auto restore = rt::scopeExit([] { setSupervisorWebSocketConstructorForTest(nullptr); });

  ASSERT_EQ(vectors().size(), std::size_t{10});
  for (const json::value& vector : vectors()) {
    const json::object& input = vector.at("input").as_object();
    nowClock += 10'000;
    socketsOpened = 0;
    failBriefReads = input.at("failBriefReads").as_bool();
    const json::value& key = input.at("key");
    const rt::env::ScopedOverride keyScope{{"CEREBRAS_API_KEY", key.is_null() ? std::nullopt : std::optional<std::string>(std::string(key.as_string()))}};
    const auto outbound = std::make_shared<std::vector<std::string>>();
    const json::value respond = input.at("respond");
    const rt::GlobalFetchOverride summarizer(rt::Fetch([outbound, respond](rt::RequestInfo, rt::RequestInit init) {
      const std::string* text = init.body ? std::get_if<std::string>(&*init.body) : nullptr;
      outbound->push_back(text ? *text : "<non-string body>");
      return respondWith(respond);
    }));
    auto handling = handleAdminCodexSupervisorBrief(briefRequest(std::string(input.at("body").as_string())));
    const rt::Response response = co_await std::move(handling);
    auto recording = record(std::string(vector.at("name").as_string()), response, outbound);
    const json::value actual = co_await std::move(recording);
    const json::object& expected = vector.as_object();
    for (const char* member : {"name", "status", "headers", "body", "outbound", "now"}) {
      const bool same = rt::deepEqual(actual.at(member), expected.at(member));
      if (!same) {
        std::fprintf(stderr, "    %s %s:\n      actual   %s\n      expected %s\n", rt::jsonStringify(expected.at("name")).c_str(), member,
                     rt::jsonStringify(actual.at(member)).c_str(), rt::jsonStringify(expected.at(member)).c_str());
      }
      EXPECT(same);
    }
  }
}
