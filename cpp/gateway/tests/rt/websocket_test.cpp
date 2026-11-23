// rt::ws against ws 8.18.3 on Bun 1.4. Every expected event log, wire byte and
// error text below was recorded from ws running under Bun with the same
// scenario (the probe method is in docs/rt/websocket.md, section 7): client and
// server loops over TCP and Unix sockets, raw peers that send exact frames, raw
// servers that send exact handshake responses, and TLS.
#include "uos/rt/websocket.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <array>
#include <deque>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
  using namespace uos;
  namespace rt = uos::rt;
  namespace ws = uos::rt::ws;
  namespace asio = boost::asio;
  using Generic = asio::generic::stream_protocol::socket;

  // ---- logs in the shape the Bun probes printed ---------------------------------------

  std::string hex(const ws::Bytes& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : bytes) {
      out += digits[byte >> 4];
      out += digits[byte & 15];
    }
    return out;
  }

  ws::Bytes bytesOf(std::string_view text) { return ws::Bytes(text.begin(), text.end()); }

  json::value describe(const ws::Data& data) {
    json::object out;
    if (const auto* text = std::get_if<std::string>(&data)) out["string"] = *text;
    else out["buffer"] = hex(std::get<ws::Bytes>(data));
    return out;
  }

  json::value describeError(const rt::Reason& reason) {
    json::object out;
    out["error"] = rt::errorName(reason).value_or("?");
    out["message"] = rt::errorMessage(reason).value_or("?");
    out["code"] = nullptr;
    out["errno"] = nullptr;
    out["syscall"] = nullptr;
    try {
      std::rethrow_exception(reason);
    } catch (const ws::CodedError& error) {
      if (!error.code.empty()) out["code"] = error.code;
      if (error.errno_) out["errno"] = static_cast<std::int64_t>(*error.errno_);
      if (error.syscall) out["syscall"] = *error.syscall;
    } catch (...) {
    }
    return out;
  }

  using Log = std::shared_ptr<json::array>;
  Log newLog() { return std::make_shared<json::array>(); }

  void push(const Log& log, std::initializer_list<json::value> entry) {
    json::array out;
    for (const auto& item : entry) out.push_back(item);
    log->push_back(std::move(out));
  }

  // The recorder of the probes: every event with its arguments and readyState.
  void record(const Log& log, const std::string& label, const std::shared_ptr<ws::WebSocket>& socket) {
    ws::WebSocket* raw = socket.get();
    socket->on("open", [log, label, raw] { push(log, {json::value(label), "open", raw->readyState()}); });
    socket->on("message", [log, label, raw](const ws::EventArgs& args) { push(log, {json::value(label), "message", describe(args.data), args.isBinary, raw->readyState()}); });
    socket->on("close", [log, label, raw](const ws::EventArgs& args) { push(log, {json::value(label), "close", args.code, describe(args.reason), raw->readyState()}); });
    socket->on("error", [log, label, raw](const ws::EventArgs& args) { push(log, {json::value(label), "error", describeError(args.error), raw->readyState()}); });
    socket->on("ping", [log, label, raw](const ws::EventArgs& args) { push(log, {json::value(label), "ping", describe(args.data), raw->readyState()}); });
    socket->on("pong", [log, label, raw](const ws::EventArgs& args) { push(log, {json::value(label), "pong", describe(args.data), raw->readyState()}); });
    socket->on("unexpected-response", [log, label, raw](const ws::EventArgs& args) {
      args.response->resume();
      push(log, {json::value(label), "unexpected-response", args.response->statusCode, raw->readyState()});
    });
    socket->on("upgrade", [log, label, raw](const ws::EventArgs& args) { push(log, {json::value(label), "upgrade", args.response->statusCode, raw->readyState()}); });
  }

  // The entries of one side, as JSON text.
  std::string side(const Log& log, std::string_view label) {
    json::array out;
    for (const auto& entry : *log) {
      if (entry.as_array()[0].as_string() == label) out.push_back(entry);
    }
    return json::serialize(out);
  }

  // Polls (5 ms steps) until `condition` holds or `ms` passed.
  rt::Task<void> waitFor(std::function<bool()> condition, double ms = 2000) {
    const double deadline = rt::performanceNow() + ms;
    while (!condition() && rt::performanceNow() < deadline) co_await rt::sleep(5);
  }

  // ---- frames ------------------------------------------------------------------------

  struct FrameOptions {
    bool fin = true;
    std::optional<std::array<std::uint8_t, 4>> mask{};
    std::uint8_t rsv = 0;
  };

  ws::Bytes frame(int opcode, ws::Bytes payload, FrameOptions options = {}) {
    ws::Bytes out;
    out.push_back(static_cast<std::uint8_t>((options.fin ? 0x80 : 0) | options.rsv | opcode));
    const std::uint8_t maskBit = options.mask ? 0x80 : 0;
    const std::size_t length = payload.size();
    if (length < 126) {
      out.push_back(static_cast<std::uint8_t>(maskBit | length));
    } else if (length < 65536) {
      out.push_back(static_cast<std::uint8_t>(maskBit | 126));
      out.push_back(static_cast<std::uint8_t>(length >> 8));
      out.push_back(static_cast<std::uint8_t>(length & 255));
    } else {
      out.push_back(static_cast<std::uint8_t>(maskBit | 127));
      for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(length) >> shift));
    }
    if (options.mask) {
      out.insert(out.end(), options.mask->begin(), options.mask->end());
      for (std::size_t index = 0; index < payload.size(); ++index) payload[index] ^= (*options.mask)[index % 4];
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
  }
  ws::Bytes frame(int opcode, std::string_view payload, FrameOptions options = {}) { return frame(opcode, bytesOf(payload), options); }

  constexpr std::array<std::uint8_t, 4> MASK{0x11, 0x22, 0x33, 0x44};

  // One frame a peer sent, unmasked.
  struct ParsedFrame {
    int first = 0;
    bool masked = false;
    ws::Bytes payload;
  };
  std::vector<ParsedFrame> parseFrames(const ws::Bytes& data) {
    std::vector<ParsedFrame> out;
    std::size_t position = 0;
    while (position + 2 <= data.size()) {
      ParsedFrame parsed;
      parsed.first = data[position];
      parsed.masked = (data[position + 1] & 0x80) != 0;
      std::size_t length = data[position + 1] & 0x7f;
      position += 2;
      if (length == 126) {
        length = (static_cast<std::size_t>(data[position]) << 8) | data[position + 1];
        position += 2;
      } else if (length == 127) {
        length = 0;
        for (int index = 0; index < 8; ++index) length = (length << 8) | data[position + static_cast<std::size_t>(index)];
        position += 8;
      }
      std::array<std::uint8_t, 4> key{0, 0, 0, 0};
      if (parsed.masked) {
        std::copy(data.begin() + static_cast<std::ptrdiff_t>(position), data.begin() + static_cast<std::ptrdiff_t>(position + 4), key.begin());
        position += 4;
      }
      parsed.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(position), data.begin() + static_cast<std::ptrdiff_t>(position + length));
      for (std::size_t index = 0; index < parsed.payload.size(); ++index) parsed.payload[index] ^= key[index % 4];
      position += length;
      out.push_back(std::move(parsed));
    }
    return out;
  }

  std::string upgradeRequest(const std::string& path = "/x", const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    std::string out = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
    for (const auto& [name, value] : extra) out += name + ": " + value + "\r\n";
    return out + "\r\n";
  }

  std::string repeat(const std::string& text, int count) {
    std::string out;
    for (int index = 0; index < count; ++index) out += text;
    return out;
  }

  std::string replace(std::string text, const std::string& from, const std::string& to) {
    const std::size_t position = text.find(from);
    if (position != std::string::npos) text.replace(position, from.size(), to);
    return text;
  }

  // ---- a raw peer --------------------------------------------------------------------

  struct RawPeer : std::enable_shared_from_this<RawPeer> {
    explicit RawPeer(Generic stream) : socket(std::move(stream)) {}
    Generic socket;
    ws::Bytes received;
    bool ended = false;
    std::function<void()> onReceive;
    std::array<std::uint8_t, 65536> buffer{};
    void start() {
      socket.async_read_some(asio::buffer(buffer), [self = shared_from_this()](const boost::system::error_code& error, std::size_t count) {
        if (error) {
          // A net.Socket (allowHalfOpen false) ends its side when the peer does.
          self->ended = true;
          self->end();
          if (self->onReceive) self->onReceive();
          return;
        }
        self->received.insert(self->received.end(), self->buffer.begin(), self->buffer.begin() + static_cast<std::ptrdiff_t>(count));
        if (self->onReceive) self->onReceive();
        self->start();
      });
    }
    void write(const ws::Bytes& data) {
      boost::system::error_code ignored;
      asio::write(socket, asio::buffer(data), ignored);
    }
    void write(const std::string& data) { write(bytesOf(data)); }
    void end() {
      boost::system::error_code ignored;
      socket.shutdown(asio::socket_base::shutdown_send, ignored);
    }
    void destroy() {
      boost::system::error_code ignored;
      socket.close(ignored);
    }
    // The bytes after the HTTP head, and the head.
    std::string head() const {
      const std::string text(received.begin(), received.end());
      const std::size_t end = text.find("\r\n\r\n");
      return end == std::string::npos ? text : text.substr(0, end + 4);
    }
    ws::Bytes after() const {
      const std::string text(received.begin(), received.end());
      const std::size_t end = text.find("\r\n\r\n");
      return end == std::string::npos ? ws::Bytes{} : ws::Bytes(received.begin() + static_cast<std::ptrdiff_t>(end + 4), received.end());
    }
  };

  rt::Task<std::shared_ptr<RawPeer>> connectRaw(int port) {
    asio::ip::tcp::socket tcp(rt::executor());
    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port));
    auto token = asio::use_awaitable;
    co_await tcp.async_connect(endpoint, token);
    auto peer = std::make_shared<RawPeer>(Generic(std::move(tcp)));
    peer->start();
    co_return peer;
  }

  // ---- the HTTP server stage of the tests ------------------------------------------------
  // Accepts on 127.0.0.1 (OS-assigned port) or a Unix socket, reads one request
  // head and hands the upgrade over, as src/runtime/http.ts's "upgrade" event does.

  using UpgradeHandler = std::function<void(const ws::UpgradeRequest& request, ws::UpgradeSocket& socket, ws::Bytes head)>;

  class TestHttpServer : public std::enable_shared_from_this<TestHttpServer> {
  public:
    explicit TestHttpServer(UpgradeHandler handler) : handler_(std::move(handler)) {}
    int port = 0;
    std::string path;

    void listenTcp() {
      tcp_ = std::make_unique<asio::ip::tcp::acceptor>(rt::executor(), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
      port = tcp_->local_endpoint().port();
      acceptTcp();
    }
    void listenUnix(const std::string& socketPath) {
      path = socketPath;
      local_ = std::make_unique<asio::local::stream_protocol::acceptor>(rt::executor(), asio::local::stream_protocol::endpoint(socketPath));
      acceptLocal();
    }
    void close() {
      boost::system::error_code ignored;
      if (tcp_) tcp_->close(ignored);
      if (local_) local_->close(ignored);
    }

  private:
    void acceptTcp() {
      tcp_->async_accept([self = shared_from_this()](const boost::system::error_code& error, asio::ip::tcp::socket socket) {
        if (error) return;
        self->readHead(std::make_shared<Generic>(std::move(socket)), std::make_shared<std::string>());
        self->acceptTcp();
      });
    }
    void acceptLocal() {
      local_->async_accept([self = shared_from_this()](const boost::system::error_code& error, asio::local::stream_protocol::socket socket) {
        if (error) return;
        self->readHead(std::make_shared<Generic>(std::move(socket)), std::make_shared<std::string>());
        self->acceptLocal();
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
        const std::string head = received->substr(0, end);
        std::size_t lineEnd = head.find("\r\n");
        const std::string requestLine = head.substr(0, lineEnd);
        const std::size_t firstSpace = requestLine.find(' ');
        const std::size_t secondSpace = requestLine.find(' ', firstSpace + 1);
        request.method = requestLine.substr(0, firstSpace);
        request.url = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        std::size_t position = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
        while (position < head.size()) {
          lineEnd = head.find("\r\n", position);
          if (lineEnd == std::string::npos) lineEnd = head.size();
          const std::string line = head.substr(position, lineEnd - position);
          position = lineEnd + 2;
          const std::size_t colon = line.find(':');
          std::string value = line.substr(colon + 1);
          while (!value.empty() && value.front() == ' ') value.erase(0, 1);
          std::string latin;
          for (const char c : value) rt::appendCodePoint(latin, static_cast<unsigned char>(c));
          request.rawHeaders.emplace_back(line.substr(0, colon), latin);
        }
        ws::Bytes rest(received->begin() + static_cast<std::ptrdiff_t>(end + 4), received->end());
        ws::UpgradeSocket upgrade{std::move(*socket)};
        self->handler_(request, upgrade, std::move(rest));
      });
    }

    UpgradeHandler handler_;
    std::unique_ptr<asio::ip::tcp::acceptor> tcp_;
    std::unique_ptr<asio::local::stream_protocol::acceptor> local_;
  };

  // src/runtime/http-upgrade.ts in miniature: a noServer WebSocketServer per
  // upgrade, handleUpgrade, then close().
  std::shared_ptr<TestHttpServer> startServer(ws::ServerOptions options, std::function<void(std::shared_ptr<ws::WebSocket>)> accept,
                                              std::function<void(ws::WebSocketServer&)> configure = nullptr) {
    options.noServer = true;
    options.clientTracking = false;
    options.perMessageDeflate = false;
    auto server = std::make_shared<TestHttpServer>([options, accept, configure](const ws::UpgradeRequest& request, ws::UpgradeSocket& socket, ws::Bytes head) {
      auto upgradeServer = std::make_shared<ws::WebSocketServer>(options);
      if (configure) configure(*upgradeServer);
      upgradeServer->handleUpgrade(request, socket, std::move(head), [accept](std::shared_ptr<ws::WebSocket> websocket, const ws::UpgradeRequest&) { accept(websocket); });
      upgradeServer->close();
    });
    return server;
  }

  ws::ClientOptions noDeflate() {
    ws::ClientOptions options;
    options.perMessageDeflate = false;
    return options;
  }

  // Unix sockets live in one temporary directory, removed when the binary exits.
  struct TempDirectory {
    std::string path;
    ~TempDirectory() {
      if (!path.empty()) rt::fs::rmSync(path, {.recursive = true, .force = true});
    }
  };
  std::string tempSocketPath() {
    static TempDirectory directory;
    if (directory.path.empty()) directory.path = rt::fs::mkdtempSync(rt::os::tmpdir() + "/uos-rt-ws-");
    static int counter = 0;
    return directory.path + "/s" + std::to_string(++counter) + ".sock";
  }
}

// ---- headers ----------------------------------------------------------------------------

TEST("upgrade request headers follow node:http's join rules") {
  ws::UpgradeRequest request;
  request.rawHeaders = {{"Host", "one"},          {"Host", "two"},     {"Upgrade", "websocket"}, {"upgrade", "other"},  {"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="},
                        {"sec-websocket-key", "second=="}, {"X-Dup", "a"}, {"x-dup", "b"},     {"Cookie", "a=1"},      {"Cookie", "b=2"},
                        {"Authorization", "one"}, {"Authorization", "two"}, {"Sec-WebSocket-Protocol", "chat"}, {"Sec-WebSocket-Protocol", "other"}};
  // Bun's IncomingMessage.headers for the same raw headers.
  EXPECT_EQ(request.header("host"), std::optional<std::string>("one"));
  EXPECT_EQ(request.header("upgrade"), std::optional<std::string>("websocket, other"));
  EXPECT_EQ(request.header("sec-websocket-key"), std::optional<std::string>("dGhlIHNhbXBsZSBub25jZQ==, second=="));
  EXPECT_EQ(request.header("x-dup"), std::optional<std::string>("a, b"));
  EXPECT_EQ(request.header("cookie"), std::optional<std::string>("a=1; b=2"));
  EXPECT_EQ(request.header("authorization"), std::optional<std::string>("one"));
  EXPECT_EQ(request.header("sec-websocket-protocol"), std::optional<std::string>("chat, other"));
  EXPECT(!request.header("origin"));
}

TEST("EventEmitter: once, removeListener of the latest registration, emit snapshot, unhandled error") {
  auto socket = std::make_shared<ws::WebSocket>(nullptr);
  std::vector<std::string> calls;
  ws::Listener first([&] { calls.push_back("first"); });
  ws::Listener second([&] { calls.push_back("second"); });
  socket->on("x", first);
  socket->once("x", second);
  socket->on("x", first);
  socket->on("x", [&] {
    calls.push_back("adds");
    socket->on("x", [&] { calls.push_back("late"); });
  });
  EXPECT_EQ(socket->listenerCount("x"), std::size_t(4));
  EXPECT(socket->emit("x"));
  EXPECT_EQ(calls, (std::vector<std::string>{"first", "second", "first", "adds"}));
  calls.clear();
  socket->removeListener("x", first);
  socket->emit("x");
  EXPECT_EQ(calls, (std::vector<std::string>{"first", "adds", "late"}));
  EXPECT(!socket->emit("nothing"));
  ws::EventArgs failure;
  failure.error = std::make_exception_ptr(rt::JsError("boom"));
  EXPECT_THROWS_MESSAGE(socket->emit("error", failure), rt::JsError, "boom");
}

// ---- client <-> server loops (TCP and Unix sockets) ------------------------------------------

namespace {
#define WAIT_UNTIL(expression) WAIT_UNTIL_FOR(expression, 2000)
#define WAIT_UNTIL_FOR(expression, ms)                                 \
  do {                                                                 \
    std::function<bool()> uosCondition = [&] { return (expression); }; \
    const double uosLimit = ms;                                        \
    co_await waitFor(uosCondition, uosLimit);                          \
  } while (0)

  struct LoopCase {
    std::string name;
    // Runs on the server socket once accepted (listeners, scheduled actions).
    std::function<void(std::shared_ptr<ws::WebSocket> socket, Log log)> accept;
    // Runs on the client once it is open.
    std::function<void(std::shared_ptr<ws::WebSocket> socket, Log log)> onOpen;
    std::string client;
    std::string server;
    double maxPayload = 1024;
  };

  ws::ServerOptions serverOptions(double maxPayload) {
    ws::ServerOptions options;
    options.maxPayload = maxPayload;
    return options;
  }

  rt::Task<void> runLoopCase(LoopCase loopCase, bool overUnix) {
    auto log = newLog();
    auto accept = loopCase.accept;
    std::function<void(std::shared_ptr<ws::WebSocket>)> onAccept = [log, accept](std::shared_ptr<ws::WebSocket> socket) {
      record(log, "server", socket);
      if (accept) accept(socket, log);
    };
    auto server = startServer(serverOptions(loopCase.maxPayload), onAccept);
    std::string url;
    if (overUnix) {
      server->listenUnix(tempSocketPath());
      url = "ws+unix://" + server->path + ":/path";
    } else {
      server->listenTcp();
      url = "ws://127.0.0.1:" + std::to_string(server->port) + "/path";
    }
    auto client = ws::WebSocket::create(url, noDeflate());
    record(log, "client", client);
    auto onOpen = loopCase.onOpen;
    client->once("open", [client, log, onOpen] {
      if (onOpen) onOpen(client, log);
    });
    WAIT_UNTIL(side(log, "client") == loopCase.client && side(log, "server") == loopCase.server);
    co_await rt::sleep(30);
    const std::string label = loopCase.name + (overUnix ? " (unix)" : " (tcp)");
    if (side(log, "client") != loopCase.client) std::fprintf(stderr, "    case %s\n", label.c_str());
    EXPECT_EQ(side(log, "client"), loopCase.client);
    if (side(log, "server") != loopCase.server) std::fprintf(stderr, "    case %s\n", label.c_str());
    EXPECT_EQ(side(log, "server"), loopCase.server);
    client->terminate();
    server->close();
  }

  const std::string OPENED = R"(["client","upgrade",101,0],["client","open",1])";

  std::vector<LoopCase> loopCases() {
    std::vector<LoopCase> cases;
    cases.push_back({"text echo",
                     [](std::shared_ptr<ws::WebSocket> socket, Log) {
                       ws::WebSocket* raw = socket.get();
                       socket->on("message", [raw](const ws::EventArgs& args) {
                         const auto& bytes = std::get<ws::Bytes>(args.data);
                         if (args.isBinary) raw->send(args.data);
                         else raw->send(std::string(bytes.begin(), bytes.end()));
                       });
                     },
                     [](std::shared_ptr<ws::WebSocket> socket, Log) { socket->send(std::string("h\xc3\xa9llo")); },
                     "[" + OPENED + R"(,["client","message",{"buffer":"68c3a96c6c6f"},false,1]])",
                     R"([["server","message",{"buffer":"68c3a96c6c6f"},false,1]])"});
    cases.push_back({"binary echo",
                     [](std::shared_ptr<ws::WebSocket> socket, Log) {
                       ws::WebSocket* raw = socket.get();
                       socket->on("message", [raw](const ws::EventArgs& args) { raw->send(args.data); });
                     },
                     [](std::shared_ptr<ws::WebSocket> socket, Log) { socket->send(ws::Bytes{0, 1, 2, 255}); },
                     "[" + OPENED + R"(,["client","message",{"buffer":"000102ff"},true,1]])",
                     R"([["server","message",{"buffer":"000102ff"},true,1]])"});
    cases.push_back({"client close 1000", nullptr, [](std::shared_ptr<ws::WebSocket> socket, Log) { socket->close(1000, std::string("bye")); },
                     "[" + OPENED + R"(,["client","close",1000,{"buffer":"627965"},3]])", R"([["server","close",1000,{"buffer":"627965"},3]])"});
    cases.push_back({"server close 1001",
                     [](std::shared_ptr<ws::WebSocket> socket, Log) {
                       rt::setTimeout([socket] { socket->close(1001, std::string("Server shutting down")); }, 20);
                     },
                     nullptr, "[" + OPENED + R"(,["client","close",1001,{"buffer":"536572766572207368757474696e6720646f776e"},3]])",
                     R"([["server","close",1001,{"buffer":"536572766572207368757474696e6720646f776e"},3]])"});
    cases.push_back({"client close without code", nullptr, [](std::shared_ptr<ws::WebSocket> socket, Log) { socket->close(); },
                     "[" + OPENED + R"(,["client","close",1005,{"buffer":""},3]])", R"([["server","close",1005,{"buffer":""},3]])"});
    cases.push_back({"client terminate", nullptr, [](std::shared_ptr<ws::WebSocket> socket, Log) { rt::setTimeout([socket] { socket->terminate(); }, 20); },
                     "[" + OPENED + R"(,["client","close",1006,{"buffer":""},3]])", R"([["server","close",1006,{"buffer":""},3]])"});
    cases.push_back({"server terminate", [](std::shared_ptr<ws::WebSocket> socket, Log) { rt::setTimeout([socket] { socket->terminate(); }, 20); }, nullptr,
                     "[" + OPENED + R"(,["client","close",1006,{"buffer":""},3]])", R"([["server","close",1006,{"buffer":""},3]])"});
    cases.push_back({"server ping, automatic pong", [](std::shared_ptr<ws::WebSocket> socket, Log) { rt::setTimeout([socket] { socket->ping(std::string("pp")); }, 20); }, nullptr,
                     "[" + OPENED + R"(,["client","ping",{"buffer":"7070"},1]])", R"([["server","pong",{"buffer":"7070"},1]])"});
    cases.push_back({"client ping and pong", nullptr,
                     [](std::shared_ptr<ws::WebSocket> socket, Log) {
                       socket->ping();
                       socket->pong(std::string("x"));
                     },
                     "[" + OPENED + R"(,["client","pong",{"buffer":""},1]])", R"([["server","ping",{"buffer":""},1],["server","pong",{"buffer":"78"},1]])"});
    const std::string payloadError = R"({"error":"RangeError","message":"Max payload size exceeded","code":"WS_ERR_UNSUPPORTED_MESSAGE_LENGTH","errno":null,"syscall":null})";
    cases.push_back({"payload limit", nullptr, [](std::shared_ptr<ws::WebSocket> socket, Log) { socket->send(ws::Bytes(1025)); },
                     "[" + OPENED + R"(,["client","close",1009,{"buffer":""},3]])", R"([["server","error",)" + payloadError + R"(,2],["server","close",1006,{"buffer":""},3]])"});
    cases.push_back({"payload limit across fragments", nullptr,
                     [](std::shared_ptr<ws::WebSocket> socket, Log) {
                       socket->send(ws::Bytes(600), ws::SendOptions{.fin = false});
                       socket->send(ws::Bytes(600), ws::SendOptions{.fin = true});
                     },
                     "[" + OPENED + R"(,["client","close",1009,{"buffer":""},3]])", R"([["server","error",)" + payloadError + R"(,2],["server","close",1006,{"buffer":""},3]])"});
    cases.push_back({"payload at the limit",
                     [](std::shared_ptr<ws::WebSocket> socket, Log) {
                       ws::WebSocket* raw = socket.get();
                       socket->on("message", [raw](const ws::EventArgs& args) { raw->send(std::to_string(std::get<ws::Bytes>(args.data).size())); });
                     },
                     [](std::shared_ptr<ws::WebSocket> socket, Log) { socket->send(ws::Bytes(1024)); }, "[" + OPENED + R"(,["client","message",{"buffer":"31303234"},false,1]])",
                     R"([["server","message",{"buffer":")" + std::string(2048, '0') + R"("},true,1]])"});
    cases.push_back({"fragmented send", nullptr,
                     [](std::shared_ptr<ws::WebSocket> socket, Log) {
                       socket->send(std::string("Hel"), ws::SendOptions{.fin = false});
                       socket->send(std::string("lo"), ws::SendOptions{.fin = false});
                       socket->send(std::string("!"));
                     },
                     "[" + OPENED + "]", R"([["server","message",{"buffer":"48656c6c6f21"},false,1]])"});
    const std::string closing = R"j({"error":"Error","message":"WebSocket is not open: readyState 2 (CLOSING)","code":null,"errno":null,"syscall":null})j";
    const std::string closed = R"j({"error":"Error","message":"WebSocket is not open: readyState 3 (CLOSED)","code":null,"errno":null,"syscall":null})j";
    cases.push_back({"send after close", nullptr,
                     [](std::shared_ptr<ws::WebSocket> socket, Log log) {
                       socket->close(1000);
                       push(log, {"client", "bufferedAmount-after-close", static_cast<std::int64_t>(socket->bufferedAmount()), socket->readyState()});
                       socket->send(std::string("abc"));
                       push(log, {"client", "bufferedAmount-after-send", static_cast<std::int64_t>(socket->bufferedAmount()), socket->readyState()});
                       ws::WebSocket* raw = socket.get();
                       socket->send(std::string("defg"), [log, raw](rt::Reason error) { push(log, {"client", "send-callback", describeError(error), raw->readyState()}); });
                       socket->ping(ws::Data(std::string("p")), std::nullopt, [log, raw](rt::Reason error) { push(log, {"client", "ping-callback", describeError(error), raw->readyState()}); });
                       push(log, {"client", "bufferedAmount-after-send2", static_cast<std::int64_t>(socket->bufferedAmount()), socket->readyState()});
                       rt::setTimeout(
                         [socket, log] {
                           ws::WebSocket* inner = socket.get();
                           socket->send(std::string("hij"), [log, inner](rt::Reason error) { push(log, {"client", "send-callback-closed", describeError(error), inner->readyState()}); });
                           push(log, {"client", "bufferedAmount-closed", static_cast<std::int64_t>(socket->bufferedAmount()), socket->readyState()});
                         },
                         100);
                     },
                     "[" + OPENED +
                       R"(,["client","bufferedAmount-after-close",0,2],["client","bufferedAmount-after-send",3,2],["client","bufferedAmount-after-send2",8,2],["client","send-callback",)" +
                       closing + R"(,2],["client","ping-callback",)" + closing + R"(,2],["client","close",1000,{"buffer":""},3],["client","bufferedAmount-closed",11,3],["client","send-callback-closed",)" +
                       closed + ",3]]",
                     R"([["server","close",1000,{"buffer":""},3]])"});
    return cases;
  }
}

ASYNC_TEST("client and server loops match ws over TCP and Unix sockets") {
  for (const bool overUnix : {false, true}) {
    for (auto& loopCase : loopCases()) co_await runLoopCase(loopCase, overUnix);
  }
}

// ---- raw client -> ws server: frames and handshakes on the wire ----------------------------------

namespace {
  const std::string SWITCHED = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";

  enum class Reply { none, echoText, describeBinary, closeAfter20 };

  struct ServerWireCase {
    std::string name;
    std::vector<ws::Bytes> chunks;
    std::string response;
    std::string after;
    bool ended;
    std::string serverLog;
    Reply reply = Reply::none;
    std::string request = upgradeRequest();
  };

  std::string frameError(const std::string& name, const std::string& message, const std::string& code) {
    return R"j([["server","error",{"error":")j" + name + R"j(","message":")j" + message + R"j(","code":")j" + code + R"j(","errno":null,"syscall":null},2],["server","close",1006,{"buffer":""},3]])j";
  }

  std::string errorResponse(int status, const std::string& text, const std::string& body, const std::string& extra = "") {
    return "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nConnection: close\r\nContent-Type: text/html\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n";
  }

  std::vector<ServerWireCase> serverWireCases() {
    const auto masked = [](int opcode, ws::Bytes payload, bool fin = true) { return frame(opcode, std::move(payload), FrameOptions{.fin = fin, .mask = MASK}); };
    std::vector<ServerWireCase> cases;
    cases.push_back({"fragmented text with a ping between the fragments",
                     {masked(1, bytesOf("Hel"), false), masked(9, bytesOf("pi")), masked(0, bytesOf("lo"))},
                     SWITCHED, "8a027069810548656c6c6f", false,
                     R"([["server","ping",{"buffer":"7069"},1],["server","message",{"buffer":"48656c6c6f"},false,1]])", Reply::echoText});
    cases.push_back({"unmasked frame from a client", {frame(1, "abc")}, SWITCHED, "880203ea", true, frameError("RangeError", "Invalid WebSocket frame: MASK must be set", "WS_ERR_EXPECTED_MASK")});
    cases.push_back({"invalid UTF-8 text", {masked(1, ws::Bytes{0xc3, 0x28})}, SWITCHED, "880203ef", true, frameError("Error", "Invalid WebSocket frame: invalid UTF-8 sequence", "WS_ERR_INVALID_UTF8")});
    cases.push_back({"invalid UTF-8 across fragments", {masked(1, ws::Bytes{0xe2, 0x82}, false), masked(0, ws::Bytes{0x28})}, SWITCHED, "880203ef", true,
                     frameError("Error", "Invalid WebSocket frame: invalid UTF-8 sequence", "WS_ERR_INVALID_UTF8")});
    cases.push_back({"a code point split across fragments", {masked(1, ws::Bytes{0xe2, 0x82}, false), masked(0, ws::Bytes{0xac})}, SWITCHED, "8103e282ac", false,
                     R"([["server","message",{"buffer":"e282ac"},false,1]])", Reply::echoText});
    cases.push_back({"close code 1005 on the wire", {masked(8, ws::Bytes{0x03, 0xed})}, SWITCHED, "880203ea", true,
                     frameError("RangeError", "Invalid WebSocket frame: invalid status code 1005", "WS_ERR_INVALID_CLOSE_CODE")});
    cases.push_back({"close payload of one byte", {masked(8, ws::Bytes{0x03})}, SWITCHED, "880203ea", true,
                     frameError("RangeError", "Invalid WebSocket frame: invalid payload length 1", "WS_ERR_INVALID_CONTROL_PAYLOAD_LENGTH")});
    cases.push_back({"close reason that is not UTF-8", {masked(8, ws::Bytes{0x03, 0xe8, 0xff})}, SWITCHED, "880203ef", true,
                     frameError("Error", "Invalid WebSocket frame: invalid UTF-8 sequence", "WS_ERR_INVALID_UTF8")});
    cases.push_back({"close 1014 with a reason", {masked(8, ws::Bytes{0x03, 0xf6, 'o', 'k'})}, SWITCHED, "880403f66f6b", true,
                     R"([["server","close",1014,{"buffer":"6f6b"},3]])"});
    cases.push_back({"close 1014 with an invalid reason", {masked(8, ws::Bytes{0x03, 0xf6, 0xff})}, SWITCHED, "880203ef", true,
                     frameError("Error", "Invalid WebSocket frame: invalid UTF-8 sequence", "WS_ERR_INVALID_UTF8")});
    cases.push_back({"close 4000 with a reason", {masked(8, ws::Bytes{0x0f, 0xa0, 'c', 'u', 's', 't', 'o', 'm'})}, SWITCHED, "88080fa0637573746f6d", true,
                     R"([["server","close",4000,{"buffer":"637573746f6d"},3]])"});
    cases.push_back({"empty close frame", {masked(8, ws::Bytes{})}, SWITCHED, "8800", true, R"([["server","close",1005,{"buffer":""},3]])"});
    cases.push_back({"opcode 3", {masked(3, bytesOf("x"))}, SWITCHED, "880203ea", true, frameError("RangeError", "Invalid WebSocket frame: invalid opcode 3", "WS_ERR_INVALID_OPCODE")});
    cases.push_back({"opcode 11", {masked(11, bytesOf("x"))}, SWITCHED, "880203ea", true, frameError("RangeError", "Invalid WebSocket frame: invalid opcode 11", "WS_ERR_INVALID_OPCODE")});
    cases.push_back({"continuation without a message", {masked(0, bytesOf("x"))}, SWITCHED, "880203ea", true, frameError("RangeError", "Invalid WebSocket frame: invalid opcode 0", "WS_ERR_INVALID_OPCODE")});
    cases.push_back({"text frame inside a fragmented message", {masked(1, bytesOf("a"), false), masked(1, bytesOf("b"))}, SWITCHED, "880203ea", true,
                     frameError("RangeError", "Invalid WebSocket frame: invalid opcode 1", "WS_ERR_INVALID_OPCODE")});
    cases.push_back({"RSV1 set", {frame(1, "x", FrameOptions{.mask = MASK, .rsv = 0x40})}, SWITCHED, "880203ea", true,
                     frameError("RangeError", "Invalid WebSocket frame: RSV1 must be clear", "WS_ERR_UNEXPECTED_RSV_1")});
    cases.push_back({"RSV2 set", {frame(1, "x", FrameOptions{.mask = MASK, .rsv = 0x20})}, SWITCHED, "880203ea", true,
                     frameError("RangeError", "Invalid WebSocket frame: RSV2 and RSV3 must be clear", "WS_ERR_UNEXPECTED_RSV_2_3")});
    cases.push_back({"control frame without FIN", {masked(9, bytesOf("x"), false)}, SWITCHED, "880203ea", true, frameError("RangeError", "Invalid WebSocket frame: FIN must be set", "WS_ERR_EXPECTED_FIN")});
    cases.push_back({"ping of 126 bytes", {masked(9, ws::Bytes(126))}, SWITCHED, "880203ea", true,
                     frameError("RangeError", "Invalid WebSocket frame: invalid payload length 126", "WS_ERR_INVALID_CONTROL_PAYLOAD_LENGTH")});
    cases.push_back({"64-bit length beyond 2^53", {ws::Bytes{0x82, 0xff, 0x00, 0x20, 0, 0, 0, 0, 0, 0}}, SWITCHED, "880203f1", true,
                     frameError("RangeError", "Unsupported WebSocket frame: payload length > 2^53 - 1", "WS_ERR_UNSUPPORTED_DATA_PAYLOAD_LENGTH")});
    cases.push_back({"payload limit from the header alone", {ws::Bytes{0x82, 0xfe, 0x04, 0x01, 0x11, 0x22, 0x33, 0x44}}, SWITCHED, "880203f1", true,
                     frameError("RangeError", "Max payload size exceeded", "WS_ERR_UNSUPPORTED_MESSAGE_LENGTH")});
    cases.push_back({"zero masking key", {frame(1, "zz", FrameOptions{.mask = std::array<std::uint8_t, 4>{0, 0, 0, 0}})}, SWITCHED, "81027a7a", false,
                     R"([["server","message",{"buffer":"7a7a"},false,1]])", Reply::echoText});
    cases.push_back({"empty binary message", {masked(2, ws::Bytes{})}, SWITCHED, "810e6c656e3d302c62696e3d74727565", false, R"([["server","message",{"buffer":""},true,1]])", Reply::describeBinary});
    cases.push_back({"server close 1001 frame", {}, SWITCHED, "881603e9536572766572207368757474696e6720646f776e", false, "[]", Reply::closeAfter20});
    cases.push_back({"POST upgrade", {}, errorResponse(405, "Method Not Allowed", "Invalid HTTP method"), hex(bytesOf("Invalid HTTP method")), true, "[]", Reply::none,
                     replace(upgradeRequest(), "GET", "POST")});
    cases.push_back({"Upgrade: h2c", {}, errorResponse(400, "Bad Request", "Invalid Upgrade header"), hex(bytesOf("Invalid Upgrade header")), true, "[]", Reply::none,
                     replace(upgradeRequest(), "Upgrade: websocket", "Upgrade: h2c")});
    cases.push_back({"missing key", {}, errorResponse(400, "Bad Request", "Missing or invalid Sec-WebSocket-Key header"), hex(bytesOf("Missing or invalid Sec-WebSocket-Key header")), true, "[]",
                     Reply::none, replace(upgradeRequest(), "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n", "")});
    cases.push_back({"malformed key", {}, errorResponse(400, "Bad Request", "Missing or invalid Sec-WebSocket-Key header"), hex(bytesOf("Missing or invalid Sec-WebSocket-Key header")), true, "[]",
                     Reply::none, replace(upgradeRequest(), "dGhlIHNhbXBsZSBub25jZQ==", "abc")});
    cases.push_back({"version 7", {}, errorResponse(400, "Bad Request", "Missing or invalid Sec-WebSocket-Version header", "Sec-WebSocket-Version: 13, 8\r\n"),
                     hex(bytesOf("Missing or invalid Sec-WebSocket-Version header")), true, "[]", Reply::none, replace(upgradeRequest(), "Sec-WebSocket-Version: 13", "Sec-WebSocket-Version: 7")});
    cases.push_back({"version 8", {}, SWITCHED, "", false, "[]", Reply::none, replace(upgradeRequest(), "Sec-WebSocket-Version: 13", "Sec-WebSocket-Version: 8")});
    cases.push_back({"subprotocols offered",
                     {},
                     replace(SWITCHED, "\r\n\r\n", "\r\nSec-WebSocket-Protocol: chat\r\n\r\n"),
                     "",
                     false,
                     "[]",
                     Reply::none,
                     upgradeRequest("/x", {{"Sec-WebSocket-Protocol", "chat, superchat"}})});
    cases.push_back({"malformed subprotocols", {}, errorResponse(400, "Bad Request", "Invalid Sec-WebSocket-Protocol header"), hex(bytesOf("Invalid Sec-WebSocket-Protocol header")), true, "[]",
                     Reply::none, upgradeRequest("/x", {{"Sec-WebSocket-Protocol", "a b"}})});
    cases.push_back({"extension offer ignored", {}, SWITCHED, "", false, "[]", Reply::none, upgradeRequest("/x", {{"Sec-WebSocket-Extensions", "permessage-deflate"}})});
    return cases;
  }

  rt::Task<void> runServerWireCase(ServerWireCase wireCase) {
    auto log = newLog();
    const Reply reply = wireCase.reply;
    std::function<void(std::shared_ptr<ws::WebSocket>)> onAccept = [log, reply](std::shared_ptr<ws::WebSocket> socket) {
      record(log, "server", socket);
      ws::WebSocket* raw = socket.get();
      if (reply == Reply::echoText) {
        socket->on("message", [raw](const ws::EventArgs& args) {
          const auto& bytes = std::get<ws::Bytes>(args.data);
          raw->send(std::string(bytes.begin(), bytes.end()));
        });
      } else if (reply == Reply::describeBinary) {
        socket->on("message", [raw](const ws::EventArgs& args) {
          raw->send("len=" + std::to_string(std::get<ws::Bytes>(args.data).size()) + ",bin=" + (args.isBinary ? "true" : "false"));
        });
      } else if (reply == Reply::closeAfter20) {
        rt::setTimeout([socket] { socket->close(1001, std::string("Server shutting down")); }, 20);
      }
    };
    auto server = startServer(serverOptions(1024), onAccept);
    server->listenTcp();
    std::shared_ptr<RawPeer> peer = co_await connectRaw(server->port);
    peer->write(wireCase.request);
    WAIT_UNTIL(peer->head().find("\r\n\r\n") != std::string::npos);
    co_await rt::sleep(20);
    for (const auto& chunk : wireCase.chunks) {
      peer->write(chunk);
      co_await rt::sleep(10);
    }
    WAIT_UNTIL(hex(peer->after()) == wireCase.after && peer->ended == wireCase.ended && json::serialize(*log) == wireCase.serverLog);
    co_await rt::sleep(30);
    if (peer->head() != wireCase.response || hex(peer->after()) != wireCase.after || peer->ended != wireCase.ended || json::serialize(*log) != wireCase.serverLog)
      std::fprintf(stderr, "    case %s\n", wireCase.name.c_str());
    EXPECT_EQ(peer->head(), wireCase.response);
    EXPECT_EQ(hex(peer->after()), wireCase.after);
    EXPECT_EQ(peer->ended, wireCase.ended);
    EXPECT_EQ(json::serialize(*log), wireCase.serverLog);
    peer->destroy();
    server->close();
  }
}

ASYNC_TEST("ws server: frames and handshakes from a raw client match ws byte for byte") {
  for (auto& wireCase : serverWireCases()) co_await runServerWireCase(wireCase);
}

// ---- raw server -> ws client -------------------------------------------------------------------

namespace {
  // Accepts one connection on 127.0.0.1, reads the request head and calls
  // `respond(peer, acceptValue)`; later bytes from the client stay in peer->received.
  class RawServer : public std::enable_shared_from_this<RawServer> {
  public:
    using Respond = std::function<void(const std::shared_ptr<RawPeer>& peer, const std::string& accept)>;
    explicit RawServer(Respond respond) : respond_(std::move(respond)), acceptor_(rt::executor(), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {}
    int port() const { return acceptor_.local_endpoint().port(); }
    std::shared_ptr<RawPeer> peer;
    std::string request;
    void start() {
      acceptor_.async_accept([self = shared_from_this()](const boost::system::error_code& error, asio::ip::tcp::socket socket) {
        if (error) return;
        self->peer = std::make_shared<RawPeer>(Generic(std::move(socket)));
        std::weak_ptr<RawServer> weak = self;
        self->peer->onReceive = [weak] {
          auto server = weak.lock();
          if (!server || !server->request.empty()) return;
          const std::string text(server->peer->received.begin(), server->peer->received.end());
          const std::size_t end = text.find("\r\n\r\n");
          if (end == std::string::npos) return;
          server->request = text.substr(0, end + 4);
          server->peer->received.erase(server->peer->received.begin(), server->peer->received.begin() + static_cast<std::ptrdiff_t>(end + 4));
          const std::size_t keyAt = server->request.find("Sec-WebSocket-Key: ");
          const std::string key = keyAt == std::string::npos ? "" : server->request.substr(keyAt + 19, server->request.find("\r\n", keyAt) - keyAt - 19);
          const std::string accept = rt::crypto::createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
          if (server->respond_) server->respond_(server->peer, accept);
        };
        self->peer->start();
      });
    }
    void close() {
      boost::system::error_code ignored;
      acceptor_.close(ignored);
      if (peer) peer->destroy();
    }

  private:
    Respond respond_;
    asio::ip::tcp::acceptor acceptor_;
  };

  std::string switching(const std::string& accept, const std::string& extra = "") {
    return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n" + extra + "\r\n";
  }

  struct ClientWireCase {
    std::string name;
    RawServer::Respond respond;
    std::string clientLog;
    // Frames the client sent after the handshake: first byte, masked, payload (hex).
    std::string frames = "[]";
    ws::ClientOptions options = noDeflate();
    // Runs right after the client is created.
    std::function<void(std::shared_ptr<ws::WebSocket>, Log)> after = nullptr;
    bool recordUnexpected = true;
    // How long the log must then stay as expected.
    double settleMs = 40;
  };

  std::string describeFrames(const ws::Bytes& received) {
    json::array out;
    for (const auto& parsed : parseFrames(received)) out.push_back(json::array{parsed.first, parsed.masked, hex(parsed.payload)});
    return json::serialize(out);
  }

  std::string clientError(const std::string& message, const std::string& extra = "") {
    return R"j(["client","error",{"error":"Error","message":")j" + message + R"j(","code":null,"errno":null,"syscall":null},2],["client","close",1006,{"buffer":""},3])j" + extra;
  }

  rt::Task<void> runClientWireCase(ClientWireCase wireCase) {
    auto log = newLog();
    auto server = std::make_shared<RawServer>(wireCase.respond);
    server->start();
    auto client = ws::WebSocket::create("ws://127.0.0.1:" + std::to_string(server->port()) + "/", wireCase.options);
    if (wireCase.recordUnexpected) {
      record(log, "client", client);
    } else {
      ws::WebSocket* raw = client.get();
      client->on("error", [log, raw](const ws::EventArgs& args) { push(log, {"client", "error", describeError(args.error), raw->readyState()}); });
      client->on("close", [log, raw](const ws::EventArgs& args) { push(log, {"client", "close", args.code, describe(args.reason), raw->readyState()}); });
    }
    if (wireCase.after) wireCase.after(client, log);
    WAIT_UNTIL_FOR(side(log, "client") == wireCase.clientLog && (!server->peer || describeFrames(server->peer->received) == wireCase.frames), 1500);
    co_await rt::sleep(wireCase.settleMs);
    const std::string frames = server->peer ? describeFrames(server->peer->received) : "[]";
    if (side(log, "client") != wireCase.clientLog || frames != wireCase.frames) std::fprintf(stderr, "    case %s\n", wireCase.name.c_str());
    EXPECT_EQ(side(log, "client"), wireCase.clientLog);
    EXPECT_EQ(frames, wireCase.frames);
    client->terminate();
    server->close();
  }

  std::vector<ClientWireCase> clientWireCases() {
    std::vector<ClientWireCase> cases;
    const std::string upgraded = R"(["client","upgrade",101,0])";
    cases.push_back({"text and close from the server",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       peer->write(switching(accept));
                       ws::Bytes frames = frame(1, "hi");
                       const ws::Bytes close = frame(8, ws::Bytes{0x03, 0xe8, 'd', 'o', 'n', 'e'});
                       frames.insert(frames.end(), close.begin(), close.end());
                       peer->write(frames);
                     },
                     "[" + upgraded + R"(,["client","open",1],["client","message",{"buffer":"6869"},false,1],["client","close",1000,{"buffer":"646f6e65"},3]])",
                     R"([[136,true,"03e8646f6e65"]])"});
    cases.push_back({"wrong Sec-WebSocket-Accept", [](const std::shared_ptr<RawPeer>& peer, const std::string&) { peer->write(switching("nope")); },
                     "[" + upgraded + "," + clientError("Invalid Sec-WebSocket-Accept header") + "]"});
    cases.push_back({"101 without an Upgrade header",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       peer->write("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
                     },
                     R"([["client","unexpected-response",101,0]])"});
    cases.push_back({"unrequested subprotocol", [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) { peer->write(switching(accept, "Sec-WebSocket-Protocol: chat\r\n")); },
                     "[" + upgraded + "," + clientError("Server sent a subprotocol but none was requested") + "]"});
    cases.push_back({"unrequested extension",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) { peer->write(switching(accept, "Sec-WebSocket-Extensions: permessage-deflate\r\n")); },
                     "[" + upgraded + "," + clientError("Server sent a Sec-WebSocket-Extensions header but no extension was requested") + "]"});
    cases.push_back({"401 with a listener",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string&) {
                       peer->write("HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\nConnection: close\r\n\r\nunauthorized");
                       peer->end();
                     },
                     R"([["client","unexpected-response",401,0]])"});
    cases.push_back({"401 with a listener that terminates",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string&) {
                       peer->write("HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n\r\nunauthorized");
                       peer->end();
                     },
                     R"j([["client","unexpected-response",401,0],["client","terminate-called",0],["client","after-terminate",2],)j" +
                       clientError("WebSocket was closed before the connection was established") + "]",
                     "[]", noDeflate(),
                     [](std::shared_ptr<ws::WebSocket> socket, Log log) {
                       ws::WebSocket* raw = socket.get();
                       socket->on("unexpected-response", [socket, log, raw] {
                         rt::setTimeout(
                           [socket, log, raw] {
                             push(log, {"client", "terminate-called", raw->readyState()});
                             socket->terminate();
                             push(log, {"client", "after-terminate", raw->readyState()});
                           },
                           20);
                       });
                     }});
    cases.push_back({"200", [](const std::shared_ptr<RawPeer>& peer, const std::string&) { peer->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"); }, R"([["client","unexpected-response",200,0]])"});
    cases.push_back({"302", [](const std::shared_ptr<RawPeer>& peer, const std::string&) { peer->write("HTTP/1.1 302 Found\r\nLocation: /elsewhere\r\nContent-Length: 0\r\n\r\n"); },
                     R"([["client","unexpected-response",302,0]])"});
    cases.push_back({"masked frame from the server",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       peer->write(switching(accept));
                       peer->write(frame(1, "hi", FrameOptions{.mask = std::array<std::uint8_t, 4>{1, 2, 3, 4}}));
                     },
                     "[" + upgraded +
                       R"j(,["client","open",1],["client","error",{"error":"RangeError","message":"Invalid WebSocket frame: MASK must be clear","code":"WS_ERR_UNEXPECTED_MASK","errno":null,"syscall":null},2],["client","close",1006,{"buffer":""},3]])j",
                     R"([[136,true,"03ea"]])"});
    ws::ClientOptions small = noDeflate();
    small.maxPayload = 10;
    cases.push_back({"client maxPayload",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       peer->write(switching(accept));
                       peer->write(frame(2, ws::Bytes(11)));
                     },
                     "[" + upgraded +
                       R"j(,["client","open",1],["client","error",{"error":"RangeError","message":"Max payload size exceeded","code":"WS_ERR_UNSUPPORTED_MESSAGE_LENGTH","errno":null,"syscall":null},2],["client","close",1006,{"buffer":""},3]])j",
                     R"([[136,true,"03f1"]])", small});
    ws::ClientOptions quick = noDeflate();
    quick.handshakeTimeout = 100;
    cases.push_back({"handshake timeout", [](const std::shared_ptr<RawPeer>&, const std::string&) {}, "[" + clientError("Opening handshake has timed out") + "]", "[]", quick});
    ws::ClientOptions idle = noDeflate();
    idle.handshakeTimeout = 150;
    cases.push_back({"handshake timeout is an idle timer that stops at the response",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string&) {
                       auto parts = std::make_shared<std::vector<std::string>>();
                       const std::string response = "HTTP/1.1 401 Unauthorized\r\nX-A: 1\r\nX-B: 2\r\nX-C: 3\r\nX-D: 4\r\nContent-Length: 0\r\n\r\n";
                       for (std::size_t index = 0; index < response.size(); index += 6) parts->push_back(response.substr(index, 6));
                       auto next = std::make_shared<std::size_t>(0);
                       auto timer = std::make_shared<rt::Timeout>();
                       *timer = rt::setInterval(
                         [peer, parts, next, timer] {
                           if (*next < parts->size()) peer->write((*parts)[(*next)++]);
                           else rt::clearInterval(*timer);
                         },
                         60);
                     },
                     R"([["client","unexpected-response",401,0]])", "[]", idle, nullptr, true, 400});
    cases.push_back({"server closes before responding", [](const std::shared_ptr<RawPeer>& peer, const std::string&) { peer->destroy(); },
                     R"j([["client","error",{"error":"Error","message":"socket hang up","code":"ECONNRESET","errno":null,"syscall":null},2],["client","close",1006,{"buffer":""},3]])j"});
    cases.push_back({"server ends mid-response",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string&) {
                       peer->write("HTTP/1.1 101 Switch");
                       peer->end();
                     },
                     R"j([["client","error",{"error":"Error","message":"socket hang up","code":"ECONNRESET","errno":null,"syscall":null},2],["client","close",1006,{"buffer":""},3]])j"});
    cases.push_back({"close while connecting", [](const std::shared_ptr<RawPeer>&, const std::string&) {},
                     R"j([["client","after-close",2],)j" + clientError("WebSocket was closed before the connection was established") + "]", "[]", noDeflate(),
                     [](std::shared_ptr<ws::WebSocket> socket, Log log) {
                       socket->close();
                       push(log, {"client", "after-close", socket->readyState()});
                     }});
    cases.push_back({"terminate while connecting", [](const std::shared_ptr<RawPeer>&, const std::string&) {},
                     R"j([["client","after-terminate",2],)j" + clientError("WebSocket was closed before the connection was established") + "]", "[]", noDeflate(),
                     [](std::shared_ptr<ws::WebSocket> socket, Log log) {
                       socket->terminate();
                       push(log, {"client", "after-terminate", socket->readyState()});
                     }});
    cases.push_back({"send and ping while connecting", [](const std::shared_ptr<RawPeer>&, const std::string&) {},
                     R"j([["client","send-threw","Error","WebSocket is not open: readyState 0 (CONNECTING)"],["client","ping-threw","Error","WebSocket is not open: readyState 0 (CONNECTING)"],)j" +
                       clientError("WebSocket was closed before the connection was established") + "]",
                     "[]", noDeflate(),
                     [](std::shared_ptr<ws::WebSocket> socket, Log log) {
                       try {
                         socket->send(std::string("x"));
                       } catch (const rt::JsError& error) {
                         push(log, {"client", "send-threw", json::value(error.name), json::value(error.message)});
                       }
                       try {
                         socket->ping();
                       } catch (const rt::JsError& error) {
                         push(log, {"client", "ping-threw", json::value(error.name), json::value(error.message)});
                       }
                       socket->terminate();
                     }});
    cases.push_back({"Upgrade: foo", [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) { peer->write(replace(switching(accept), "Upgrade: websocket", "Upgrade: foo")); },
                     "[" + upgraded + "," + clientError("Invalid Upgrade header") + "]"});
    cases.push_back({"lower-case response headers",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       peer->write("HTTP/1.1 101 Switching Protocols\r\nupgrade: WebSocket\r\nconnection: upgrade\r\nsec-websocket-accept: " + accept + "\r\n\r\n");
                     },
                     "[" + upgraded + R"(,["client","open",1]])"});
    cases.push_back({"101 without Upgrade and no listener",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       peer->write("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
                     },
                     "[" + clientError("Unexpected server response: 101") + "]", "[]", noDeflate(), nullptr, false});
    cases.push_back({"401 and no listener",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string&) {
                       peer->write("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
                       peer->end();
                     },
                     "[" + clientError("Unexpected server response: 401") + "]", "[]", noDeflate(), nullptr, false});
    cases.push_back({"503 kept alive and no listener", [](const std::shared_ptr<RawPeer>& peer, const std::string&) { peer->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 4\r\n\r\nbusy"); },
                     "[" + clientError("Unexpected server response: 503") + "]", "[]", noDeflate(), nullptr, false});
    cases.push_back({"frames in the response chunk",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       ws::Bytes out = bytesOf(switching(accept));
                       const ws::Bytes early = frame(1, "early");
                       const ws::Bytes ping = frame(9, "p");
                       out.insert(out.end(), early.begin(), early.end());
                       out.insert(out.end(), ping.begin(), ping.end());
                       peer->write(out);
                     },
                     "[" + upgraded + R"(,["client","open",1],["client","message",{"buffer":"6561726c79"},false,1],["client","ping",{"buffer":"70"},1]])", R"([[138,true,"70"]])"});
    cases.push_back({"server FIN after open",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       peer->write(switching(accept));
                       rt::setTimeout([peer] { peer->end(); }, 20);
                     },
                     "[" + upgraded + R"(,["client","open",1],["client","close",1006,{"buffer":""},3]])"});
    cases.push_back({"server close frame without an echo wait",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       peer->write(switching(accept));
                       peer->write(frame(8, ws::Bytes{0x03, 0xe8}));
                     },
                     "[" + upgraded + R"(,["client","open",1],["client","close",1000,{"buffer":""},3]])", R"([[136,true,"03e8"]])"});
    cases.push_back({"fragmented binary with a pong between",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       peer->write(switching(accept));
                       ws::Bytes out = frame(2, "ab", FrameOptions{.fin = false});
                       for (const ws::Bytes& part : {frame(10, "pg"), frame(0, "cd", FrameOptions{.fin = false}), frame(0, "ef")}) out.insert(out.end(), part.begin(), part.end());
                       peer->write(out);
                     },
                     "[" + upgraded + R"(,["client","open",1],["client","pong",{"buffer":"7067"},1],["client","message",{"buffer":"616263646566"},true,1]])"});
    cases.push_back({"empty close frame then FIN",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       peer->write(switching(accept));
                       peer->write(frame(8, ws::Bytes{}));
                       rt::setTimeout([peer] { peer->end(); }, 30);
                     },
                     "[" + upgraded + R"(,["client","open",1],["client","close",1005,{"buffer":""},3]])", R"([[136,true,""]])"});
    cases.push_back({"client close the server never answers", [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) { peer->write(switching(accept)); },
                     "[" + upgraded + R"(,["client","open",1]])", R"([[136,true,"0fa0627965"]])", noDeflate(),
                     [](std::shared_ptr<ws::WebSocket> socket, Log) { socket->once("open", [socket] { socket->close(4000, std::string("bye")); }); }});
    cases.push_back({"garbage response", [](const std::shared_ptr<RawPeer>& peer, const std::string&) { peer->write("garbage\r\n\r\n"); },
                     R"j([["client","error",{"error":"Error","message":"Parse Error: Expected HTTP/, RTSP/ or ICE/","code":"HPE_INVALID_CONSTANT","errno":null,"syscall":null},2],["client","close",1006,{"buffer":""},3]])j"});
    cases.push_back({"invalid status code", [](const std::shared_ptr<RawPeer>& peer, const std::string&) { peer->write("HTTP/1.1 abc OK\r\n\r\n"); },
                     R"j([["client","error",{"error":"Error","message":"Parse Error: Invalid status code","code":"HPE_INVALID_STATUS","errno":null,"syscall":null},2],["client","close",1006,{"buffer":""},3]])j"});
    cases.push_back({"HTTP/1.0 101", [](const std::shared_ptr<RawPeer>& peer, const std::string&) { peer->write("HTTP/1.0 101 Switching\r\nUpgrade: websocket\r\n\r\n"); },
                     R"([["client","unexpected-response",101,0]])"});
    cases.push_back({"messages while closing",
                     [](const std::shared_ptr<RawPeer>& peer, const std::string& accept) {
                       peer->write(switching(accept));
                       std::weak_ptr<RawPeer> weak = peer;
                       peer->onReceive = [weak] {
                         auto target = weak.lock();
                         if (!target || target->received.empty() || target->ended) return;
                         target->onReceive = nullptr;
                         ws::Bytes out = frame(1, "late");
                         for (const ws::Bytes& part : {frame(8, ws::Bytes{0x03, 0xe8}), frame(1, "after-close")}) out.insert(out.end(), part.begin(), part.end());
                         target->write(out);
                         rt::setTimeout([target] { target->end(); }, 20);
                       };
                     },
                     "[" + upgraded + R"(,["client","open",1],["client","message",{"buffer":"6c617465"},false,2],["client","send-in-closing-ok",10],["client","close",1000,{"buffer":""},3]])",
                     R"([[136,true,"03e878"]])", noDeflate(),
                     [](std::shared_ptr<ws::WebSocket> socket, Log log) {
                       socket->once("open", [socket, log] {
                         socket->close(1000, std::string("x"));
                         socket->on("message", [socket, log] {
                           socket->send(std::string("in-closing"));
                           push(log, {"client", "send-in-closing-ok", static_cast<std::int64_t>(socket->bufferedAmount())});
                         });
                       });
                     }});
    return cases;
  }
}

ASYNC_TEST("ws client: handshakes and frames from a raw server match ws") {
  for (auto& wireCase : clientWireCases()) co_await runClientWireCase(wireCase);
}

// ---- the opening handshake request ------------------------------------------------------------

namespace {
  // The request head a client sends, with the random key blanked.
  rt::Task<std::string> requestHead(std::string url, ws::ClientOptions options) {
    auto server = std::make_shared<RawServer>([](const std::shared_ptr<RawPeer>& peer, const std::string&) { peer->destroy(); });
    server->start();
    const std::string address = replace(url, "PORT", std::to_string(server->port()));
    auto client = ws::WebSocket::create(address, std::move(options));
    client->on("error", [] {});
    WAIT_UNTIL(!server->request.empty());
    std::string head = server->request;
    const std::size_t key = head.find("Sec-WebSocket-Key: ");
    if (key != std::string::npos) head.replace(key + 19, 24, "<key>");
    head = replace(head, ":" + std::to_string(server->port()) + "\r\n", ":PORT\r\n");
    client->terminate();
    server->close();
    co_return head;
  }
}

ASYNC_TEST("client request heads match ws on Bun") {
  ws::ClientOptions options = noDeflate();
  options.headers = {{"Host", "localhost"}, {"authorization", "Bearer x"}};
  options.maxPayload = 1024;
  options.handshakeTimeout = 3000;
  std::string head = co_await requestHead("ws://127.0.0.1:PORT/p", options);
  EXPECT_EQ(head, "GET /p HTTP/1.1\r\nHost: localhost\r\nauthorization: Bearer x\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: <key>\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
  head = co_await requestHead("http://127.0.0.1:PORT", noDeflate());
  EXPECT_EQ(head, "GET / HTTP/1.1\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: <key>\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nHost: 127.0.0.1:PORT\r\n\r\n");
  ws::ClientOptions custom = noDeflate();
  custom.headers = {{"x-custom", "a"}, {"Sec-WebSocket-Version", "8"}};
  head = co_await requestHead("ws://127.0.0.1:PORT/", custom);
  EXPECT_EQ(head, "GET / HTTP/1.1\r\nx-custom: a\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: <key>\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nHost: 127.0.0.1:PORT\r\n\r\n");
  head = co_await requestHead("ws://user:pa%20ss@127.0.0.1:PORT/", noDeflate());
  EXPECT_EQ(head, "GET / HTTP/1.1\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: <key>\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nHost: 127.0.0.1:PORT\r\nAuthorization: Basic dXNlcjpwYSUyMHNz\r\n\r\n");
  head = co_await requestHead("ws://127.0.0.1:PORT/path?x=1", ws::ClientOptions{});
  // ws offers permessage-deflate by default; rt never does (docs/rt/websocket.md, limits).
  EXPECT_EQ(head, "GET /path?x=1 HTTP/1.1\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: <key>\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nHost: 127.0.0.1:PORT\r\n\r\n");
  ws::ClientOptions latin = noDeflate();
  latin.headers = {{"x-a", "caf\xc3\xa9"}};
  const std::string path = tempSocketPath();
  ws::UpgradeRequest seen;
  auto capture = std::make_shared<TestHttpServer>([&seen](const ws::UpgradeRequest& request, ws::UpgradeSocket& socket, ws::Bytes) {
    seen = request;
    boost::system::error_code ignored;
    socket.stream.close(ignored);
  });
  capture->listenUnix(path);
  auto client = ws::WebSocket::create("ws+unix://" + path + ":/request/path?q=1", latin);
  client->on("error", [] {});
  WAIT_UNTIL(!seen.method.empty());
  EXPECT_EQ(seen.method, "GET");
  EXPECT_EQ(seen.url, "/request/path?q=1");
  // No Host header for a Unix socket unless one is given; Latin-1 header values go out as single bytes.
  EXPECT(!seen.header("host"));
  EXPECT_EQ(seen.header("x-a"), std::optional<std::string>("caf\xc3\xa9"));
  EXPECT_EQ(client->url(), std::optional<std::string>("ws+unix://" + path + ":/request/path?q=1"));
  client->terminate();
  capture->close();
}

TEST("client constructor errors match ws on Bun") {
  EXPECT_THROWS_MESSAGE(ws::WebSocket::create("not a url"), rt::SyntaxError, "Invalid URL: not a url");
  EXPECT_THROWS_MESSAGE(ws::WebSocket::create("ftp://127.0.0.1/"), rt::SyntaxError, "The URL's protocol must be one of \"ws:\", \"wss:\", \"http:\", \"https:\", or \"ws+unix:\"");
  EXPECT_THROWS_MESSAGE(ws::WebSocket::create("ws://127.0.0.1:1/#x"), rt::SyntaxError, "The URL contains a fragment identifier");
  EXPECT_THROWS_MESSAGE(ws::WebSocket::create("ws+unix:"), rt::SyntaxError, "The URL's pathname is empty");
  for (const auto& [name, value, message, code] : std::vector<std::tuple<std::string, std::string, std::string, std::string>>{
         {"bad name", "x", "Header name must be a valid HTTP token [\"bad name\"]", "ERR_INVALID_HTTP_TOKEN"},
         {"", "x", "Header name must be a valid HTTP token [\"\"]", "ERR_INVALID_HTTP_TOKEN"},
         {"x-a", "line\nbreak", "Invalid character in header content [\"x-a\"]", "ERR_INVALID_CHAR"},
         {"x-a", "\xe2\x82\xac", "Invalid character in header content [\"x-a\"]", "ERR_INVALID_CHAR"}}) {
    ws::ClientOptions options = noDeflate();
    options.headers = {{name, value}};
    try {
      ws::WebSocket::create("ws://127.0.0.1:9/", options);
      EXPECT(false);
    } catch (const ws::CodedError& error) {
      EXPECT_EQ(error.name, "TypeError");
      EXPECT_EQ(error.message, message);
      EXPECT_EQ(error.code, code);
    }
  }
}

ASYNC_TEST("connection failures carry Node's system errors") {
  auto log = newLog();
  int port = 0;
  {
    asio::ip::tcp::acceptor probe(rt::executor(), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    port = probe.local_endpoint().port();
  }
  auto refused = ws::WebSocket::create("ws://127.0.0.1:" + std::to_string(port) + "/", noDeflate());
  record(log, "refused", refused);
  const std::string missing = tempSocketPath() + ".missing";
  auto absent = ws::WebSocket::create("ws+unix://" + missing + ":/x", noDeflate());
  record(log, "absent", absent);
  WAIT_UNTIL(refused->readyState() == ws::WebSocket::CLOSED && absent->readyState() == ws::WebSocket::CLOSED);
#if defined(__APPLE__)
  EXPECT_EQ(side(log, "refused"), R"j([["refused","error",{"error":"Error","message":"connect ECONNREFUSED 127.0.0.1:)j" + std::to_string(port) +
              R"j(","code":"ECONNREFUSED","errno":-61,"syscall":"connect"},2],["refused","close",1006,{"buffer":""},3]])j");
#else
  EXPECT_EQ(side(log, "refused"), R"j([["refused","error",{"error":"Error","message":"connect ECONNREFUSED 127.0.0.1:)j" + std::to_string(port) +
              R"j(","code":"ECONNREFUSED","errno":-111,"syscall":"connect"},2],["refused","close",1006,{"buffer":""},3]])j");
#endif
  EXPECT_EQ(side(log, "absent"), R"j([["absent","error",{"error":"Error","message":"connect ENOENT )j" + missing + R"j(","code":"ENOENT","errno":-2,"syscall":"connect"},2],["absent","close",1006,{"buffer":""},3]])j");
}

// ---- close() argument checks ------------------------------------------------------------------

ASYNC_TEST("close() validates like ws, leaving readyState CLOSING when it throws") {
  struct Attempt {
    std::optional<int> code;
    std::optional<ws::Data> reason;
    std::string outcome;
    std::string frame;
  };
  const std::string invalidCode = R"(["close-threw","TypeError","First argument must be a valid error code number",2],["second-close-ok",2])";
  const std::string tooLong = R"(["close-threw","RangeError","The message must not be greater than 123 bytes",2],["second-close-ok",2])";
  std::vector<Attempt> attempts{
    {1004, std::nullopt, invalidCode, ""},
    {999, std::nullopt, invalidCode, ""},
    {5000, std::nullopt, invalidCode, ""},
    {1006, std::nullopt, invalidCode, ""},
    {1005, std::nullopt, invalidCode, ""},
    {1015, std::nullopt, invalidCode, ""},
    {2999, std::nullopt, invalidCode, ""},
    {3000, std::nullopt, R"(["close-ok",2])", "0bb8"},
    {4999, std::nullopt, R"(["close-ok",2])", "1387"},
    {1014, std::nullopt, R"(["close-ok",2])", "03f6"},
    {1000, ws::Data(std::string(124, 'x')), tooLong, ""},
    {1000, ws::Data(std::string(123, 'x')), R"(["close-ok",2])", "03e8" + repeat("78", 123)},
    {3000, ws::Data(repeat("\xc3\xa9", 62)),
     tooLong, ""},
    {std::nullopt, ws::Data(std::string("reason")), R"(["close-ok",2])", ""},
    {1000, ws::Data(std::string("")), R"(["close-ok",2])", "03e8"},
    {1000, ws::Data(ws::Bytes{'b', 'u', 'f'}), R"(["close-ok",2])", "03e8627566"},
  };
  for (auto& attempt : attempts) {
    auto log = newLog();
    auto server = std::make_shared<RawServer>([](const std::shared_ptr<RawPeer>& peer, const std::string& accept) { peer->write(switching(accept)); });
    server->start();
    auto client = ws::WebSocket::create("ws://127.0.0.1:" + std::to_string(server->port()) + "/", noDeflate());
    client->on("error", [] {});
    WAIT_UNTIL(client->readyState() == ws::WebSocket::OPEN);
    try {
      client->close(attempt.code, attempt.reason);
      push(log, {"close-ok", client->readyState()});
    } catch (const rt::JsError& error) {
      push(log, {"close-threw", json::value(error.name), json::value(error.message), client->readyState()});
      client->close();
      push(log, {"second-close-ok", client->readyState()});
    }
    co_await rt::sleep(40);
    const auto frames = parseFrames(server->peer->received);
    EXPECT_EQ(json::serialize(*log), "[" + attempt.outcome + "]");
    if (attempt.outcome.find("threw") != std::string::npos) {
      EXPECT_EQ(frames.size(), std::size_t(0));
    } else {
      ASSERT_EQ(frames.size(), std::size_t(1));
      EXPECT_EQ(frames[0].first, 0x88);
      EXPECT(frames[0].masked);
      EXPECT_EQ(hex(frames[0].payload), attempt.frame);
    }
    client->terminate();
    server->close();
  }
  auto server = std::make_shared<RawServer>([](const std::shared_ptr<RawPeer>& peer, const std::string& accept) { peer->write(switching(accept)); });
  server->start();
  auto client = ws::WebSocket::create("ws://127.0.0.1:" + std::to_string(server->port()) + "/", noDeflate());
  WAIT_UNTIL(client->readyState() == ws::WebSocket::OPEN);
  EXPECT_THROWS_MESSAGE(client->ping(ws::Bytes(126)), rt::RangeError, "The data size must not be greater than 125 bytes");
  EXPECT_THROWS_MESSAGE(client->pong(std::string(126, 'p')), rt::RangeError, "The data size must not be greater than 125 bytes");
  client->terminate();
  server->close();
}

// ---- server features the gateway relies on ------------------------------------------------------

namespace {
  // tests/runtime-http.test.ts's fixture through src/runtime/http-upgrade.ts:
  // a socket class whose close() rewrites ws's bare 1009 close.
  class PayloadLimitedSocket : public ws::WebSocket {
  public:
    PayloadLimitedSocket(bool autoPong, std::optional<std::pair<int, std::string>> payloadLimitClose) : ws::WebSocket(nullptr, autoPong), payloadLimitClose_(std::move(payloadLimitClose)) {}
    using ws::WebSocket::close;
    void close(std::optional<int> code, std::optional<ws::Data> reason) override {
      // ws invokes a bare 1009 close for its receiver's maxPayload failure.
      if (code == 1009 && !reason && payloadLimitClose_) ws::WebSocket::close(payloadLimitClose_->first, ws::Data(payloadLimitClose_->second));
      else ws::WebSocket::close(code, std::move(reason));
    }

  private:
    std::optional<std::pair<int, std::string>> payloadLimitClose_;
  };
}

ASYNC_TEST("a custom socket class rewrites the payload-limit close (runtime-http payload fixture)") {
  auto log = newLog();
  auto messages = std::make_shared<int>(0);
  ws::ServerOptions options = serverOptions(1024);
  options.WebSocket = [](const ws::ServerOptions& serverOptions) {
    return std::make_shared<PayloadLimitedSocket>(serverOptions.autoPong, std::make_pair(1011, std::string("fixture payload limit")));
  };
  std::function<void(std::shared_ptr<ws::WebSocket>)> onAccept = [log, messages](std::shared_ptr<ws::WebSocket> socket) {
    record(log, "server", socket);
    socket->on("message", [messages] { ++*messages; });
    socket->on("error", [] {});
  };
  auto server = startServer(options, onAccept);
  server->listenTcp();
  auto client = ws::WebSocket::create("ws://127.0.0.1:" + std::to_string(server->port), noDeflate());
  record(log, "client", client);
  client->once("open", [client] { client->send(ws::Bytes(1025)); });
  WAIT_UNTIL(client->readyState() == ws::WebSocket::CLOSED);
  EXPECT_EQ(side(log, "client"), "[" + OPENED + R"(,["client","close",1011,{"buffer":")" + hex(bytesOf("fixture payload limit")) + R"("},3]])");
  WAIT_UNTIL(side(log, "server").find("close") != std::string::npos);
  EXPECT_EQ(side(log, "server"),
            R"j([["server","error",{"error":"RangeError","message":"Max payload size exceeded","code":"WS_ERR_UNSUPPORTED_MESSAGE_LENGTH","errno":null,"syscall":null},2],["server","close",1006,{"buffer":""},3]])j");
  EXPECT_EQ(*messages, 0);
  server->close();
}

ASYNC_TEST("headers listeners extend the 101 response; frames sent with the request reach the accepted socket") {
  auto received = std::make_shared<std::vector<std::string>>();
  std::function<void(std::shared_ptr<ws::WebSocket>)> onAccept = [received](std::shared_ptr<ws::WebSocket> socket) {
    socket->on("message", [received](const ws::EventArgs& args) {
      const auto& bytes = std::get<ws::Bytes>(args.data);
      received->emplace_back(bytes.begin(), bytes.end());
    });
  };
  std::function<void(ws::WebSocketServer&)> configure = [](ws::WebSocketServer& server) {
    server.once("headers", [](const ws::EventArgs& args) {
      EXPECT_EQ(args.request->url, "/x");
      args.headers->push_back("x-extra: caf\xc3\xa9");
      args.headers->push_back("set-cookie: a=1");
    });
  };
  auto server = startServer(serverOptions(1024), onAccept, configure);
  server->listenTcp();
  std::shared_ptr<RawPeer> peer = co_await connectRaw(server->port);
  ws::Bytes first = bytesOf(upgradeRequest());
  for (const ws::Bytes& part : {frame(1, "one", FrameOptions{.mask = MASK}), frame(1, "two", FrameOptions{.mask = MASK})}) first.insert(first.end(), part.begin(), part.end());
  peer->write(first);
  WAIT_UNTIL(received->size() == 2);
  // A header line is written as UTF-8, like ws's socket.write(string).
  EXPECT_EQ(peer->head(), replace(SWITCHED, "\r\n\r\n", "\r\nx-extra: caf\xc3\xa9\r\nset-cookie: a=1\r\n\r\n"));
  EXPECT_EQ(*received, (std::vector<std::string>{"one", "two"}));
  peer->destroy();
  server->close();
}

ASYNC_TEST("WebSocketServer: close, 503 after close, half-closed sockets, a throwing headers listener, client tracking") {
  EXPECT_THROWS_MESSAGE(ws::WebSocketServer(ws::ServerOptions{}), rt::TypeError, "One and only one of the \"port\", \"server\", or \"noServer\" options must be specified");
  ws::ServerOptions options;
  options.noServer = true;
  options.path = "/only";
  auto upgradeServer = std::make_shared<ws::WebSocketServer>(options);
  ws::UpgradeRequest wrongPath{"GET", "/other?x=1", {}};
  ws::UpgradeRequest rightPath{"GET", "/only?x=1", {}};
  EXPECT(!upgradeServer->shouldHandle(wrongPath));
  EXPECT(upgradeServer->shouldHandle(rightPath));
  EXPECT_EQ(upgradeServer->clients().size(), std::size_t(0));

  auto calls = std::make_shared<std::vector<std::string>>();
  upgradeServer->close([calls](rt::Reason error) { calls->push_back(error ? rt::errorMessage(error).value_or("?") : "closed"); });
  EXPECT(calls->empty());
  co_await rt::microtask();
  co_await rt::microtask();
  EXPECT_EQ(*calls, (std::vector<std::string>{"closed"}));
  upgradeServer->close([calls](rt::Reason error) { calls->push_back(error ? rt::errorMessage(error).value_or("?") : "closed"); });
  co_await rt::microtask();
  co_await rt::microtask();
  EXPECT_EQ(*calls, (std::vector<std::string>{"closed", "The server is not running"}));

  // An upgrade after close() is refused with 503.
  auto http = std::make_shared<TestHttpServer>([upgradeServer](const ws::UpgradeRequest& request, ws::UpgradeSocket& socket, ws::Bytes head) {
    ws::UpgradeRequest routed = request;
    routed.url = "/only";
    upgradeServer->handleUpgrade(routed, socket, std::move(head), [](std::shared_ptr<ws::WebSocket>, const ws::UpgradeRequest&) { EXPECT(false); });
  });
  http->listenTcp();
  std::shared_ptr<RawPeer> peer = co_await connectRaw(http->port);
  peer->write(upgradeRequest());
  WAIT_UNTIL(peer->ended);
  EXPECT_EQ(std::string(peer->received.begin(), peer->received.end()), errorResponse(503, "Service Unavailable", "Service Unavailable") + "Service Unavailable");
  peer->destroy();
  http->close();

  // A socket the client already half-closed is destroyed without a callback;
  // a throwing headers listener leaves the stream with the caller.
  ws::ServerOptions open;
  open.noServer = true;
  auto fresh = std::make_shared<ws::WebSocketServer>(open);
  auto outcomes = std::make_shared<std::vector<std::string>>();
  auto second = std::make_shared<TestHttpServer>([fresh, outcomes](const ws::UpgradeRequest& request, ws::UpgradeSocket& socket, ws::Bytes head) {
    if (request.url == "/half") {
      socket.readable = false;
      fresh->handleUpgrade(request, socket, std::move(head), [outcomes](std::shared_ptr<ws::WebSocket>, const ws::UpgradeRequest&) { outcomes->push_back("half-callback"); });
      outcomes->push_back(socket.stream.is_open() ? "half-open" : "half-taken");
      return;
    }
    ws::Listener thrower([] { throw rt::TypeError("headers listener failed"); });
    fresh->once("headers", thrower);
    try {
      fresh->handleUpgrade(request, socket, std::move(head), [outcomes](std::shared_ptr<ws::WebSocket>, const ws::UpgradeRequest&) { outcomes->push_back("throw-callback"); });
    } catch (const rt::TypeError& error) {
      outcomes->push_back(error.message);
    }
    outcomes->push_back(socket.stream.is_open() ? "stream-kept" : "stream-taken");
    boost::system::error_code ignored;
    socket.stream.close(ignored);
  });
  second->listenTcp();
  std::shared_ptr<RawPeer> half = co_await connectRaw(second->port);
  half->write(upgradeRequest("/half"));
  WAIT_UNTIL(half->ended);
  std::shared_ptr<RawPeer> throwing = co_await connectRaw(second->port);
  throwing->write(upgradeRequest("/throw"));
  WAIT_UNTIL(throwing->ended);
  EXPECT_EQ(*outcomes, (std::vector<std::string>{"half-taken", "headers listener failed", "stream-kept"}));
  EXPECT(half->received.empty());
  EXPECT(throwing->received.empty());
  second->close();

  // clientTracking: clients() lists the open socket; "close" waits for it.
  ws::ServerOptions tracking;
  tracking.noServer = true;
  auto tracker = std::make_shared<ws::WebSocketServer>(tracking);
  std::shared_ptr<ws::WebSocket> accepted;
  auto third = std::make_shared<TestHttpServer>([tracker, &accepted](const ws::UpgradeRequest& request, ws::UpgradeSocket& socket, ws::Bytes head) {
    tracker->handleUpgrade(request, socket, std::move(head), [&accepted](std::shared_ptr<ws::WebSocket> websocket, const ws::UpgradeRequest&) { accepted = websocket; });
  });
  third->listenTcp();
  auto client = ws::WebSocket::create("ws://127.0.0.1:" + std::to_string(third->port) + "/", noDeflate());
  WAIT_UNTIL(client->readyState() == ws::WebSocket::OPEN && accepted);
  EXPECT_EQ(tracker->clients().size(), std::size_t(1));
  EXPECT(tracker->clients()[0] == accepted);
  auto trackerClosed = std::make_shared<bool>(false);
  tracker->close([trackerClosed](rt::Reason) { *trackerClosed = true; });
  co_await rt::sleep(20);
  EXPECT(!*trackerClosed);
  client->close(1000);
  WAIT_UNTIL(*trackerClosed);
  EXPECT(*trackerClosed);
  EXPECT_EQ(tracker->clients().size(), std::size_t(0));
  third->close();
}

ASYNC_TEST("onopen/onmessage/onclose/onerror attributes keep ws's listener order and event shapes") {
  auto log = newLog();
  std::function<void(std::shared_ptr<ws::WebSocket>)> onAccept = [log](std::shared_ptr<ws::WebSocket> socket) {
    socket->on("message", [log] { push(log, {"server", "on-message-1"}); });
    socket->onmessage([log](const ws::MessageEvent& event) {
      const bool text = std::holds_alternative<std::string>(event.data);
      push(log, {"server", "onmessage", text ? "string" : "object", describe(event.data), json::value(event.type)});
    });
    socket->on("message", [log] { push(log, {"server", "on-message-2"}); });
    socket->onclose([log](const ws::CloseEvent& event) { push(log, {"server", "onclose", event.code, json::value(event.reason), event.wasClean, json::value(event.type)}); });
    socket->onerror([log](const ws::ErrorEvent& event) { push(log, {"server", "onerror", json::value(event.message), json::value(event.type)}); });
  };
  auto server = startServer(serverOptions(1024), onAccept);
  server->listenTcp();
  auto client = ws::WebSocket::create("ws://127.0.0.1:" + std::to_string(server->port) + "/", noDeflate());
  ws::WebSocket* raw = client.get();
  client->onopen([log, raw](const ws::Event& event) {
    push(log, {"client", "onopen", json::value(event.type)});
    raw->send(std::string("txt"));
    raw->send(ws::Bytes{'b', 'i', 'n'});
    raw->close(4001, std::string("reason"));
  });
  client->onclose([log](const ws::CloseEvent& event) { push(log, {"client", "onclose", event.code, json::value(event.reason), event.wasClean}); });
  EXPECT(static_cast<bool>(client->onopen()));
  EXPECT(!client->onmessage());
  WAIT_UNTIL(side(log, "client").find("onclose") != std::string::npos && side(log, "server").find("onclose") != std::string::npos);
  EXPECT_EQ(side(log, "client"), R"([["client","onopen","open"],["client","onclose",4001,"reason",true]])");
  EXPECT_EQ(side(log, "server"),
            R"([["server","on-message-1"],["server","onmessage","string",{"string":"txt"},"message"],["server","on-message-2"],["server","on-message-1"],["server","onmessage","object",{"buffer":"62696e"},"message"],["server","on-message-2"],["server","onclose",4001,"reason",true,"close"]])");
  // Replacing the attribute moves it to the end; null removes it.
  auto other = std::make_shared<ws::WebSocket>(nullptr);
  std::vector<std::string> order;
  other->onmessage([&order](const ws::MessageEvent&) { order.push_back("attribute-1"); });
  other->on("message", [&order] { order.push_back("listener"); });
  other->onmessage([&order](const ws::MessageEvent&) { order.push_back("attribute-2"); });
  ws::EventArgs args;
  args.data = ws::Bytes{'a'};
  other->emit("message", args);
  other->onmessage(nullptr);
  other->emit("message", args);
  EXPECT_EQ(order, (std::vector<std::string>{"listener", "attribute-2", "listener"}));
  server->close();
}

ASYNC_TEST("bufferedAmount: 0 once the kernel took the data; callbacks run after the microtasks; the prototype getter can be replaced") {
  auto log = newLog();
  auto server = startServer(serverOptions(104857600), [](std::shared_ptr<ws::WebSocket> socket) { socket->on("message", [] {}); });
  server->listenTcp();
  const std::string url = "ws://127.0.0.1:" + std::to_string(server->port) + "/";
  auto client = ws::WebSocket::create(url, noDeflate());
  WAIT_UNTIL(client->readyState() == ws::WebSocket::OPEN);
  ws::WebSocket* raw = client.get();
  client->send(std::string("abc"), [log, raw](rt::Reason error) { push(log, {"send-callback", error ? describeError(error) : json::value(nullptr), static_cast<std::int64_t>(raw->bufferedAmount())}); });
  push(log, {"bufferedAmount-sync", static_cast<std::int64_t>(client->bufferedAmount())});
  client->send(ws::Bytes(100000), [log, raw](rt::Reason error) { push(log, {"send-callback-big", error ? describeError(error) : json::value(nullptr), static_cast<std::int64_t>(raw->bufferedAmount())}); });
  push(log, {"bufferedAmount-sync-big", static_cast<std::int64_t>(client->bufferedAmount())});
  rt::queueMicrotask([log, raw] { push(log, {"bufferedAmount-microtask", static_cast<std::int64_t>(raw->bufferedAmount())}); });
  WAIT_UNTIL(log->size() == 5);
  EXPECT_EQ(json::serialize(*log),
            R"([["bufferedAmount-sync",0],["bufferedAmount-sync-big",0],["bufferedAmount-microtask",0],["send-callback",null,0],["send-callback-big",null,0]])");

  auto original = ws::WebSocket::bufferedAmountGetter();
  ws::WebSocket::bufferedAmountGetter() = [original, url](const ws::WebSocket& socket) { return socket.url() == url ? 8388609.0 : original(socket); };
  auto unrelated = std::make_shared<ws::WebSocket>(nullptr);
  EXPECT_EQ(client->bufferedAmount(), 8388609.0);
  EXPECT_EQ(unrelated->bufferedAmount(), 0.0);
  ws::WebSocket::bufferedAmountGetter() = original;
  EXPECT_EQ(client->bufferedAmount(), 0.0);
  client->terminate();
  server->close();
}

ASYNC_TEST("large messages use 16- and 64-bit lengths in both directions over a Unix socket") {
  std::function<void(std::shared_ptr<ws::WebSocket>)> onAccept = [](std::shared_ptr<ws::WebSocket> socket) {
    ws::WebSocket* raw = socket.get();
    socket->on("message", [raw](const ws::EventArgs& args) { raw->send(args.data, ws::SendOptions{.binary = args.isBinary}); });
  };
  auto server = startServer(serverOptions(104857600), onAccept);
  server->listenUnix(tempSocketPath());
  auto client = ws::WebSocket::create("ws+unix://" + server->path + ":/", noDeflate());
  auto echoes = std::make_shared<std::vector<ws::Bytes>>();
  client->on("message", [echoes](const ws::EventArgs& args) { echoes->push_back(std::get<ws::Bytes>(args.data)); });
  WAIT_UNTIL(client->readyState() == ws::WebSocket::OPEN);
  ws::Bytes big(300000);
  for (std::size_t index = 0; index < big.size(); ++index) big[index] = static_cast<std::uint8_t>(index * 7);
  const std::string medium(70000 - 1, 'm');
  client->send(big);
  client->send(medium + "\xc3\xa9");
  client->send(std::string(126, 'z'));
  WAIT_UNTIL_FOR(echoes->size() == 3, 5000);
  ASSERT_EQ(echoes->size(), std::size_t(3));
  EXPECT((*echoes)[0] == big);
  EXPECT_EQ(std::string((*echoes)[1].begin(), (*echoes)[1].end()), medium + "\xc3\xa9");
  EXPECT_EQ(std::string((*echoes)[2].begin(), (*echoes)[2].end()), std::string(126, 'z'));
  client->close(1000);
  WAIT_UNTIL(client->readyState() == ws::WebSocket::CLOSED);
  server->close();
}

// ---- wss: (TLS client) ---------------------------------------------------------------------

namespace {
  struct TestCertificate {
    std::string certificate;
    std::string key;
  };

  std::string pemOf(const std::function<int(BIO*)>& write) {
    BIO* bio = BIO_new(BIO_s_mem());
    write(bio);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string out(data, static_cast<std::size_t>(size));
    BIO_free(bio);
    return out;
  }

  // A self-signed P-256 certificate, like `openssl req -x509 -newkey ec ... -subj /CN=<cn> -addext subjectAltName=<san>`.
  TestCertificate makeCertificate(const std::string& commonName, const std::string& subjectAltName) {
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    X509* certificate = X509_new();
    X509_set_version(certificate, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1);
    X509_gmtime_adj(X509_getm_notBefore(certificate), -3600);
    X509_gmtime_adj(X509_getm_notAfter(certificate), 2 * 86400);
    X509_set_pubkey(certificate, key);
    X509_NAME* name = X509_get_subject_name(certificate);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(commonName.c_str()), -1, -1, 0);
    X509_set_issuer_name(certificate, name);
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, NID_subject_alt_name, subjectAltName.c_str());
    X509_add_ext(certificate, extension, -1);
    X509_EXTENSION_free(extension);
    X509_sign(certificate, key, EVP_sha256());
    TestCertificate out{pemOf([&](BIO* bio) { return PEM_write_bio_X509(bio, certificate); }),
                        pemOf([&](BIO* bio) { return PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr); })};
    X509_free(certificate);
    EVP_PKEY_free(key);
    return out;
  }

  // A wss: endpoint for the client tests: TLS, the ws handshake, then text
  // frames echoed and a close frame answered, followed by close_notify and FIN.
  class TlsEchoServer : public std::enable_shared_from_this<TlsEchoServer> {
  public:
    explicit TlsEchoServer(const TestCertificate& certificate)
      : context_(asio::ssl::context::tls_server), acceptor_(rt::executor(), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
      context_.use_certificate_chain(asio::buffer(certificate.certificate));
      context_.use_private_key(asio::buffer(certificate.key), asio::ssl::context::pem);
    }
    int port() const { return acceptor_.local_endpoint().port(); }
    void start() {
      acceptor_.async_accept([self = shared_from_this()](const boost::system::error_code& error, asio::ip::tcp::socket socket) {
        if (error) return;
        auto connection = std::make_shared<Connection>(std::move(socket), self->context_);
        self->connections_.push_back(connection);
        connection->stream.async_handshake(asio::ssl::stream_base::server, [connection](const boost::system::error_code& failure) {
          if (!failure) connection->read();
        });
        self->start();
      });
    }
    void close() {
      boost::system::error_code ignored;
      acceptor_.close(ignored);
      for (auto& connection : connections_) connection->stream.lowest_layer().close(ignored);
    }

  private:
    struct Connection : std::enable_shared_from_this<Connection> {
      Connection(asio::ip::tcp::socket socket, asio::ssl::context& context) : stream(std::move(socket), context) {}
      asio::ssl::stream<asio::ip::tcp::socket> stream;
      std::array<std::uint8_t, 16384> buffer{};
      ws::Bytes received;
      bool upgraded = false;
      std::deque<ws::Bytes> outgoing;
      bool writing = false;
      bool closing = false;
      void read() {
        stream.async_read_some(asio::buffer(buffer), [self = shared_from_this()](const boost::system::error_code& error, std::size_t count) {
          if (error) return;
          self->received.insert(self->received.end(), self->buffer.begin(), self->buffer.begin() + static_cast<std::ptrdiff_t>(count));
          self->process();
          self->read();
        });
      }
      void process() {
        if (!upgraded) {
          const std::string text(received.begin(), received.end());
          const std::size_t end = text.find("\r\n\r\n");
          if (end == std::string::npos) return;
          const std::size_t keyAt = text.find("Sec-WebSocket-Key: ");
          const std::string key = text.substr(keyAt + 19, text.find("\r\n", keyAt) - keyAt - 19);
          send(bytesOf(switching(rt::crypto::createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64"))));
          received.erase(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(end + 4));
          upgraded = true;
        }
        for (;;) {
          if (received.size() < 2) return;
          std::size_t length = received[1] & 0x7f;
          std::size_t header = 2;
          if (length == 126) {
            if (received.size() < 4) return;
            length = (static_cast<std::size_t>(received[2]) << 8) | received[3];
            header = 4;
          } else if (length == 127) {
            if (received.size() < 10) return;
            length = 0;
            for (std::size_t index = 2; index < 10; ++index) length = (length << 8) | received[index];
            header = 10;
          }
          if (received.size() < header + 4 + length) return;
          ws::Bytes one(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(header + 4 + length));
          received.erase(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(header + 4 + length));
          const ParsedFrame parsed = parseFrames(one).front();
          if ((parsed.first & 0x0f) == 1) send(frame(1, parsed.payload));
          if ((parsed.first & 0x0f) == 8) {
            send(frame(8, parsed.payload));
            closing = true;
          }
        }
      }
      void send(ws::Bytes data) {
        outgoing.push_back(std::move(data));
        if (!writing) writeNext();
      }
      void writeNext() {
        if (outgoing.empty()) {
          writing = false;
          if (closing) {
            stream.async_shutdown([self = shared_from_this()](const boost::system::error_code&) {
              boost::system::error_code ignored;
              self->stream.lowest_layer().shutdown(asio::socket_base::shutdown_send, ignored);
            });
          }
          return;
        }
        writing = true;
        asio::async_write(stream, asio::buffer(outgoing.front()), [self = shared_from_this()](const boost::system::error_code& error, std::size_t) {
          self->outgoing.pop_front();
          if (error) return;
          self->writeNext();
        });
      }
    };
    asio::ssl::context context_;
    asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<Connection>> connections_;
  };

  rt::Task<std::string> tlsClientLog(std::shared_ptr<TlsEchoServer> server, std::string host, ws::ClientOptions options, bool closeAfterEcho) {
    auto log = newLog();
    auto client = ws::WebSocket::create("wss://" + host + ":" + std::to_string(server->port()) + "/", std::move(options));
    record(log, "client", client);
    client->once("open", [client] { client->send(std::string("tls-echo")); });
    if (closeAfterEcho) client->once("message", [client] { client->close(1000, std::string("done")); });
    WAIT_UNTIL_FOR(client->readyState() == ws::WebSocket::CLOSED || side(log, "client").find("message") != std::string::npos, 3000);
    if (closeAfterEcho) WAIT_UNTIL_FOR(client->readyState() == ws::WebSocket::CLOSED, 3000);
    co_await rt::sleep(20);
    client->terminate();
    co_return side(log, "client");
  }
}

ASYNC_TEST("wss: certificate checks and errors match ws on Bun; echo and close handshake over TLS") {
  const TestCertificate local = makeCertificate("localhost", "DNS:localhost,IP:127.0.0.1");
  const TestCertificate other = makeCertificate("example.test", "DNS:example.test");
  auto server = std::make_shared<TlsEchoServer>(local);
  server->start();
  const std::string selfSigned =
    R"j([["client","error",{"error":"Error","message":"self signed certificate","code":"DEPTH_ZERO_SELF_SIGNED_CERT","errno":0,"syscall":null},2],["client","close",1006,{"buffer":""},3]])j";
  std::string log = co_await tlsClientLog(server, "localhost", noDeflate(), false);
  EXPECT_EQ(log, selfSigned);
  log = co_await tlsClientLog(server, "127.0.0.1", noDeflate(), false);
  EXPECT_EQ(log, selfSigned);
  ws::ClientOptions trusted = noDeflate();
  trusted.ca = local.certificate;
  log = co_await tlsClientLog(server, "localhost", trusted, true);
  EXPECT_EQ(log, "[" + OPENED + R"(,["client","message",{"buffer":"746c732d6563686f"},false,1],["client","close",1000,{"buffer":"646f6e65"},3]])");
  ws::ClientOptions insecure = noDeflate();
  insecure.rejectUnauthorized = false;
  log = co_await tlsClientLog(server, "127.0.0.1", insecure, false);
  EXPECT_EQ(log, "[" + OPENED + R"(,["client","message",{"buffer":"746c732d6563686f"},false,1]])");
  server->close();

  auto mismatch = std::make_shared<TlsEchoServer>(other);
  mismatch->start();
  ws::ClientOptions trustOther = noDeflate();
  trustOther.ca = other.certificate;
  log = co_await tlsClientLog(mismatch, "127.0.0.1", trustOther, false);
  EXPECT_EQ(log,
            R"j([["client","error",{"error":"Error","message":"Hostname/IP does not match certificate's altnames: IP: 127.0.0.1 is not in the cert's list: ","code":"ERR_TLS_CERT_ALTNAME_INVALID","errno":null,"syscall":null},2],["client","close",1006,{"buffer":""},3]])j");
  log = co_await tlsClientLog(mismatch, "localhost", trustOther, false);
  EXPECT_EQ(log,
            R"j([["client","error",{"error":"Error","message":"Hostname/IP does not match certificate's altnames: Host: localhost. is not in the cert's altnames: DNS:example.test","code":"ERR_TLS_CERT_ALTNAME_INVALID","errno":null,"syscall":null},2],["client","close",1006,{"buffer":""},3]])j");
  mismatch->close();
}

TEST("an exception thrown by a listener is uncaught; the socket carries on") {
  rt::Loop loop;
  loop.setUncaughtPolicy(rt::UncaughtPolicy::report);
  std::vector<std::string> seen;
  const auto body = [&seen]() -> rt::Task<void> {
    std::function<void(std::shared_ptr<ws::WebSocket>)> onAccept = [](std::shared_ptr<ws::WebSocket> socket) {
      ws::WebSocket* raw = socket.get();
      socket->on("message", [raw](const ws::EventArgs& args) { raw->send(args.data); });
    };
    auto server = startServer(serverOptions(1024), onAccept);
    server->listenTcp();
    auto client = ws::WebSocket::create("ws://127.0.0.1:" + std::to_string(server->port) + "/", noDeflate());
    client->on("message", [&seen](const ws::EventArgs& args) {
      const auto& bytes = std::get<ws::Bytes>(args.data);
      seen.emplace_back(bytes.begin(), bytes.end());
      if (seen.size() == 1) throw rt::JsError("listener failure");
    });
    client->on("message", [&seen] { seen.push_back("second listener"); });
    client->once("open", [client] {
      client->send(std::string("one"));
      client->send(std::string("two"));
    });
    WAIT_UNTIL(seen.size() == 3);
    client->close(1000);
    WAIT_UNTIL(client->readyState() == ws::WebSocket::CLOSED);
    server->close();
  };
  rt::blockOn(loop, rt::async(body));
  // The throwing emit skips the remaining listeners, as Node's emit does.
  EXPECT_EQ(seen, (std::vector<std::string>{"one", "two", "second listener"}));
  EXPECT_EQ(loop.uncaughtCount(), std::size_t(1));
}
