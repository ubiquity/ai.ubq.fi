// Differential case for src/codex/supervisor-inventory.ts's sampling round (no TS test drives a
// configured source): ensureSupervisorSnapshot() over four configured sources, two answered by
// app-server stand-ins on Unix sockets (rt::ws servers scripted with the recorded RESPONSES) and
// two without a socket, with a Codex home holding the SQLite state databases built by SQL. The
// expected snapshot was recorded from Bun 1.4.0 with ws 8.18.3 stand-ins and bun:sqlite
// (tests/codex_supervisor_inventory_differential_vectors.inc).
// Links: src/codex/supervisor_config.cpp src/codex/supervisor_inventory.cpp src/codex/supervisor_transport.cpp
//        src/runtime/websocket.cpp
#include "uos/codex/supervisor_inventory.hpp"

#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/websocket.hpp"

#include <exception>
#include <boost/asio/local/stream_protocol.hpp>

#include <array>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace ws = uos::rt::ws;
  namespace asio = boost::asio;
  using namespace uos::codex::supervisor_inventory;
  using uos::codex::supervisor_config::SupervisorQuotaBucket;
  using uos::codex::supervisor_config::SupervisorSourceView;
  using uos::codex::supervisor_config::toString;

#include "tests/codex_supervisor_inventory_differential_vectors.inc"

  using Generic = asio::generic::stream_protocol::socket;

  // { result?: unknown; error?: string }
  struct Reply {
    std::optional<json::value> result;
    std::optional<std::string> error;
  };

  Reply respond(const json::object& table, const std::string& method, const json::object& params) {
    const json::value* entry = rt::get(table, method);
    if (entry == nullptr) return {};
    const json::value* value = entry;
    if (method == "thread/list") {
      const json::value* cursor = rt::get(params, "cursor");
      value = rt::get(*entry, cursor && cursor->is_string() ? std::string(cursor->as_string()) : std::string());
    } else if (method == "thread/read" || method == "thread/turns/list") {
      const json::value* threadId = rt::get(params, "threadId");
      value = threadId && threadId->is_string() ? rt::get(*entry, std::string(threadId->as_string())) : nullptr;
    }
    if (value == nullptr) return {};
    if (value->is_object() && rt::hasOwn(value->as_object(), "__error")) return {std::nullopt, std::string(value->as_object().at("__error").as_string())};
    return {*value, std::nullopt};
  }

  // A Codex app-server stand-in on a Unix socket answering every request frame from `table`.
  class AppServer : public std::enable_shared_from_this<AppServer> {
  public:
    explicit AppServer(json::object table_) : table(std::move(table_)) {}
    json::object table;

    void listen(const std::string& socketPath) {
      acceptor_ = std::make_unique<asio::local::stream_protocol::acceptor>(rt::executor(), asio::local::stream_protocol::endpoint(socketPath));
      accept();
    }
    void close() {
      boost::system::error_code ignored;
      if (acceptor_) acceptor_->close(ignored);
      acceptor_.reset();
    }

  private:
    void accept() {
      acceptor_->async_accept([self = shared_from_this()](const boost::system::error_code& error, asio::local::stream_protocol::socket socket) {
        if (error) return;
        self->readHead(std::make_shared<Generic>(std::move(socket)), std::make_shared<std::string>());
        if (self->acceptor_) self->accept();
      });
    }
    void readHead(std::shared_ptr<Generic> socket, std::shared_ptr<std::string> received) {
      auto buffer = std::make_shared<std::array<char, 4096>>();
      socket->async_read_some(asio::buffer(*buffer), [self = shared_from_this(), socket, received, buffer](const boost::system::error_code& error, std::size_t count) {
        if (error) return;
        received->append(buffer->data(), count);
        const std::size_t end = received->find("\r\n\r\n");
        if (end == std::string::npos) return self->readHead(socket, received);
        ws::UpgradeRequest request;
        const std::vector<std::string> lines = rt::split(received->substr(0, end), "\r\n");
        const std::vector<std::string> requestLine = rt::split(lines[0], " ");
        request.method = requestLine[0];
        request.url = requestLine[1];
        for (std::size_t index = 1; index < lines.size(); ++index) {
          const std::size_t colon = lines[index].find(':');
          request.rawHeaders.emplace_back(lines[index].substr(0, colon), rt::trim(lines[index].substr(colon + 1)));
        }
        ws::Bytes rest(received->begin() + static_cast<std::ptrdiff_t>(end + 4), received->end());
        ws::UpgradeSocket upgrade{std::move(*socket)};
        ws::ServerOptions options;
        options.noServer = true;
        auto server = std::make_shared<ws::WebSocketServer>(options);
        server->handleUpgrade(request, upgrade, std::move(rest), [self](std::shared_ptr<ws::WebSocket> accepted, const ws::UpgradeRequest&) { self->serve(accepted); });
        server->close();
      });
    }
    void serve(const std::shared_ptr<ws::WebSocket>& socket) {
      ws::WebSocket* raw = socket.get();
      socket->on("message", [self = shared_from_this(), raw](const ws::EventArgs& event) {
        const json::value frame = rt::jsonParse(rt::decodeUtf8(std::get<ws::Bytes>(event.data)));
        const json::value* id = rt::get(frame, "id");
        if (id == nullptr || !rt::isNumber(*id)) return;
        const json::value* params = rt::get(frame, "params");
        const Reply reply = respond(self->table, std::string(rt::get(frame, "method")->as_string()), params && params->is_object() ? params->as_object() : json::object());
        json::object out;
        out["id"] = *id;
        if (reply.error) out["error"] = json::object{{"message", *reply.error}};
        else if (reply.result) out["result"] = *reply.result;
        raw->send(rt::jsonStringify(json::value(out)));
      });
    }

    std::unique_ptr<asio::local::stream_protocol::acceptor> acceptor_;
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

  template <class T> json::value orNull(const std::optional<T>& value) { return value ? json::value(*value) : json::value(nullptr); }
  json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  json::array strings(const std::vector<std::string>& values) {
    json::array out;
    for (const std::string& value : values) out.push_back(json::value(value));
    return out;
  }

  // JSON.stringify of a snapshot with the JS property order of the objects the TS builds;
  // sampledAtMs, durationMs and lastSampledAtMs are written as 0 like the recording.
  json::value snapshotJson(const SupervisorSnapshot& snapshot) {
    json::array sources;
    for (const SupervisorSourceView& view : snapshot.sources) {
      json::value quota = nullptr;
      if (view.quota) {
        json::array buckets;
        for (const SupervisorQuotaBucket& bucket : view.quota->buckets) {
          json::object item;
          item["limitId"] = bucket.limitId;
          item["limitName"] = orNull(bucket.limitName);
          item["usedPercent"] = numberOrNull(bucket.usedPercent);
          item["windowDurationMins"] = numberOrNull(bucket.windowDurationMins);
          item["resetsAtMs"] = numberOrNull(bucket.resetsAtMs);
          item["planType"] = orNull(bucket.planType);
          item["reachedType"] = orNull(bucket.reachedType);
          buckets.push_back(item);
        }
        json::object out;
        out["accountScope"] = view.quota->accountScope;
        out["buckets"] = buckets;
        out["resetCreditsAvailable"] = numberOrNull(view.quota->resetCreditsAvailable);
        quota = out;
      }
      json::object source;
      source["id"] = view.id;
      source["name"] = view.name;
      source["kind"] = view.kind;
      source["hostname"] = orNull(view.hostname);
      source["state"] = view.state;
      source["reason"] = orNull(view.reason);
      source["metadata"] = json::object{{"available", view.metadata.available}, {"database", orNull(view.metadata.database)}, {"reason", orNull(view.metadata.reason)}};
      source["inventory"] = json::object{{"listed", view.inventory.listed}, {"sampled", view.inventory.sampled}, {"returned", view.inventory.returned}, {"truncated", view.inventory.truncated}};
      source["lineage"] = view.lineage;
      source["quota"] = quota;
      source["quotaReason"] = orNull(view.quotaReason);
      source["durationMs"] = 0;
      source["sampledAtMs"] = 0;
      sources.push_back(source);
    }
    json::array sessions;
    for (const SupervisorSession& session : snapshot.sessions) {
      json::object item;
      item["id"] = session.id;
      item["sourceId"] = session.sourceId;
      item["machine"] = session.machine;
      item["title"] = orNull(session.title);
      item["titleSource"] = orNull(session.titleSource);
      item["cwd"] = orNull(session.cwd);
      item["branch"] = orNull(session.branch);
      item["model"] = orNull(session.model);
      item["effort"] = orNull(session.effort);
      item["provider"] = orNull(session.provider);
      item["sourceKind"] = orNull(session.sourceKind);
      item["parentThreadId"] = orNull(session.parentThreadId);
      item["lastActivityAtMs"] = numberOrNull(session.lastActivityAtMs);
      item["tokensUsed"] = numberOrNull(session.tokensUsed);
      item["usageSource"] = orNull(session.usageSource);
      item["state"] = std::string(toString(session.state));
      item["activeFlags"] = strings(session.activeFlags);
      item["loaded"] = orNull(session.loaded);
      item["waitingOnApproval"] = session.waitingOnApproval;
      item["waitingOnUserInput"] = session.waitingOnUserInput;
      item["childIds"] = session.childIds ? json::value(strings(*session.childIds)) : json::value(nullptr);
      item["lastSampledAtMs"] = 0;
      item["sampled"] = session.sampled;
      item["unavailable"] = strings(session.unavailable);
      sessions.push_back(item);
    }
    json::object out;
    out["sampledAtMs"] = 0;
    out["view"] = snapshot.view;
    out["sources"] = sources;
    out["sessions"] = sessions;
    out["counts"] = json::object{{"total", snapshot.counts.total}, {"active", snapshot.counts.active},   {"waiting", snapshot.counts.waiting}, {"idle", snapshot.counts.idle},
                                 {"stale", snapshot.counts.stale}, {"unknown", snapshot.counts.unknown}, {"systemError", snapshot.counts.systemError}};
    out["coverage"] = json::object{{"listed", snapshot.coverage.listed}, {"sampled", snapshot.coverage.sampled}, {"truncated", snapshot.coverage.truncated}, {"notes", strings(snapshot.coverage.notes)}};
    return out;
  }
}

ASYNC_TEST("differential: a sampling round over configured sources matches Bun") {
  const json::object responses = rt::jsonParse(RESPONSES).as_object();
  const json::object sql = rt::jsonParse(SQL).as_object();
  const std::string root = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "inv-"));
  const std::string home = rt::path::join(root, "home");
  rt::fs::mkdirSync(home);
  rt::fs::mkdirSync(rt::path::join(root, ".data"));
  for (const char* name : {"state3", "state5"}) {
    const rt::sqlite::Database database(rt::path::join(home, std::string(name) == "state3" ? "state_3.sqlite" : "state_5.sqlite"));
    for (const json::value& statement : sql.at(name).as_array()) database.run(std::string(statement.as_string()));
    database.close();
  }
  rt::fs::writeFileSync(rt::path::join(home, "state_x.sqlite"), std::string_view("not a database"));
  json::array sources;
  sources.push_back(json::object{{"id", "local"}, {"name", "Local"}, {"socketPath", rt::path::join(root, "local.sock")}, {"codexHome", home}});
  sources.push_back(json::object{{"id", "remote"}, {"name", "Remote"}, {"socketPath", rt::path::join(root, "remote.sock")}});
  sources.push_back(json::object{{"id", "gone"}, {"name", "Gone"}, {"socketPath", rt::path::join(root, "missing.sock")}, {"codexHome", home}});
  sources.push_back(json::object{{"id", "gone-remote"}, {"name", "Gone remote"}, {"socketPath", rt::path::join(root, "missing-remote.sock")}});
  rt::fs::writeFileSync(rt::path::join(root, ".data", "codex-supervisor.json"), rt::jsonStringify(json::value(json::object{{"sources", sources}})));
  std::vector<std::shared_ptr<AppServer>> servers;
  for (const char* name : {"local", "remote"}) {
    auto server = std::make_shared<AppServer>(responses.at(name).as_object());
    server->listen(rt::path::join(root, std::string(name) + ".sock"));
    servers.push_back(server);
  }
  std::string actual;
  {
    const ScopedCwd cwd(root);
    const rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}};
    auto sampling = ensureSupervisorSnapshot();
    const SupervisorSnapshot snapshot = co_await std::move(sampling);
    actual = rt::replaceAll(rt::replaceAll(rt::jsonStringify(snapshotJson(snapshot)), root, "<ROOT>"), rt::os::hostname(), "<HOST>");
  }
  for (const auto& server : servers) server->close();
  co_await rt::sleep(20);
  rt::fs::rmSync(root, {.recursive = true, .force = true});
  EXPECT_EQ(actual, std::string(EXPECTED_SNAPSHOT));
}
