// tests/codex-supervisor-inventory-coverage.test.ts: every case (the src/codex/supervisor-config.ts,
// supervisor-inventory.ts, supervisor-transport.ts and supervisor-brief.ts cases). In "message frames are
// decoded as text, bytes or multipart arrays ..." the number frame and the Buffer[] frame are not
// representable: rt::ws hands a listener a Buffer or a string only (binaryType is always "nodebuffer").
// The brief cases clear CEREBRAS_API_KEY, HOME, CODEX_HOME and CODEX_AUTH_JSON_B64 for their scope; the
// handler case that resolves the configuration runs in the fixture directory, as the snapshot case does.
//
// Fixture: the TS process confines itself with Landlock so the existing
// `.data/codex-supervisor.json` cannot be read (EACCES). Applying kernel confinement to this
// test binary needs the permissions addon (src/runtime/permissions-linux.ts, not ported), so
// the configuration path here is a directory instead (EISDIR): the same non-ENOENT read
// failure for any user, which takes resolveSupervisorConfig down the same branch.
// The snapshot case runs with this process's working directory switched to that fixture and
// HOME/CODEX_HOME removed, as the TS process runs (chdir in beforeAll, a sparse environment).
// Run with TMPDIR set to a scratch directory (the fixture uses rt::os::tmpdir()).
// Links: tests/codex_supervisor_inventory_coverage_test.cpp src/codex/supervisor_config.cpp src/codex/supervisor_inventory.cpp
//        src/codex/supervisor_transport.cpp src/runtime/websocket.cpp src/codex/supervisor_brief.cpp src/codex/supervisor_log.cpp
//        src/codex/supervisor_secret.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp
//        src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/http.cpp src/auth/relay.cpp src/config.cpp src/release.cpp
//        src/provider/cerebras.cpp src/provider/cerebras_rate_limits.cpp src/inference_deadline.cpp src/sentinel/upstream_capture.cpp
//        src/utils.cpp
#include "uos/codex/supervisor_brief.hpp"
#include "uos/codex/supervisor_config.hpp"
#include "uos/codex/supervisor_inventory.hpp"
#include "uos/codex/supervisor_transport.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/websocket.hpp"
#include <exception>
#include <algorithm>
#include <unistd.h>
#include <deque>
#include <map>
#include <memory>

namespace {
  using namespace uos::codex::supervisor_config;
  namespace rt = uos::rt;
  namespace json = uos::json;

  std::vector<std::string> idsOf(const std::vector<SupervisorSource>& sources) {
    std::vector<std::string> ids;
    for (const SupervisorSource& source : sources) ids.push_back(source.id);
    return ids;
  }

  bool someNote(const std::vector<std::string>& notes, std::string_view text) {
    return std::any_of(notes.begin(), notes.end(), [text](const std::string& note) { return rt::includes(note, text); });
  }

  // CONFINED_CWD with an unreadable `.data/codex-supervisor.json`; removed afterwards.
  struct ConfinedFixture {
    std::string cwd = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "m09-supervisor-"));
    ConfinedFixture() {
      rt::fs::mkdirSync(rt::path::join(cwd, ".data"));
      rt::fs::mkdirSync(rt::path::join(cwd, ".data", "codex-supervisor.json"));
    }
    ~ConfinedFixture() { rt::fs::rmSync(cwd, {.recursive = true, .force = true}); }
    ConfinedFixture(const ConfinedFixture&) = delete;
    ConfinedFixture& operator=(const ConfinedFixture&) = delete;
  };
}

/* --------------------------------------------------- inventory: projections */

namespace {
  using uos::codex::supervisor_inventory::classifySupervisorState;
  using uos::codex::supervisor_inventory::ensureSupervisorSnapshot;
  using uos::codex::supervisor_inventory::newestTurnOf;
  using uos::codex::supervisor_inventory::NewestTurn;
  using uos::codex::supervisor_inventory::quotaFromRateLimits;
  using uos::codex::supervisor_inventory::sessionFromParts;
  using uos::codex::supervisor_inventory::SessionFromPartsInput;

  // `process.chdir(dir)` for the scope.
  struct ScopedCwd {
    std::string previous = rt::process::cwd();
    explicit ScopedCwd(const std::string& directory) {
      if (::chdir(directory.c_str()) != 0) throw std::runtime_error("chdir failed");
    }
    ~ScopedCwd() { if (::chdir(previous.c_str()) != 0) std::terminate(); }
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;
  };
}

TEST("quotaFromRateLimits falls back to the account-wide bucket when no per-limit map exists") {
  const std::optional<SupervisorQuota> quota =
    quotaFromRateLimits(rt::jsonParse(R"({ "rateLimits": { "limitId": "codex", "primary": { "usedPercent": 12, "windowDurationMins": 300, "resetsAt": 1790000000 } } })"));
  ASSERT(quota.has_value());  // a single account-wide bucket must still be reported
  EXPECT_EQ(quota->accountScope, "shared_codex_account");
  EXPECT(!quota->resetCreditsAvailable.has_value());  // an absent credit count stays unknown rather than zero
  const std::vector<SupervisorQuotaBucket> expected{{
    .limitId = "codex",
    .limitName = std::nullopt,
    .usedPercent = 12,
    .windowDurationMins = 300,
    .resetsAtMs = 1'790'000'000'000,
    .planType = std::nullopt,
    .reachedType = std::nullopt,
  }};
  EXPECT(quota->buckets == expected);
}

TEST("quotaFromRateLimits ignores non-object buckets and prefers explicit bucket fields over the legacy primary window") {
  const std::optional<SupervisorQuota> quota = quotaFromRateLimits(rt::jsonParse(R"({
    "rateLimits": { "limitId": "codex" },
    "rateLimitsByLimitId": {
      "codex": { "limitId": "codex", "usedPercent": 41, "windowDurationMins": 60, "resetsAt": 1790000100, "planType": "pro", "rateLimitReachedType": "primary" },
      "gpt6": "not-a-bucket",
      "broken": null
    }
  })"));
  ASSERT(quota.has_value());
  std::vector<std::string> limitIds;
  for (const SupervisorQuotaBucket& bucket : quota->buckets) limitIds.push_back(bucket.limitId);
  EXPECT_EQ(limitIds, (std::vector<std::string>{"codex"}));  // a bucket whose entry is not an object is dropped instead of fabricated
  ASSERT(!quota->buckets.empty());
  EXPECT_EQ(quota->buckets[0].usedPercent, std::optional<double>(41));
  EXPECT_EQ(quota->buckets[0].planType, std::optional<std::string>("pro"));
  EXPECT_EQ(quota->buckets[0].reachedType, std::optional<std::string>("primary"));
}

TEST("quotaFromRateLimits reads the fallback bucket fields from the nested primary window") {
  const std::optional<SupervisorQuota> quota =
    quotaFromRateLimits(rt::jsonParse(R"({ "rateLimits": { "limitId": null, "primary": { "usedPercent": 7, "windowDurationMins": 5, "resetsAt": 1790000200 } } })"));
  ASSERT(quota.has_value());
  ASSERT(!quota->buckets.empty());
  EXPECT(quota->buckets[0] == (SupervisorQuotaBucket{
                                .limitId = "codex",
                                .limitName = std::nullopt,
                                .usedPercent = 7,
                                .windowDurationMins = 5,
                                .resetsAtMs = 1'790'000'200'000,
                                .planType = std::nullopt,
                                .reachedType = std::nullopt,
                              }));
}

TEST("newestTurnOf reports only a well-formed turn record") {
  EXPECT(newestTurnOf(json::value(nullptr)) == std::nullopt);
  EXPECT(newestTurnOf(rt::jsonParse(R"({ "data": "not-an-array" })")) == std::nullopt);
  EXPECT(newestTurnOf(rt::jsonParse(R"({ "data": [null] })")) == std::nullopt);
  const std::optional<NewestTurn> turn = newestTurnOf(rt::jsonParse(R"({ "data": [{ "id": "turn-1", "status": "inProgress", "items": [{ "type": "agentMessage" }] }] })"));
  ASSERT(turn.has_value());
  EXPECT_EQ(turn->id, std::optional<std::string>("turn-1"));
  EXPECT_EQ(turn->status, std::optional<std::string>("inProgress"));
  ASSERT(turn->items.has_value());
  EXPECT(rt::deepEqual(*turn->items, rt::jsonParse(R"([{ "type": "agentMessage" }])")));
  // only the newest record is read, so a non-record head means no turn
  EXPECT(newestTurnOf(rt::jsonParse(R"({ "data": [7, { "id": "turn-2", "status": null }] })")) == std::nullopt);
}

TEST("classifySupervisorState and sessionFromParts keep a source that answered nothing honestly unknown") {
  EXPECT(classifySupervisorState({std::nullopt, std::nullopt, false}) == SupervisorState::unknown);
  const SupervisorSource source{.id = "local", .name = "This Mac", .socketPath = "/var/lib/uos-supervisor/local.sock", .codexHome = "/home/tester/.codex"};
  const SupervisorSession session = sessionFromParts(SessionFromPartsInput{
    .id = "01a0bd67-no-probe",
    .source = source,
    .metadata = std::nullopt,
    .listed = std::nullopt,
    .probe = std::nullopt,
    .loaded = false,
    .sampledAtMs = 1'790'000'000'000,
    .childIds = std::nullopt,
    .unavailable = {"live status not sampled in this pass"},
  });
  EXPECT(session.state == SupervisorState::unknown);
  EXPECT_EQ(session.sampled, false);
  EXPECT(!session.childIds.has_value());
  EXPECT(!session.title.has_value());
  EXPECT(!session.titleSource.has_value());
  EXPECT(!session.tokensUsed.has_value());
  EXPECT(!session.usageSource.has_value());
  EXPECT(session.activeFlags.empty());
  EXPECT_EQ(session.unavailable, (std::vector<std::string>{"live status not sampled in this pass"}));
}

/* ----------------------------------------------- inventory: snapshot assembly */

ASYNC_TEST("ensureSupervisorSnapshot reports an empty inventory when no source can be resolved and caches the single sampling round") {
  // The kernel denies the existing configuration file and the sparse profile omits
  // HOME/CODEX_HOME: the honest
  // result is zero sources and zero sessions, never an invented local source.
  const ConfinedFixture fixture;
  const ScopedCwd cwd(fixture.cwd);
  const rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}};
  std::vector<rt::Promise<SupervisorSnapshot>> polls;
  polls.push_back(rt::Promise<SupervisorSnapshot>(ensureSupervisorSnapshot()));
  polls.push_back(rt::Promise<SupervisorSnapshot>(ensureSupervisorSnapshot()));
  const std::vector<SupervisorSnapshot> both = co_await rt::all(std::move(polls)).wait();
  ASSERT(both.size() == 2);
  const SupervisorSnapshot& first = both[0];
  EXPECT(first == both[1]);  // two polls that overlap must share one sampling round
  EXPECT_EQ(first.view, "codex_app_server_read_only");
  EXPECT(first.sources.empty());
  EXPECT(first.sessions.empty());
  EXPECT(first.counts == (SupervisorSnapshot::Counts{.total = 0, .active = 0, .waiting = 0, .idle = 0, .stale = 0, .unknown = 0, .systemError = 0}));
  EXPECT(first.coverage == (SupervisorSnapshot::Coverage{.listed = 0, .sampled = 0, .truncated = false, .notes = first.coverage.notes}));
  EXPECT(first.coverage.notes.size() >= 1);  // an empty inventory must explain why it is empty
  EXPECT(someNote(first.coverage.notes, "codex-supervisor.json"));  // the missing configuration file must be reported as a note

  auto again = ensureSupervisorSnapshot();
  const SupervisorSnapshot cached = co_await std::move(again);
  EXPECT(cached == first);  // a snapshot inside the short TTL is reused rather than resampled
  EXPECT_EQ(cached.sampledAtMs, first.sampledAtMs);
}

/* ------------------------------------------------------- supervisor-config */

TEST("uniqueStrings keeps first-seen order and readHostname never invents a hostname") {
  EXPECT_EQ(uniqueStrings({"b", "a", "b", "c", "a"}), (std::vector<std::string>{"b", "a", "c"}));
  EXPECT_EQ(uniqueStrings({}), std::vector<std::string>{});
  // Bun exposes the actual OS hostname independently of filesystem confinement.
  EXPECT_EQ(readHostname(), std::optional<std::string>(rt::os::hostname()));
  EXPECT_EQ(boundedText("short", 10), "short");
  EXPECT_EQ(boundedText("0123456789", 5), "01234…");
}

ASYNC_TEST("waitForPoll resolves on its own deadline and immediately on abort") {
  const double startedAt = rt::performanceNow();
  rt::Promise<void> poll = waitForPoll(25, rt::AbortController().signal());
  co_await poll.wait();
  EXPECT(rt::performanceNow() - startedAt >= 20);  // an unaborted poll waits for its deadline

  const rt::AbortController controller;
  const double abortedStart = rt::performanceNow();
  rt::Promise<void> pending = waitForPoll(5'000, controller.signal());
  controller.abort();
  co_await pending.wait();
  EXPECT(rt::performanceNow() - abortedStart < 1'000);  // an aborted poll must not wait out its deadline
}

ASYNC_TEST("mapWithConcurrency preserves result order and never exceeds its concurrency limit") {
  int active = 0;
  int peak = 0;
  const auto run = [&active, &peak](double item) -> rt::Task<double> {
    active += 1;
    peak = std::max(peak, active);
    co_await rt::sleep(1);
    active -= 1;
    co_return item * 2;
  };
  std::vector<double> items{1, 2, 3, 4, 5};
  rt::Task<std::vector<double>> mapping = mapWithConcurrency(std::move(items), 2, run);
  const std::vector<double> results = co_await std::move(mapping);
  EXPECT_EQ(results, (std::vector<double>{2, 4, 6, 8, 10}));
  EXPECT_EQ(peak, 2);  // the worker pool must stay at its limit

  const auto unused = [](double) { return rt::Promise<std::string>::resolve(std::string("unused")); };
  std::vector<double> none;
  rt::Task<std::vector<std::string>> mappingNone = mapWithConcurrency(std::move(none), 4, unused);
  const std::vector<std::string> empty = co_await std::move(mappingNone);
  EXPECT(empty.empty());
}

TEST("parseSupervisorConfig names every reason a source was ignored") {
  const std::string longName = rt::repeat("n", 65);
  json::object longSource{{"id", "long-name"}, {"name", longName}, {"socketPath", "/var/lib/uos-supervisor/b.sock"}};
  json::array sources = rt::jsonParse(R"([
    "not-an-object",
    { "id": "Local", "name": "Uppercase id", "socketPath": "/var/lib/uos-supervisor/a.sock" },
    null,
    { "id": "relative-home", "name": "Relative home", "socketPath": "/var/lib/uos-supervisor/c.sock", "codexHome": "relative/.codex" },
    { "id": "kept", "name": "Kept", "socketPath": "/var/lib/uos-supervisor/d.sock" }
  ])").as_array();
  sources[2] = longSource;
  json::object config;
  config["sources"] = sources;
  const ParsedSupervisorConfig parsed = parseSupervisorConfig(config);
  EXPECT_EQ(idsOf(parsed.sources), (std::vector<std::string>{"kept"}));
  EXPECT(std::find(parsed.notes.begin(), parsed.notes.end(), "ignored a source that is not an object") != parsed.notes.end());
  EXPECT(someNote(parsed.notes, "name must be 1-64 characters"));
  EXPECT(someNote(parsed.notes, "codexHome must be an absolute path"));
  EXPECT(someNote(parsed.notes, "socketPath must be an absolute path") || someNote(parsed.notes, "id must match"));
}

TEST("parseSupervisorConfig caps the usable source list and says so") {
  json::array sources;
  for (int index = 0; index < 9; ++index) {
    json::object source;
    source["id"] = "source-" + std::to_string(index);
    source["name"] = "Source " + std::to_string(index);
    source["socketPath"] = "/var/lib/uos-supervisor/source-" + std::to_string(index) + ".sock";
    sources.push_back(source);
  }
  json::object config;
  config["sources"] = sources;
  const ParsedSupervisorConfig parsed = parseSupervisorConfig(config);
  EXPECT_EQ(parsed.sources.size(), 8u);
  ASSERT(!parsed.sources.empty());
  EXPECT_EQ(parsed.sources.back().id, "source-7");
  EXPECT(std::find(parsed.notes.begin(), parsed.notes.end(), "only the first 8 sources are used") != parsed.notes.end());
}

ASYNC_TEST("resolveSupervisorConfig falls back to the local source and reports an unreadable configuration") {
  const ConfinedFixture fixture;
  const ResolveSupervisorConfigOptions missingOptions{.cwd = fixture.cwd, .env = [](std::string_view) -> std::optional<std::string> { return std::nullopt; }};
  rt::Task<SupervisorConfig> resolvingMissing = resolveSupervisorConfig(missingOptions);
  const SupervisorConfig missing = co_await std::move(resolvingMissing);
  EXPECT_EQ(missing.configLoaded, false);
  EXPECT(missing.sources.empty());  // no environment and no config file means no source at all
  EXPECT(someNote(missing.notes, ".data/codex-supervisor.json could not be read"));
  EXPECT(someNote(missing.notes, "HOME and CODEX_HOME are both unavailable"));
  EXPECT(rt::endsWith(missing.configPath, ".data/codex-supervisor.json"));

  const ResolveSupervisorConfigOptions homeOptions{
    .cwd = fixture.cwd,
    .env = [](std::string_view name) -> std::optional<std::string> { return name == "HOME" ? std::optional<std::string>("/home/tester/") : std::nullopt; },
  };
  rt::Task<SupervisorConfig> resolvingFromHome = resolveSupervisorConfig(homeOptions);
  const SupervisorConfig fromHome = co_await std::move(resolvingFromHome);
  const std::vector<SupervisorSource> expectedFromHome{{
    .id = "local",
    .name = "Local Codex",
    .socketPath = "/home/tester/.codex/app-server-control/app-server-control.sock",
    .codexHome = "/home/tester/.codex",
  }};
  EXPECT(fromHome.sources == expectedFromHome);
  EXPECT_EQ(fromHome.configLoaded, false);

  const ResolveSupervisorConfigOptions codexHomeOptions{
    .cwd = fixture.cwd,
    .env = [](std::string_view name) -> std::optional<std::string> { return name == "CODEX_HOME" ? "/custom/codex" : "/home/tester"; },
  };
  rt::Task<SupervisorConfig> resolvingFromCodexHome = resolveSupervisorConfig(codexHomeOptions);
  const SupervisorConfig fromCodexHome = co_await std::move(resolvingFromCodexHome);
  ASSERT(!fromCodexHome.sources.empty());
  EXPECT_EQ(fromCodexHome.sources[0].codexHome, std::optional<std::string>("/custom/codex"));
  EXPECT_EQ(fromCodexHome.sources[0].socketPath, "/custom/codex/app-server-control/app-server-control.sock");
}

TEST("parseSupervisorConfig reports a duplicate id and a relative socket path") {
  const ParsedSupervisorConfig parsed = parseSupervisorConfig(rt::jsonParse(R"({
    "sources": [
      { "id": "dupe", "name": "First", "socketPath": "/var/lib/uos-supervisor/a.sock" },
      { "id": "dupe", "name": "Second", "socketPath": "/var/lib/uos-supervisor/b.sock" },
      { "id": "relative", "name": "Relative", "socketPath": "app-server.sock" }
    ]
  })"));
  EXPECT_EQ(idsOf(parsed.sources), (std::vector<std::string>{"dupe"}));
  EXPECT(std::find(parsed.notes.begin(), parsed.notes.end(), "ignored duplicate source id dupe") != parsed.notes.end());
  EXPECT(std::find(parsed.notes.begin(), parsed.notes.end(), "ignored source relative: socketPath must be an absolute path") != parsed.notes.end());
}

/* ---------------------------------------------------- supervisor-transport */

namespace {
  namespace ws = uos::rt::ws;
  using uos::codex::supervisor_transport::openSupervisorConnection;
  using uos::codex::supervisor_transport::setSupervisorWebSocketConstructorForTest;
  using uos::codex::supervisor_transport::SupervisorCallResult;
  using uos::codex::supervisor_transport::SupervisorConnection;

  // What a responder returns: a result, an Error (sent as `{ id, error: { message } }`), or
  // DEFER (the test delivers the frame itself).
  struct RpcReply {
    enum class Kind { result, error, defer };
    Kind kind = Kind::result;
    json::value result;
    std::string message;
  };
  RpcReply replyWith(json::value result) { return RpcReply{RpcReply::Kind::result, std::move(result), {}}; }
  /** Defer the reply so a test can deliver the frame itself. */
  const RpcReply DEFER{RpcReply::Kind::defer, nullptr, {}};

  using RpcResponder = std::function<RpcReply(const std::string& method, const json::object& params, const json::object& frame)>;

  /**
   * Minimal structural stand-in for the `ws` client: it opens on a microtask and
   * answers JSON-RPC requests from a queued script, so the transport can be driven
   * end to end without a live socket.
   */
  class ScriptedSocket final : public ws::WebSocket {
  public:
    static std::vector<std::shared_ptr<ScriptedSocket>>& created() {
      static std::vector<std::shared_ptr<ScriptedSocket>> sockets;
      return sockets;
    }
    static std::deque<RpcResponder>& plans() {
      static std::deque<RpcResponder> queued;
      return queued;
    }

    std::vector<json::object> frames;
    std::string url;
    ws::ClientOptions options;
    int readyState_ = 1;
    bool terminated = false;
    // `{ code, reason }` once close() ran.
    std::optional<std::pair<std::optional<int>, std::optional<std::string>>> closeFrame;
    bool closeThrows = false;
    bool sendThrows = false;

    ScriptedSocket(std::string url_, ws::ClientOptions options_) : ws::WebSocket(nullptr), url(std::move(url_)), options(std::move(options_)) {
      if (plans().empty()) {
        responders_ = [](const std::string&, const json::object&, const json::object&) { return replyWith(json::object()); };
      } else {
        responders_ = plans().front();
        plans().pop_front();
      }
    }

    // The constructor as the seam calls it: `queueMicrotask(() => this.emit("open"))`.
    static ws::WebSocketConstructor constructor() {
      return [](std::string url, ws::ClientOptions options) -> std::shared_ptr<ws::WebSocket> {
        auto socket = std::make_shared<ScriptedSocket>(std::move(url), std::move(options));
        created().push_back(socket);
        rt::queueMicrotask([socket] { socket->emit("open"); });
        return socket;
      };
    }

    static void plan(RpcResponder responder) { plans().push_back(std::move(responder)); }

    static std::shared_ptr<ScriptedSocket> latest() {
      if (created().empty()) {
        EXPECT(false);  // the transport must construct a socket
        throw std::logic_error("no socket");
      }
      return created().back();
    }

    static void reset() {
      created().clear();
      plans().clear();
    }

    using ws::WebSocket::emit;
    EventEmitter& on(std::string_view event, ws::Listener listener) override {
      add(event, std::move(listener), false);
      return *this;
    }
    EventEmitter& once(std::string_view event, ws::Listener listener) override {
      add(event, std::move(listener), true);
      return *this;
    }
    int readyState() const override { return readyState_; }

    void send(ws::Data data, ws::SendOptions, ws::Callback) override {
      if (sendThrows) throw rt::JsError("socket is gone");
      const json::object frame = rt::jsonParse(std::get<std::string>(data)).as_object();
      frames.push_back(frame);
      const json::value* id = rt::get(frame, "id");
      const json::value* method = rt::get(frame, "method");
      if (id == nullptr || !rt::isNumber(*id) || method == nullptr || !method->is_string()) return;
      const json::value* params = rt::get(frame, "params");
      const RpcReply reply = responders_(std::string(method->as_string()), params && params->is_object() ? params->as_object() : json::object(), frame);
      if (reply.kind == RpcReply::Kind::defer) return;
      auto self = std::static_pointer_cast<ScriptedSocket>(shared_from_this());
      const double callId = rt::toNumber(id);
      rt::queueMicrotask([self, callId, reply] { self->replyTo(callId, reply); });
    }

    void close(std::optional<int> code, std::optional<ws::Data> reason) override {
      if (closeThrows) throw rt::JsError("close failed");
      closeFrame = std::make_pair(code, reason ? std::optional<std::string>(std::get<std::string>(*reason)) : std::nullopt);
      readyState_ = 3;
      emit("close");
    }

    void terminate() override {
      terminated = true;
      readyState_ = 3;
    }

    bool emit(std::string_view event, const ws::EventArgs& args) override {
      const std::vector<Entry> entries = listeners_[std::string(event)];
      std::vector<Entry> remaining;
      for (const Entry& entry : entries) {
        entry.listener(args);
        if (!entry.once) remaining.push_back(entry);
      }
      listeners_[std::string(event)] = remaining;
      return !entries.empty();
    }

    /** Delivers one websocket message frame exactly as the `ws` client would. */
    void emitMessage(ws::Data payload) {
      ws::EventArgs args;
      args.data = std::move(payload);
      emit("message", args);
    }

  private:
    struct Entry {
      ws::Listener listener;
      bool once = false;
    };

    void replyTo(double id, const RpcReply& reply) {
      if (terminated) return;
      json::object frame;
      frame["id"] = rt::makeNumber(id);
      if (reply.kind == RpcReply::Kind::error) frame["error"] = json::object{{"message", reply.message}};
      else frame["result"] = reply.result;
      emitMessage(rt::encodeUtf8(rt::jsonStringify(json::value(frame))));
    }

    void add(std::string_view event, ws::Listener listener, bool once) { listeners_[std::string(event)].push_back(Entry{std::move(listener), once}); }

    RpcResponder responders_;
    std::map<std::string, std::vector<Entry>> listeners_;
  };

  // withScriptedSockets(run): the seam installed for the scope, reset afterwards.
  struct ScriptedSockets {
    ScriptedSockets() {
      ScriptedSocket::reset();
      setSupervisorWebSocketConstructorForTest(ScriptedSocket::constructor());
    }
    ~ScriptedSockets() {
      setSupervisorWebSocketConstructorForTest(nullptr);
      ScriptedSocket::reset();
    }
    ScriptedSockets(const ScriptedSockets&) = delete;
    ScriptedSockets& operator=(const ScriptedSockets&) = delete;
  };

  rt::AbortSignal deferredSignal() { return rt::AbortController().signal(); }

  json::object threadParams() { return json::object{{"threadId", "thread-1"}}; }

  // `assert.rejects(task, /pattern/)`: the rejection's message, or nullopt when it resolved.
  template <class T> rt::Task<std::optional<std::string>> rejectionOf(rt::Task<T> task) {
    rt::Reason failure;
    try {
      co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    if (!failure) co_return std::nullopt;
    co_return rt::errorMessage(failure).value_or(rt::reasonToString(failure));
  }

  bool matches(const std::optional<std::string>& message, const std::string& pattern) { return message && rt::Regex(pattern, "").test(*message); }

  std::vector<std::string> frameMethods(const ScriptedSocket& socket) {
    std::vector<std::string> methods;
    for (const json::object& frame : socket.frames) {
      const json::value* method = rt::get(frame, "method");
      methods.push_back(method && method->is_string() ? std::string(method->as_string()) : "<none>");
    }
    return methods;
  }

  json::value lastFrameId(const ScriptedSocket& socket) {
    const json::value* id = socket.frames.empty() ? nullptr : rt::get(socket.frames.back(), "id");
    return id ? *id : json::value(nullptr);
  }
}

ASYNC_TEST("the transport handshakes, targets the configured socket and resolves read-only calls") {
  const ScriptedSockets scripted;
  ScriptedSocket::plan([](const std::string& method, const json::object&, const json::object&) {
    if (method == "thread/read") return replyWith(rt::jsonParse(R"({ "thread": { "id": "thread-1", "name": "Covered" } })"));
    return replyWith(json::object());
  });
  auto opening = openSupervisorConnection("/var/lib/uos-supervisor/scripted.sock");
  const SupervisorConnection connection = co_await std::move(opening);
  const std::shared_ptr<ScriptedSocket> socket = ScriptedSocket::latest();
  EXPECT_EQ(socket->url, "ws+unix:///var/lib/uos-supervisor/scripted.sock:/");
  EXPECT(socket->options.headers == (std::vector<std::pair<std::string, std::string>>{{"Host", "localhost"}}));
  EXPECT_EQ(socket->options.perMessageDeflate, false);
  EXPECT_EQ(socket->options.maxPayload, 1'048'576.0);
  EXPECT_EQ(socket->options.handshakeTimeout, std::optional<double>(3'000));

  auto calling = connection.call("thread/read", threadParams(), deferredSignal());
  const SupervisorCallResult result = co_await std::move(calling);
  ASSERT(result.has_value());
  EXPECT(rt::deepEqual(*result, rt::jsonParse(R"({ "thread": { "id": "thread-1", "name": "Covered" } })")));
  // only the handshake and the requested read method are ever sent
  EXPECT_EQ(frameMethods(*socket), (std::vector<std::string>{"initialize", "initialized", "thread/read"}));
  EXPECT(rt::deepEqual(*rt::get(socket->frames[0], "params"), rt::jsonParse(R"({ "clientInfo": { "name": "uos_supervisor", "version": "1" }, "capabilities": { "experimentalApi": true } })")));
  const std::size_t sentCount = socket->frames.size();

  auto refused = connection.call("thread/archive", threadParams(), std::nullopt);
  const std::optional<std::string> refusal = co_await rejectionOf(std::move(refused));
  EXPECT(matches(refusal, "refused a non-read-only method"));
  EXPECT_EQ(socket->frames.size(), sentCount);  // a refused method must never reach the socket
  auto closing = connection.close();
  co_await std::move(closing);
}

ASYNC_TEST("the transport closes cleanly: close frame, termination, failed pending calls and a no-op second close") {
  const ScriptedSockets scripted;
  ScriptedSocket::plan([](const std::string& method, const json::object&, const json::object&) { return method == "initialize" ? replyWith(json::object()) : DEFER; });
  auto opening = openSupervisorConnection("/var/lib/uos-supervisor/closing.sock");
  const SupervisorConnection connection = co_await std::move(opening);
  const std::shared_ptr<ScriptedSocket> socket = ScriptedSocket::latest();
  const rt::Promise<std::optional<std::string>> rejected(rejectionOf(connection.call("thread/read", threadParams(), deferredSignal())));
  auto closing = connection.close();
  co_await std::move(closing);
  const std::optional<std::string> rejection = co_await rejected.wait();
  EXPECT(matches(rejection, "app-server connection closed"));
  ASSERT(socket->closeFrame.has_value());
  EXPECT_EQ(socket->closeFrame->first, std::optional<int>(1000));
  EXPECT_EQ(socket->closeFrame->second, std::optional<std::string>("supervisor sample complete"));
  EXPECT_EQ(socket->terminated, true);  // the grace period must end in a terminated socket

  const std::size_t framesAfterClose = socket->frames.size();
  auto closingAgain = connection.close();
  co_await std::move(closingAgain);
  EXPECT_EQ(socket->frames.size(), framesAfterClose);  // a second close must not send anything
}

ASYNC_TEST("a close whose frame fails is still terminated") {
  const ScriptedSockets scripted;
  ScriptedSocket::plan([](const std::string&, const json::object&, const json::object&) { return replyWith(json::object()); });
  auto opening = openSupervisorConnection("/var/lib/uos-supervisor/close-throws.sock");
  const SupervisorConnection connection = co_await std::move(opening);
  const std::shared_ptr<ScriptedSocket> socket = ScriptedSocket::latest();
  socket->closeThrows = true;
  auto closing = connection.close();
  co_await std::move(closing);
  EXPECT_EQ(socket->terminated, true);  // the close frame is best effort, not the guarantee
}

ASYNC_TEST("a socket that already reported itself closed is not closed twice") {
  const ScriptedSockets scripted;
  ScriptedSocket::plan([](const std::string&, const json::object&, const json::object&) { return replyWith(json::object()); });
  auto opening = openSupervisorConnection("/var/lib/uos-supervisor/already-closed.sock");
  const SupervisorConnection connection = co_await std::move(opening);
  const std::shared_ptr<ScriptedSocket> socket = ScriptedSocket::latest();
  socket->readyState_ = 3;
  auto closing = connection.close();
  co_await std::move(closing);
  EXPECT(!socket->closeFrame.has_value());  // no close frame is sent to a socket that is already closed
  EXPECT_EQ(socket->terminated, false);
}

ASYNC_TEST("calls fail fast on a closed socket, an aborted signal and a send failure") {
  const ScriptedSockets scripted;
  ScriptedSocket::plan([](const std::string&, const json::object&, const json::object&) { return replyWith(json::object()); });
  auto opening = openSupervisorConnection("/var/lib/uos-supervisor/fail-fast.sock");
  const SupervisorConnection connection = co_await std::move(opening);
  const std::shared_ptr<ScriptedSocket> socket = ScriptedSocket::latest();

  const rt::AbortController controller;
  controller.abort();
  auto aborted = connection.call("thread/read", threadParams(), controller.signal());
  const std::optional<std::string> abortedMessage = co_await rejectionOf(std::move(aborted));
  EXPECT(matches(abortedMessage, "app-server call aborted"));

  const rt::Promise<std::optional<std::string>> pending(rejectionOf(connection.call("thread/read", threadParams(), deferredSignal())));
  socket->emit("error");
  const std::optional<std::string> failed = co_await pending.wait();
  EXPECT(matches(failed, "app-server connection failed"));

  socket->sendThrows = true;
  auto sendFailure = connection.call("thread/read", threadParams(), deferredSignal());
  const std::optional<std::string> sendFailed = co_await rejectionOf(std::move(sendFailure));
  EXPECT(matches(sendFailed, "app-server send failed"));
  socket->sendThrows = false;

  socket->readyState_ = 3;
  auto unavailable = connection.call("thread/read", threadParams(), deferredSignal());
  const std::optional<std::string> unavailableMessage = co_await rejectionOf(std::move(unavailable));
  EXPECT(matches(unavailableMessage, "app-server connection unavailable"));
}

ASYNC_TEST("an aborted call signal rejects the exact pending call") {
  const ScriptedSockets scripted;
  ScriptedSocket::plan([](const std::string& method, const json::object&, const json::object&) { return method == "initialize" ? replyWith(json::object()) : DEFER; });
  auto opening = openSupervisorConnection("/var/lib/uos-supervisor/abort-pending.sock");
  const SupervisorConnection connection = co_await std::move(opening);
  const rt::AbortController controller;
  const rt::Promise<std::optional<std::string>> pending(rejectionOf(connection.call("thread/read", threadParams(), controller.signal())));
  controller.abort();
  const std::optional<std::string> message = co_await pending.wait();
  EXPECT(matches(message, "app-server call aborted"));
}

TEST("an abandoned supervisor call releases the control socket") {
  const rt::AbortController caller;
  std::weak_ptr<ScriptedSocket> socket;
  std::weak_ptr<rt::detail::AbortSignalState> signalState;
  {
    rt::FakeClock clock;
    rt::Loop loop(clock);
    const rt::LoopScope scope(loop);
    const ScriptedSockets scripted;
    ScriptedSocket::plan([](const std::string& method, const json::object&, const json::object&) {
      return method == "initialize" ? replyWith(json::object()) : DEFER;
    });
    auto opening = openSupervisorConnection("/var/lib/uos-supervisor/abandoned.sock");
    const auto connection = rt::blockOn(loop, std::move(opening));
    socket = ScriptedSocket::latest();
    const auto signal = rt::AbortSignal::any({caller.signal()});
    signalState = signal.state();
    const rt::Promise<SupervisorCallResult> pending(connection.call("thread/read", threadParams(), signal));
    EXPECT(pending.pending());
    EXPECT_EQ(frameMethods(*ScriptedSocket::latest()).back(), "thread/read");
  }
  EXPECT(socket.expired());
  EXPECT(signalState.expired());
}

ASYNC_TEST("an aborted sampling signal never opens a socket") {
  const ScriptedSockets scripted;
  const rt::AbortController controller;
  controller.abort();
  auto opening = openSupervisorConnection("/var/lib/uos-supervisor/never.sock", controller.signal());
  const std::optional<std::string> message = co_await rejectionOf(std::move(opening));
  EXPECT(matches(message, "app-server connection aborted"));
  EXPECT_EQ(ScriptedSocket::created().size(), 0u);  // no socket is constructed for an aborted attempt
}

ASYNC_TEST("message frames are decoded as text, bytes or multipart arrays and malformed frames are ignored") {
  const ScriptedSockets scripted;
  ScriptedSocket::plan([](const std::string& method, const json::object&, const json::object&) { return method == "initialize" ? replyWith(json::object()) : DEFER; });
  auto opening = openSupervisorConnection("/var/lib/uos-supervisor/frames.sock");
  const SupervisorConnection connection = co_await std::move(opening);
  const std::shared_ptr<ScriptedSocket> socket = ScriptedSocket::latest();
  const rt::Promise<SupervisorCallResult> pending(connection.call("thread/read", threadParams(), deferredSignal()));
  const json::value requestId = lastFrameId(*socket);
  EXPECT(rt::isNumber(requestId));

  // `socket.emitMessage(7)` has no rt::ws form (see the file comment).
  socket->emitMessage(std::string("not json at all"));
  socket->emitMessage(rt::jsonStringify(json::value("a string, not an object")));
  socket->emitMessage(rt::jsonStringify(rt::jsonParse(R"({ "id": 99999, "result": "unknown call" })")));
  socket->emitMessage(rt::jsonStringify(rt::jsonParse(R"({ "id": "not-a-number", "result": "bad id" })")));
  json::object reply;
  reply["id"] = requestId;
  reply["result"] = rt::jsonParse(R"({ "thread": { "id": "thread-1" } })");
  socket->emitMessage(rt::encodeUtf8(rt::jsonStringify(json::value(reply))));
  const SupervisorCallResult resolved = co_await pending.wait();
  ASSERT(resolved.has_value());
  // a byte payload carrying a valid reply resolves the call
  EXPECT(rt::deepEqual(*resolved, rt::jsonParse(R"({ "thread": { "id": "thread-1" } })")));
  // The multipart (Buffer[]) frame of the TS case has no rt::ws form (see the file comment).
}

ASYNC_TEST("an error frame rejects the pending call with the bounded upstream message") {
  const ScriptedSockets scripted;
  ScriptedSocket::plan([](const std::string& method, const json::object&, const json::object&) { return method == "initialize" ? replyWith(json::object()) : DEFER; });
  auto opening = openSupervisorConnection("/var/lib/uos-supervisor/errors.sock");
  const SupervisorConnection connection = co_await std::move(opening);
  const std::shared_ptr<ScriptedSocket> socket = ScriptedSocket::latest();

  const rt::Promise<std::optional<std::string>> described(rejectionOf(connection.call("thread/read", threadParams(), deferredSignal())));
  json::object bounded;
  bounded["id"] = lastFrameId(*socket);
  bounded["error"] = json::object{{"message", rt::repeat("x", 500)}};
  socket->emitMessage(rt::jsonStringify(json::value(bounded)));
  const std::optional<std::string> describedMessage = co_await described.wait();
  ASSERT(describedMessage.has_value());
  EXPECT_EQ(rt::length(*describedMessage), std::int64_t(400));  // an upstream message is bounded before it reaches a caller

  const rt::Promise<std::optional<std::string>> undescribed(rejectionOf(connection.call("thread/read", threadParams(), deferredSignal())));
  json::object codeOnly;
  codeOnly["id"] = lastFrameId(*socket);
  codeOnly["error"] = json::object{{"code", -32'000}};
  socket->emitMessage(rt::jsonStringify(json::value(codeOnly)));
  const std::optional<std::string> undescribedMessage = co_await undescribed.wait();
  EXPECT(matches(undescribedMessage, "app-server returned an error"));
}

/* ---------------------------------------------------------- supervisor-brief */

namespace {
  using uos::codex::supervisor_brief::appendRolloutTailTurn;
  using uos::codex::supervisor_brief::AppendRolloutTailTurnInput;
  using uos::codex::supervisor_brief::assembleBriefContext;
  using uos::codex::supervisor_brief::BriefAssembly;
  using uos::codex::supervisor_brief::BriefTranscriptItem;
  using uos::codex::supervisor_brief::BriefTranscriptTurn;
  using uos::codex::supervisor_brief::buildSupervisorBriefPrompt;
  using uos::codex::supervisor_brief::buildSupervisorBriefRequestBody;
  using uos::codex::supervisor_brief::collectSupervisorBriefContext;
  using uos::codex::supervisor_brief::handleAdminCodexSupervisorBrief;
  using uos::codex::supervisor_brief::parseSupervisorBriefOutput;
  using uos::codex::supervisor_brief::SupervisorBriefContext;
  using uos::codex::supervisor_brief::SupervisorBriefOutput;
  using uos::codex::supervisor_log::SupervisorLogEvent;

  // No provider key and no credential home for the brief cases' scope.
  struct BriefEnvironment {
    rt::env::ScopedOverride environment{{"CEREBRAS_API_KEY", std::nullopt}, {"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
  };

  RpcReply replyError(std::string message) { return RpcReply{RpcReply::Kind::error, nullptr, std::move(message)}; }

  std::vector<std::string> turnIdsOf(const SupervisorBriefContext& context) {
    std::vector<std::string> ids;
    for (const BriefTranscriptTurn& turn : context.turns) ids.push_back(turn.id);
    return ids;
  }

  std::vector<std::string> itemTextsOf(const BriefTranscriptTurn& turn) {
    std::vector<std::string> out;
    for (const BriefTranscriptItem& item : turn.items) out.push_back(item.text);
    return out;
  }

  const BriefTranscriptTurn* turnWithId(const std::vector<BriefTranscriptTurn>& turns, std::string_view id) {
    for (const BriefTranscriptTurn& turn : turns)
      if (turn.id == id) return &turn;
    return nullptr;
  }

  bool promptMatches(const std::string& prompt, const char* pattern) { return rt::Regex(pattern, "").test(prompt); }

  // `params.itemsView === view && params.sortDirection === direction`
  bool turnsListIs(const json::object& params, std::string_view view, std::string_view direction) {
    return rt::getString(params, "itemsView") == view && rt::getString(params, "sortDirection") == direction;
  }

  rt::Request briefRequest(std::string body) {
    rt::RequestInit init;
    init.method = "POST";
    init.body = rt::BodyInit(std::move(body));
    init.headers = rt::Headers{{"Content-Type", "application/json"}};
    return rt::Request("https://ai.ubq.fi/admin/codex/supervisor/brief", std::move(init));
  }

  rt::Task<std::string> errorMessageOf(rt::Response response) {
    auto reading = response.json();
    const json::value payload = co_await std::move(reading);
    const json::value* error = rt::get(payload, "error");
    const std::optional<std::string_view> message = error ? rt::getString(*error, "message") : std::nullopt;
    co_return message ? std::string(*message) : std::string("<no message>");
  }
}

ASYNC_TEST("collectSupervisorBriefContext reads one redacted transcript through the read-only allowlist") {
  const BriefEnvironment environment;
  const ScriptedSockets scripted;
  const json::value thread = rt::jsonParse(R"({"id":"thread-1","name":"Coverage work","cwd":"/repo","model":"gpt-6-astra","modelProvider":"uos","reasoningEffort":"high",
    "status":{"type":"active","activeFlags":["waitingOnApproval",7]},"updatedAt":1790001000,"path":"/rollout.jsonl"})");
  ScriptedSocket::plan([thread](const std::string& method, const json::object& params, const json::object&) {
    if (method == "thread/read") return replyWith(json::object{{"thread", thread}});
    if (method != "thread/turns/list") return replyWith(json::object());
    if (turnsListIs(params, "summary", "asc")) {
      return replyWith(rt::jsonParse(R"({"data":[{"id":"turn-1","status":"completed","startedAt":1790000000,"completedAt":1790000100,"items":[
        {"type":"userMessage","content":[{"type":"text","text":"opening request"},{"type":"image","url":"ignored"},null,7]},
        {"type":"mcpToolCall","name":"search","status":"completed"},
        {"type":"mcpToolCall","server":"filesystem"},
        {"type":"fileChange"}]}]})"));
    }
    return replyWith(rt::jsonParse(R"({"data":[{"id":"turn-2","status":"inProgress","startedAt":1790000500,"items":[{"type":"agentMessage","text":"recent progress"}]}]})"));
  });

  const SupervisorSource source{.id = "vps", .name = "vps.pavelcik.com", .socketPath = "/var/lib/uos-supervisor/brief.sock", .codexHome = std::nullopt};
  auto collecting = collectSupervisorBriefContext(source, "thread-1", deferredSignal());
  const SupervisorBriefContext context = co_await std::move(collecting);
  EXPECT_EQ(context.sourceId, "vps");
  EXPECT_EQ(context.machine, "vps.pavelcik.com");
  EXPECT_EQ(context.threadId, "thread-1");
  EXPECT_EQ(context.title, std::optional<std::string>("Coverage work"));
  EXPECT_EQ(context.cwd, std::optional<std::string>("/repo"));
  EXPECT_EQ(context.model, std::optional<std::string>("gpt-6-astra"));
  EXPECT_EQ(context.provider, std::optional<std::string>("uos"));
  EXPECT_EQ(context.effort, std::optional<std::string>("high"));
  EXPECT_EQ(context.state, std::optional<std::string>("active"));
  EXPECT_EQ(context.activeFlags, std::vector<std::string>{"waitingOnApproval"});  // non-string active flags are dropped
  EXPECT_EQ(context.updatedAtMs, std::optional<double>(1'790'001'000'000));
  EXPECT_EQ(context.rolloutPath, std::optional<std::string>("/rollout.jsonl"));
  EXPECT_EQ(turnIdsOf(context), (std::vector<std::string>{"turn-1", "turn-2"}));  // page order alone produces a chronological transcript
  EXPECT_EQ(context.transcriptAvailable, true);
  EXPECT_EQ(context.truncated, false);
  EXPECT_EQ(context.rolloutTailEvents, std::int64_t{0});  // a source without a local codex home has no rollout tail
  EXPECT_EQ(context.projectedHistoryBehindMs, std::optional<double>(500'000));  // the fresh metadata leads the projected history by the recorded gap

  const std::string prompt = buildSupervisorBriefPrompt(context);
  EXPECT(promptMatches(prompt, "opening request"));
  EXPECT(promptMatches(prompt, "mcpToolCall search completed"));
  EXPECT(promptMatches(prompt, "mcpToolCall filesystem"));  // a tool item without a name falls back to its server
  EXPECT(promptMatches(prompt, "recent progress"));
  EXPECT(promptMatches(prompt, "runtime_state=active"));
  EXPECT(promptMatches(prompt, "active_flags=waitingOnApproval"));
  EXPECT(!promptMatches(prompt, "ignored"));  // non-text user-message parts never reach the prompt
}

ASYNC_TEST("collectSupervisorBriefContext tolerates unusable pages and skips malformed turns and items") {
  const BriefEnvironment environment;
  const ScriptedSockets scripted;
  ScriptedSocket::plan([](const std::string& method, const json::object& params, const json::object&) {
    if (method == "thread/read") return replyWith(rt::jsonParse(R"({"thread":{"id":"thread-1","status":{"type":"idle"}}})"));
    if (method != "thread/turns/list") return replyWith(json::object());
    if (turnsListIs(params, "summary", "asc")) return replyWith(json::object{{"data", "not-an-array"}});
    if (turnsListIs(params, "summary", "desc")) {
      return replyWith(rt::jsonParse(R"({"data":["not-a-turn",
        {"status":"completed","items":[{"type":"agentMessage","text":"no id, ignored"}]},
        {"id":"turn-2","status":"inProgress","items":[null,{"type":"reasoning","text":"private"},{"type":"agentMessage","text":7},
          {"type":"agentMessage","text":"visible agent message"},
          {"type":"commandExecution","command":null,"status":null,"exitCode":"1","aggregatedOutput":"out"}]}]})"));
    }
    return replyWith(json::object{{"data", json::array{}}});
  });

  const SupervisorSource source{.id = "vps", .name = "vps", .socketPath = "/var/lib/uos-supervisor/malformed.sock", .codexHome = std::nullopt};
  auto collecting = collectSupervisorBriefContext(source, "thread-1", deferredSignal());
  const SupervisorBriefContext context = co_await std::move(collecting);
  EXPECT_EQ(turnIdsOf(context), std::vector<std::string>{"turn-2"});  // a page without an array, a turn without an id and non-record items are skipped
  EXPECT_EQ(context.transcriptAvailable, true);
  const std::string prompt = buildSupervisorBriefPrompt(context);
  EXPECT(promptMatches(prompt, "visible agent message"));
  EXPECT(promptMatches(prompt, "\\(command unavailable\\) \\[recorded\\]\\nout"));  // a command without a recorded command or status stays explicit
  EXPECT(!promptMatches(prompt, "no id, ignored"));
  EXPECT(!promptMatches(prompt, "private"));
}

TEST("assembleBriefContext spends the budget newest-first, drops content-free turns and reports truncation") {
  const BriefEnvironment environment;
  const auto item = [](std::string text) { return BriefTranscriptItem{.type = "assistant", .text = std::move(text)}; };
  const auto turnOf = [](std::string id, std::vector<BriefTranscriptItem> items, std::int64_t droppedItems = 0, double startedAtMs = 1'790'000'000) {
    return BriefTranscriptTurn{
      .id = std::move(id),
      .status = "completed",
      .startedAtMs = startedAtMs,
      .completedAtMs = std::nullopt,
      .items = std::move(items),
      .droppedItems = droppedItems,
    };
  };

  // Recent turns are allocated newest-first, so the newest progress keeps the
  // budget and the opening turn is dropped once the remaining budget is smaller
  // than a single bounded item.
  const auto heavy = [&](std::string id, double startedAtMs) { return turnOf(std::move(id), std::vector<BriefTranscriptItem>(12, item(std::string(1'200, 'h'))), 0, startedAtMs); };
  const BriefAssembly assembled = assembleBriefContext({
    heavy("turn-opening", 1'790'000'000),
    heavy("turn-recent-3", 1'790'000'300),
    heavy("turn-recent-2", 1'790'000'600),
    heavy("turn-recent-1", 1'790'000'900),
  });
  std::vector<std::string> ids;
  for (const BriefTranscriptTurn& turn : assembled.turns) ids.push_back(turn.id);
  EXPECT_EQ(ids, (std::vector<std::string>{"turn-recent-3", "turn-recent-2", "turn-recent-1"}));  // the newest turns keep the budget and the opening turn is omitted rather than half-kept
  ASSERT(!assembled.turns.empty());
  const BriefTranscriptTurn& partial = assembled.turns[0];
  EXPECT(!partial.items.empty() && partial.items.size() < 12);  // a turn is kept only with the items that still fit
  EXPECT_EQ(assembled.truncated, true);
  EXPECT_EQ(assembled.transcriptAvailable, true);
  EXPECT(assembled.contextBytes <= 32 * 1024);  // the assembled context stays inside its byte budget

  const BriefAssembly flagged = assembleBriefContext({turnOf("turn-a", {item("kept")}, 3)});
  EXPECT_EQ(flagged.truncated, true);  // item-limit drops recorded upstream still mark the brief truncated

  const BriefAssembly contentFree = assembleBriefContext({turnOf("turn-empty", {})});
  EXPECT(contentFree.turns.empty());  // a turn with no items contributes nothing
  EXPECT_EQ(contentFree.transcriptAvailable, false);
  EXPECT_EQ(contentFree.truncated, true);  // an omitted turn still marks the transcript as incomplete

  const BriefAssembly redaction = assembleBriefContext({turnOf("turn-secret", {item("token=Bearer abcdefghijklmnop"), item("second")})});
  EXPECT_EQ(redaction.redactions, std::int64_t{2});  // both credential-shaped spans (the bearer value and the token assignment) are counted
  ASSERT_EQ(redaction.turns.size(), std::size_t{1});
  EXPECT(!promptMatches(rt::join(itemTextsOf(redaction.turns[0]), "\n"), "abcdefghijklmnop"));
}

TEST("appendRolloutTailTurn drops stale, duplicate and empty events and keeps the newest items") {
  const BriefEnvironment environment;
  const std::vector<BriefTranscriptTurn> turns{
    BriefTranscriptTurn{
      .id = "turn-1",
      .status = "completed",
      .startedAtMs = 1'000,
      .completedAtMs = 2'000,
      .items = {{.type = "assistant", .text = "known text"}},
      .droppedItems = 0,
    },
  };
  const std::vector<SupervisorLogEvent> events{
    {.atMs = 1'500, .kind = "assistant", .text = "stale event"},
    {.atMs = 2'100, .kind = "assistant", .text = "known text"},
    {.atMs = 2'200, .kind = "assistant", .text = "   "},
    {.atMs = 2'300, .kind = "tool", .text = std::string(500, 't')},
    {.atMs = 2'400, .kind = "assistant", .text = std::string(1'300, 'x')},
  };
  const std::vector<BriefTranscriptTurn> next = appendRolloutTailTurn(turns, AppendRolloutTailTurnInput{.threadId = "thread-1", .state = "inProgress", .events = events});
  ASSERT_EQ(next.size(), std::size_t{2});
  const BriefTranscriptTurn& tail = next[1];
  EXPECT(tail.freshTail == true);
  EXPECT_EQ(tail.id, "rollout-tail:thread-1");
  EXPECT_EQ(tail.status, std::optional<std::string>("inProgress"));
  EXPECT_EQ(tail.startedAtMs, std::optional<double>(2'300));
  EXPECT_EQ(tail.completedAtMs, std::optional<double>(2'400));
  EXPECT_EQ(tail.droppedItems, std::int64_t{2});  // a duplicate and a blank-after-trim event are counted while the stale event is skipped
  std::vector<std::string> types;
  for (const BriefTranscriptItem& entry : tail.items) types.push_back(entry.type);
  EXPECT_EQ(types, (std::vector<std::string>{"tool", "assistant"}));
  ASSERT_EQ(tail.items.size(), std::size_t{2});
  EXPECT_EQ(rt::length(tail.items[0].text), std::int64_t{400});  // a tool tail item is bounded to its own smaller limit
  EXPECT(rt::endsWith(tail.items[0].text, "…"));
  EXPECT_EQ(rt::length(tail.items[1].text), std::int64_t{1'200});
  EXPECT(rt::endsWith(tail.items[1].text, "…"));

  const std::vector<BriefTranscriptTurn> allStale =
    appendRolloutTailTurn(turns, AppendRolloutTailTurnInput{.threadId = "thread-1", .state = std::nullopt, .events = {{.atMs = 500, .kind = "assistant", .text = "old"}}});
  EXPECT(allStale == turns);  // events older than the projected history never create a tail turn

  std::vector<SupervisorLogEvent> many;
  for (int index = 0; index < 14; index += 1) many.push_back({.atMs = 2'500.0 + index, .kind = "assistant", .text = "event " + std::to_string(index)});
  const std::vector<BriefTranscriptTurn> capped = appendRolloutTailTurn(turns, AppendRolloutTailTurnInput{.threadId = "thread-1", .state = std::nullopt, .events = many});
  ASSERT_EQ(capped.size(), std::size_t{2});
  const BriefTranscriptTurn& cappedTail = capped[1];
  ASSERT_EQ(cappedTail.items.size(), std::size_t{12});  // the tail turn keeps at most one per-turn item budget
  EXPECT_EQ(cappedTail.items[0].text, "event 2");
  EXPECT_EQ(cappedTail.items.back().text, "event 13");  // the newest events win the cap
  EXPECT_EQ(cappedTail.droppedItems, std::int64_t{2});
}

TEST("parseSupervisorBriefOutput accepts only a usable two-field JSON object") {
  const BriefEnvironment environment;
  const auto output = [](std::string about, std::string status) { return std::optional<SupervisorBriefOutput>(SupervisorBriefOutput{.about = std::move(about), .status = std::move(status)}); };
  EXPECT(parseSupervisorBriefOutput(json::value(R"({"about":"a","status":"b"})")) == output("a", "b"));
  EXPECT(parseSupervisorBriefOutput(json::value("```json\n{\"about\":\"a\",\"status\":\"b\"}\n```")) == output("a", "b"));
  EXPECT(parseSupervisorBriefOutput(json::value(R"({"about":"  spaced \n out ","status":"ok"})")) == output("spaced out", "ok"));
  const std::optional<SupervisorBriefOutput> bounded =
    parseSupervisorBriefOutput(json::value(rt::jsonStringify(json::value(json::object{{"about", std::string(1'300, 'a')}, {"status", "ok"}}))));
  ASSERT(bounded.has_value());
  EXPECT_EQ(rt::length(bounded->about), std::int64_t{1'200});
  EXPECT(rt::endsWith(bounded->about, "…"));
  EXPECT(!parseSupervisorBriefOutput(json::value("not json")).has_value());
  EXPECT(!parseSupervisorBriefOutput(json::value("[1,2]")).has_value());
  EXPECT(!parseSupervisorBriefOutput(json::value(R"({"about":"a"})")).has_value());
  EXPECT(!parseSupervisorBriefOutput(json::value(R"({"about":"   ","status":"b"})")).has_value());
  EXPECT(!parseSupervisorBriefOutput(json::value(nullptr)).has_value());
  EXPECT(!parseSupervisorBriefOutput(json::value("")).has_value());
}

TEST("buildSupervisorBriefRequestBody bounds the prompt, names unavailable metadata and keeps the model contract") {
  const BriefEnvironment environment;
  SupervisorBriefContext emptyContext{};
  emptyContext.sourceId = "vps";
  emptyContext.machine = "vps.pavelcik.com";
  emptyContext.threadId = "thread-1";
  const std::string prompt = buildSupervisorBriefPrompt(emptyContext);
  EXPECT(promptMatches(prompt, "\\(no recorded turns were available\\)"));
  EXPECT(promptMatches(prompt, "title=unavailable"));
  EXPECT(promptMatches(prompt, "runtime_state=unavailable"));
  EXPECT(promptMatches(prompt, "active_flags=none"));
  EXPECT(promptMatches(prompt, "projected_history_behind_ms=unknown"));
  const json::object body = buildSupervisorBriefRequestBody(emptyContext);
  EXPECT(body.at("stream") == false);
  EXPECT(rt::deepEqual(body.at("response_format"), json::object{{"type", "json_object"}}));
  EXPECT(body.at("model") == "gpt-oss-120b");
  EXPECT(body.at("reasoning_effort") == "high");
  EXPECT_EQ(rt::getNumber(body, "max_completion_tokens"), std::optional<double>(4'096));
  const json::array& messages = body.at("messages").as_array();
  std::vector<std::string> roles;
  for (const json::value& message : messages) roles.push_back(std::string(message.at("role").as_string()));
  EXPECT_EQ(roles, (std::vector<std::string>{"system", "user"}));
  EXPECT_EQ(std::string(messages.at(1).at("content").as_string()), prompt);

  SupervisorBriefContext huge = emptyContext;
  huge.transcriptAvailable = true;
  for (int index = 0; index < 40; index += 1) {
    huge.turns.push_back(BriefTranscriptTurn{
      .id = "turn-" + std::to_string(index),
      .status = "completed",
      .startedAtMs = 1'790'000'000,
      .completedAtMs = std::nullopt,
      .items = std::vector<BriefTranscriptItem>(12, BriefTranscriptItem{.type = "assistant", .text = std::string(1'200, 'y')}),
      .droppedItems = 0,
      .freshTail = index == 0,
    });
  }
  const json::object hugeBody = buildSupervisorBriefRequestBody(huge);
  const std::string hugePrompt(hugeBody.at("messages").as_array().at(1).at("content").as_string());
  EXPECT_EQ(rt::length(hugePrompt), std::int64_t{48 * 1024});  // the prompt is bounded before it is sent
  EXPECT(rt::endsWith(hugePrompt, "…"));
}

ASYNC_TEST("handleAdminCodexSupervisorBrief validates the request body and refuses an unknown source") {
  // As the TS process runs: an unreadable configuration file in the working directory and no HOME/CODEX_HOME.
  const BriefEnvironment environment;
  const ConfinedFixture fixture;
  const ScopedCwd cwd(fixture.cwd);
  const auto send = [](std::string body) { return handleAdminCodexSupervisorBrief(briefRequest(std::move(body))); };

  auto sendingNotJson = send("not json");
  const rt::Response notJson = co_await std::move(sendingNotJson);
  EXPECT_EQ(notJson.status(), 400);
  auto readingNotJson = errorMessageOf(notJson);
  const std::string notJsonMessage = co_await std::move(readingNotJson);
  EXPECT_EQ(notJsonMessage, "Expected a JSON body with source and id");

  auto sendingNoSource = send("{}");
  const rt::Response noSource = co_await std::move(sendingNoSource);
  EXPECT_EQ(noSource.status(), 400);
  auto readingNoSource = errorMessageOf(noSource);
  const std::string noSourceMessage = co_await std::move(readingNoSource);
  EXPECT_EQ(noSourceMessage, "Invalid supervisor source id");

  auto sendingBadThread = send(rt::jsonStringify(json::value(json::object{{"source", "local"}, {"id", "bad id!"}})));
  const rt::Response badThread = co_await std::move(sendingBadThread);
  EXPECT_EQ(badThread.status(), 400);
  auto readingBadThread = errorMessageOf(badThread);
  const std::string badThreadMessage = co_await std::move(readingBadThread);
  EXPECT_EQ(badThreadMessage, "Invalid thread id");

  auto sendingUnknown = send(rt::jsonStringify(json::value(json::object{{"source", "local"}, {"id", "01a0c7ae-5be8-7712-aaee-26638cf84a32"}})));
  const rt::Response unknownSource = co_await std::move(sendingUnknown);
  EXPECT_EQ(unknownSource.status(), 400);
  auto readingUnknown = errorMessageOf(unknownSource);
  const std::string unknownMessage = co_await std::move(readingUnknown);
  EXPECT_EQ(unknownMessage, "Unknown supervisor source");
}

ASYNC_TEST("a repeated opening turn is merged and an empty newest turn is enriched from the single frame-safe full read") {
  const BriefEnvironment environment;
  {
    const ScriptedSockets scripted;
    const auto fullRequests = std::make_shared<std::vector<json::object>>();
    ScriptedSocket::plan([fullRequests](const std::string& method, const json::object& params, const json::object&) {
      if (method == "thread/read") return replyWith(rt::jsonParse(R"({"thread":{"id":"thread-1","name":"Enrichment","updatedAt":1790001500}})"));
      if (method != "thread/turns/list") return replyWith(json::object());
      if (rt::getString(params, "itemsView") == "full") {
        fullRequests->push_back(params);
        return replyWith(rt::jsonParse(R"({"data":[{"id":"turn-3","status":"inProgress","startedAt":1790001000,"items":[{"type":"agentMessage","text":"newest full content"}]}]})"));
      }
      if (rt::getString(params, "sortDirection") == "asc") {
        return replyWith(rt::jsonParse(R"({"data":[{"id":"turn-1","status":"completed","startedAt":1790000000,"completedAt":1790000100,
          "items":[{"type":"userMessage","content":[{"type":"text","text":"opening request"}]}]}]})"));
      }
      return replyWith(rt::jsonParse(R"({"data":[
        {"id":"turn-3","status":"inProgress","startedAt":1790001000,"items":[]},
        {"id":"turn-2","status":"completed","startedAt":1790000500,"completedAt":1790000600,"items":[{"type":"agentMessage","text":"middle progress"}]},
        {"id":"turn-1","status":"completed","startedAt":1790000000,"completedAt":1790000100,"items":[{"type":"agentMessage","text":"opening tail"}]}]})"));
    });

    const SupervisorSource source{.id = "vps", .name = "vps", .socketPath = "/var/lib/uos-supervisor/enrich.sock", .codexHome = std::nullopt};
    auto collecting = collectSupervisorBriefContext(source, "thread-1", deferredSignal());
    const SupervisorBriefContext context = co_await std::move(collecting);
    EXPECT_EQ(turnIdsOf(context), (std::vector<std::string>{"turn-1", "turn-2", "turn-3"}));  // the repeated opening turn is merged in place and the chronological order is preserved
    const BriefTranscriptTurn* opening = turnWithId(context.turns, "turn-1");
    const BriefTranscriptTurn* newest = turnWithId(context.turns, "turn-3");
    ASSERT(opening != nullptr && newest != nullptr);
    EXPECT_EQ(itemTextsOf(*opening), (std::vector<std::string>{"opening request", "opening tail"}));  // the merged opening turn keeps its request and its newest recorded tail
    EXPECT_EQ(itemTextsOf(*newest), std::vector<std::string>{"newest full content"});  // the empty newest turn is replaced by the one permitted full read
    ASSERT_EQ(fullRequests->size(), std::size_t{1});  // exactly one full-view read is issued
    EXPECT_EQ(rt::getString((*fullRequests)[0], "sortDirection"), std::optional<std::string_view>("desc"));
    EXPECT_EQ(rt::getNumber((*fullRequests)[0], "limit"), std::optional<double>(1));
  }

  {
    const ScriptedSockets scripted;
    ScriptedSocket::plan([](const std::string& method, const json::object& params, const json::object&) {
      if (method == "thread/read") return replyWith(rt::jsonParse(R"({"thread":{"id":"thread-1","status":{"type":"active"}}})"));
      if (method != "thread/turns/list") return replyWith(json::object());
      if (rt::getString(params, "itemsView") == "full") return replyError("app-server connection failed");
      if (rt::getString(params, "sortDirection") == "asc") return replyWith(json::object{{"data", json::array{}}});
      return replyWith(rt::jsonParse(R"({"data":[{"id":"turn-9","status":"inProgress","items":[]}]})"));
    });
    const SupervisorSource source{.id = "vps", .name = "vps", .socketPath = "/var/lib/uos-supervisor/failed-full.sock", .codexHome = std::nullopt};
    auto collecting = collectSupervisorBriefContext(source, "thread-1", deferredSignal());
    const SupervisorBriefContext context = co_await std::move(collecting);
    EXPECT_EQ(context.transcriptAvailable, false);  // a failed full read degrades to the honest recorded summaries
    EXPECT(context.turns.empty());
    EXPECT_EQ(context.state, std::optional<std::string>("active"));  // the fresh runtime metadata still survives the failed enrichment
  }
}

ASYNC_TEST("brief tail pages keep the newest items, skip content-free parts and tolerate a repeated turn with no opening request") {
  const BriefEnvironment environment;
  const ScriptedSockets scripted;
  ScriptedSocket::plan([](const std::string& method, const json::object& params, const json::object&) {
    if (method == "thread/read") return replyWith(rt::jsonParse(R"({"thread":{"id":"thread-1","status":{"type":"idle"}}})"));
    if (method != "thread/turns/list") return replyWith(json::object());
    if (rt::getString(params, "itemsView") == "full") return replyWith(json::object{{"data", json::array{}}});
    if (rt::getString(params, "sortDirection") == "asc") {
      return replyWith(rt::jsonParse(R"({"data":[{"id":"turn-1","status":"completed","items":[
        {"type":"userMessage","content":"not-an-array"},{"type":"agentMessage","text":"recorded opening"}]}]})"));
    }
    json::array items;
    for (int index = 0; index < 13; index += 1) items.push_back(json::object{{"type", "agentMessage"}, {"text", "tail item " + std::to_string(index)}});
    return replyWith(json::object{{"data", json::array{json::object{{"id", "turn-1"}, {"status", "completed"}, {"items", std::move(items)}}}}});
  });
  const SupervisorSource source{.id = "vps", .name = "vps", .socketPath = "/var/lib/uos-supervisor/tail.sock", .codexHome = std::nullopt};
  auto collecting = collectSupervisorBriefContext(source, "thread-1", deferredSignal());
  const SupervisorBriefContext context = co_await std::move(collecting);
  const BriefTranscriptTurn* opening = turnWithId(context.turns, "turn-1");
  ASSERT(opening != nullptr);
  std::vector<std::string> expected;
  for (int index = 1; index <= 12; index += 1) expected.push_back("tail item " + std::to_string(index));
  EXPECT_EQ(itemTextsOf(*opening), expected);  // the newest tail items win the per-turn cap while the content-free user message contributes nothing
  EXPECT_EQ(opening->droppedItems, std::int64_t{1});  // the single item dropped by the tail cap is reported
}

ASYNC_TEST("handleAdminCodexSupervisorBrief refuses a JSON body that is not an object") {
  const BriefEnvironment environment;
  auto sending = handleAdminCodexSupervisorBrief(briefRequest("\"a string, not an object\""));
  const rt::Response response = co_await std::move(sending);
  EXPECT_EQ(response.status(), 400);
  auto reading = errorMessageOf(response);
  const std::string message = co_await std::move(reading);
  EXPECT_EQ(message, "Expected a JSON body with source and id");
}

ASYNC_TEST("an empty opening turn older than the newest turn keeps its honest absence while the newest turn is enriched") {
  const BriefEnvironment environment;
  const ScriptedSockets scripted;
  const auto fullCalls = std::make_shared<std::vector<json::object>>();
  ScriptedSocket::plan([fullCalls](const std::string& method, const json::object& params, const json::object&) {
    if (method == "thread/read") return replyWith(rt::jsonParse(R"({"thread":{"id":"thread-1","status":{"type":"active"}}})"));
    if (method != "thread/turns/list") return replyWith(json::object());
    if (rt::getString(params, "itemsView") == "full") {
      fullCalls->push_back(params);
      return replyWith(rt::jsonParse(R"({"data":[{"id":"turn-2","status":"inProgress","startedAt":1790000900,"items":[{"type":"agentMessage","text":"enriched newest"}]}]})"));
    }
    if (rt::getString(params, "sortDirection") == "asc") {
      return replyWith(rt::jsonParse(R"({"data":[{"id":"turn-1","status":"completed","startedAt":1790000100,"completedAt":1790000200,"items":[]}]})"));
    }
    return replyWith(rt::jsonParse(R"({"data":[
      {"id":"turn-2","status":"inProgress","startedAt":1790000900,"items":[]},
      {"id":"turn-1","status":"completed","startedAt":1790000100,"completedAt":1790000200,"items":[]}]})"));
  });
  const SupervisorSource source{.id = "vps", .name = "vps", .socketPath = "/var/lib/uos-supervisor/unreachable.sock", .codexHome = std::nullopt};
  auto collecting = collectSupervisorBriefContext(source, "thread-1", deferredSignal());
  const SupervisorBriefContext context = co_await std::move(collecting);
  EXPECT_EQ(fullCalls->size(), std::size_t{1});  // the unreachable older target buys no extra read
  EXPECT_EQ(turnIdsOf(context), std::vector<std::string>{"turn-2"});  // only the reachable newest turn is returned
  ASSERT(!context.turns.empty());
  EXPECT_EQ(itemTextsOf(context.turns[0]), std::vector<std::string>{"enriched newest"});
  EXPECT_EQ(context.truncated, true);  // the omitted empty opening is reported as an incomplete transcript
}
