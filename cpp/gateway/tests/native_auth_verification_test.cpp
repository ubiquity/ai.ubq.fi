// Port of tests/native-auth-verification.test.ts, plus differential cases for
// requestNativeCodexRefresh (only the manual tests/helpers/codex-native-auth-acceptance.ts
// drives it, against a real codex-cli; that helper is not ported). The expected outcomes and
// frames were recorded from Bun 1.4.0 running the TS function against a `ws` 8.18.3 server
// on the home's app-server-control Unix socket with the same scripts.
// Links: src/codex/native_auth_transport.cpp src/runtime/websocket.cpp src/utils.cpp
#include "uos/codex/native_auth_transport.hpp"

#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/websocket.hpp"

#include <boost/asio/local/stream_protocol.hpp>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace ws = uos::rt::ws;
  namespace asio = boost::asio;
  using namespace uos::codex::native_auth_transport;

  const NativeCodexExpectedAccount expected{"native@example.test", "acct-native"};

  json::value reported(json::object overrides = {}) {
    json::object account{{"type", "chatgpt"}, {"email", expected.email}, {"planType", "pro"}};
    for (const auto& [key, value] : overrides) rt::objectSet(account, key, value);
    json::object out;
    out["account"] = account;
    out["requiresOpenaiAuth"] = true;
    out["workspaceRouting"] = json::object{{"accountRoutingOverride", "NO_CONSTRAINT"}, {"backendOrigin", "https://chatgpt.com"}, {"chatgptAccountId", expected.accountId}};
    return out;
  }

  json::value withMember(json::value base, std::string key, json::value value) {
    rt::objectSet(base.as_object(), key, value);
    return base;
  }
}

TEST("a reported ChatGPT identity must match email and account exactly") {
  EXPECT_EQ(verifyNativeCodexAccount(reported(), expected, std::nullopt).ok, true);
  const std::vector<std::optional<json::value>> mismatches{
    reported(json::object{{"email", "other@example.test"}}),
    withMember(reported(), "workspaceRouting", json::object{{"accountRoutingOverride", "NO_CONSTRAINT"}, {"backendOrigin", "https://chatgpt.com"}, {"chatgptAccountId", "acct-other"}}),
    withMember(reported(), "account", json::object{{"type", "apiKey"}}),
    rt::jsonParse(R"({ "account": null, "requiresOpenaiAuth": true, "workspaceRouting": null })"),
    json::object(),
    json::value(nullptr),
  };
  for (const auto& value : mismatches) EXPECT_EQ(verifyNativeCodexAccount(*value, expected, expected.accountId).ok, false);
}

TEST("a blank account is accepted only for a non-OpenAI-auth provider with the bound file account") {
  const json::value blank = rt::jsonParse(R"({ "account": null, "requiresOpenaiAuth": false, "workspaceRouting": null })");
  EXPECT_EQ(verifyNativeCodexAccount(blank, expected, expected.accountId).ok, true);
  for (const std::optional<std::string>& fileAccountId : {std::optional<std::string>(), std::optional<std::string>("acct-other")}) {
    EXPECT_EQ(verifyNativeCodexAccount(blank, expected, fileAccountId).ok, false);
  }
  EXPECT_EQ(verifyNativeCodexAccount(rt::jsonParse(R"({ "account": null, "requiresOpenaiAuth": true, "workspaceRouting": null })"), expected, expected.accountId).ok, false);
  EXPECT_EQ(verifyNativeCodexAccount(rt::jsonParse(R"({ "account": null })"), expected, expected.accountId).ok, false);
}

TEST("differential: verification details and the undefined account") {
  using V = NativeCodexAccountVerification;
  const V accepted{true, std::nullopt};
  const V unreported{false, "Native Codex daemon did not report its ChatGPT account"};
  const V different{false, "Native Codex daemon uses a different ChatGPT account"};
  EXPECT(verifyNativeCodexAccount(reported(), expected, std::nullopt) == accepted);
  EXPECT(verifyNativeCodexAccount(nullptr, expected, expected.accountId) == unreported);
  EXPECT(verifyNativeCodexAccount(reported(json::object{{"email", "x"}}), expected, std::nullopt) == different);
  // utils.isRecord counts an array as a record: an array account reads no members.
  EXPECT(verifyNativeCodexAccount(json::array{1}, expected, expected.accountId) == unreported);
  EXPECT(verifyNativeCodexAccount(rt::jsonParse(R"({ "account": [], "workspaceRouting": { "chatgptAccountId": "acct-native" } })"), expected, std::nullopt) == different);
}

namespace {
  using Generic = asio::generic::stream_protocol::socket;
  // What the scripted daemon answers: the result of a request (std::nullopt: no `result`).
  using Script = std::function<std::optional<json::value>(const std::string& method, const std::string& home)>;

  // A Codex app-server stand-in on a Unix socket: one HTTP upgrade per connection, handed
  // to a noServer ws server whose sockets answer each request frame through `script`.
  class Daemon : public std::enable_shared_from_this<Daemon> {
  public:
    Daemon(std::string home_, std::string mode_, Script script_) : home(std::move(home_)), mode(std::move(mode_)), script(std::move(script_)) {}
    std::string home;
    std::string mode;
    Script script;
    std::vector<std::string> frames;

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
        options.clientTracking = false;
        options.perMessageDeflate = false;
        auto server = std::make_shared<ws::WebSocketServer>(options);
        server->handleUpgrade(request, upgrade, std::move(rest), [self](std::shared_ptr<ws::WebSocket> accepted, const ws::UpgradeRequest&) { self->serve(accepted); });
        server->close();
      });
    }
    void serve(const std::shared_ptr<ws::WebSocket>& socket) {
      if (mode == "close-on-open") {
        socket->close(1000, ws::Data(std::string("bye")));
        return;
      }
      ws::WebSocket* raw = socket.get();
      socket->on("message", [self = shared_from_this(), raw](const ws::EventArgs& event) {
        const std::string text = rt::decodeUtf8(std::get<ws::Bytes>(event.data));
        self->frames.push_back(text);
        const json::value frame = rt::jsonParse(text);
        const json::value* id = rt::get(frame, "id");
        if (id == nullptr || !rt::isNumber(*id)) return;
        json::object reply;
        reply["id"] = *id;
        if (self->mode == "error-initialize") {
          reply["error"] = json::object{{"code", -1}, {"message", "nope"}};
        } else {
          const std::optional<json::value> result = self->script(std::string(rt::get(frame, "method")->as_string()), self->home);
          if (result) reply["result"] = *result;
        }
        raw->send(rt::jsonStringify(json::value(reply)));
      });
    }

    std::unique_ptr<asio::local::stream_protocol::acceptor> acceptor_;
  };

  json::value accountReply() {
    return rt::jsonParse(R"({ "account": { "type": "chatgpt", "email": "native@example.test", "planType": "pro" }, "requiresOpenaiAuth": true, "workspaceRouting": { "chatgptAccountId": "acct-native" } })");
  }

  // One scenario: the outcome ("resolved" or "rejected: <message>") and the frames the
  // daemon received, with the home directory written as <HOME>.
  rt::Task<std::string> scenario(std::string mode, Script script, std::optional<json::value> authJson, std::string expectedAccount) {
    const std::string home = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "nat-"));
    rt::fs::mkdirSync(rt::path::join(home, "app-server-control"));
    if (authJson) rt::fs::writeFileSync(rt::path::join(home, "auth.json"), rt::jsonStringify(*authJson));
    auto daemon = std::make_shared<Daemon>(home, mode, script);
    if (mode != "none") daemon->listen(rt::path::join(home, "app-server-control", "app-server-control.sock"));
    std::string outcome;
    rt::Reason failure;
    try {
      auto refreshing = requestNativeCodexRefresh(home, "native@example.test", expectedAccount);
      co_await std::move(refreshing);
      outcome = "resolved";
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) outcome = "rejected: " + rt::replaceAll(rt::errorMessage(failure).value_or(rt::reasonToString(failure)), home, "<HOME>");
    daemon->close();
    co_await rt::sleep(20);
    rt::fs::rmSync(home, {.recursive = true, .force = true});
    json::object out;
    out["outcome"] = outcome;
    json::array frames;
    for (const std::string& frame : daemon->frames) frames.push_back(rt::jsonParse(frame));
    out["frames"] = frames;
    co_return rt::replaceAll(rt::jsonStringify(json::value(out)), home, "<HOME>");
  }

  const char* const INITIALIZE_FRAME = R"({"id":1,"method":"initialize","params":{"clientInfo":{"name":"uos_native_auth","version":"1"},"capabilities":{"experimentalApi":true}}})";
  std::string framesOf(std::vector<std::string> frames) {
    std::string out = "[";
    for (std::size_t index = 0; index < frames.size(); ++index) out += (index ? "," : "") + frames[index];
    return out + "]";
  }
  std::string expect(const std::string& outcome, std::vector<std::string> frames) { return R"({"outcome":")" + outcome + R"(","frames":)" + framesOf(std::move(frames)) + "}"; }
  const std::string INITIALIZED = R"({"method":"initialized"})";
  const std::string READ = R"({"id":2,"method":"account/read","params":{"refreshToken":false}})";
  const std::string REFRESH = R"({"id":3,"method":"account/read","params":{"refreshToken":true}})";
}

ASYNC_TEST("differential: requestNativeCodexRefresh verifies the daemon before it asks for a refresh") {
  const Script ok = [](const std::string& method, const std::string& home) -> std::optional<json::value> {
    if (method == "initialize") return json::value(json::object{{"codexHome", home}});
    return accountReply();
  };
  const auto initializeWith = [](json::value initialized) {
    return Script([initialized](const std::string& method, const std::string&) -> std::optional<json::value> { return method == "initialize" ? initialized : accountReply(); });
  };
  const Script blankScript = [](const std::string& method, const std::string& home) -> std::optional<json::value> {
    if (method == "initialize") return json::value(json::object{{"codexHome", home}});
    return rt::jsonParse(R"({ "account": null, "requiresOpenaiAuth": false, "workspaceRouting": null })");
  };
  const Script undefinedResult = [](const std::string& method, const std::string& home) -> std::optional<json::value> {
    if (method == "initialize") return json::value(json::object{{"codexHome", home}});
    return std::nullopt;
  };
  const std::optional<json::value> noAuth;
  const std::string account = "acct-native";
  struct Case {
    std::string name;
    std::string mode;
    Script script;
    std::optional<json::value> authJson;
    std::string expectedAccount;
    std::string expected;
  };
  const std::vector<Case> cases{
    {"success", "script", ok, noAuth, account, expect("resolved", {INITIALIZE_FRAME, INITIALIZED, READ, REFRESH})},
    {"wrong-home", "script", initializeWith(json::object{{"codexHome", "/tmp"}}), noAuth, account, expect("rejected: Native Codex daemon uses a different credential home", {INITIALIZE_FRAME})},
    {"missing-home", "script", initializeWith(json::object()), noAuth, account, expect("rejected: Native Codex daemon uses a different credential home", {INITIALIZE_FRAME})},
    {"empty-home", "script", initializeWith(json::object{{"codexHome", ""}}), noAuth, account, expect("rejected: Native Codex daemon uses a different credential home", {INITIALIZE_FRAME})},
    {"nonexistent-home", "script", initializeWith(json::object{{"codexHome", "/nonexistent-native-home"}}), noAuth, account,
     expect("rejected: ENOENT: no such file or directory, lstat '/nonexistent-native-home'", {INITIALIZE_FRAME})},
    {"mismatch", "script", ok, noAuth, "acct-other", expect("rejected: Native Codex daemon uses a different ChatGPT account", {INITIALIZE_FRAME, INITIALIZED, READ})},
    {"blank-with-file", "script", blankScript, rt::jsonParse(R"({ "auth_mode": "chatgpt", "tokens": { "account_id": "acct-native" } })"), account,
     expect("resolved", {INITIALIZE_FRAME, INITIALIZED, READ, REFRESH})},
    {"blank-file-other", "script", blankScript, rt::jsonParse(R"({ "auth_mode": "chatgpt", "tokens": { "account_id": "acct-other" } })"), account,
     expect("rejected: Native Codex daemon uses a different ChatGPT account", {INITIALIZE_FRAME, INITIALIZED, READ})},
    {"blank-no-file", "script", blankScript, noAuth, account, expect("rejected: Native Codex daemon uses a different ChatGPT account", {INITIALIZE_FRAME, INITIALIZED, READ})},
    {"blank-file-apikey", "script", blankScript, rt::jsonParse(R"({ "auth_mode": "apikey", "tokens": { "account_id": "acct-native" } })"), account,
     expect("rejected: Native Codex daemon uses a different ChatGPT account", {INITIALIZE_FRAME, INITIALIZED, READ})},
    {"error-initialize", "error-initialize", ok, noAuth, account, expect("rejected: Native Codex auth request refused", {INITIALIZE_FRAME})},
    {"close-on-open", "close-on-open", ok, noAuth, account, expect("rejected: Native Codex socket closed", {})},
    {"no-daemon", "none", ok, noAuth, account, expect("rejected: Native Codex daemon unavailable", {})},
    {"undefined-result", "script", undefinedResult, noAuth, account, expect("rejected: Native Codex daemon did not report its ChatGPT account", {INITIALIZE_FRAME, INITIALIZED, READ})},
  };
  for (const Case& entry : cases) {
    auto running = scenario(entry.mode, entry.script, entry.authJson, entry.expectedAccount);
    const std::string actual = co_await std::move(running);
    if (actual != entry.expected) std::fprintf(stderr, "      scenario: %s\n", entry.name.c_str());
    EXPECT_EQ(actual, entry.expected);
  }
}
