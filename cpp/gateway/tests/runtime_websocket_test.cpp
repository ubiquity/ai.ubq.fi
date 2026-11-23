// src/runtime/websocket.ts has no bun tests. Differential facts recorded from
// the TS module under Bun 1.4 (loadWebSocketModule() twice, then awaited):
//   {"samePromise":true,"keys":["default","WebSocketServer"],"default":"function",
//    "server":"function","constants":[0,1,2,3],"third":true}
// and the constructors driven the way the Codex transports and
// src/runtime/http-upgrade.ts drive them.
#include "uos/runtime/websocket.hpp"

#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test_async.hpp"

#include <boost/asio/local/stream_protocol.hpp>

#include <array>
#include <memory>
#include <string>

namespace {
  using namespace uos;
  namespace rt = uos::rt;
  namespace ws = uos::rt::ws;
  namespace asio = boost::asio;

  // The HTTP server stage in miniature: one Unix-socket connection, its request
  // head parsed into a ws::UpgradeRequest and handed to `upgrade`.
  using Upgrade = std::function<void(const ws::UpgradeRequest& request, ws::UpgradeSocket& socket, ws::Bytes head)>;
  void acceptOne(std::shared_ptr<asio::local::stream_protocol::acceptor> acceptor, Upgrade upgrade) {
    acceptor->async_accept([acceptor, upgrade](const boost::system::error_code& error, asio::local::stream_protocol::socket accepted) {
      if (error) return;
      auto socket = std::make_shared<asio::generic::stream_protocol::socket>(std::move(accepted));
      auto received = std::make_shared<std::string>();
      auto buffer = std::make_shared<std::array<char, 4096>>();
      auto read = std::make_shared<std::function<void()>>();
      *read = [socket, received, buffer, read, upgrade] {
        socket->async_read_some(asio::buffer(*buffer), [socket, received, buffer, read, upgrade](const boost::system::error_code& failure, std::size_t count) {
          if (failure) {
            *read = nullptr;
            return;
          }
          received->append(buffer->data(), count);
          const std::size_t end = received->find("\r\n\r\n");
          if (end == std::string::npos) return (*read)();
          *read = nullptr;
          ws::UpgradeRequest request;
          const std::string head = received->substr(0, end);
          request.method = head.substr(0, head.find(' '));
          request.url = head.substr(head.find(' ') + 1, head.find(" HTTP/") - head.find(' ') - 1);
          std::size_t position = head.find("\r\n") + 2;
          while (position < head.size()) {
            std::size_t lineEnd = head.find("\r\n", position);
            if (lineEnd == std::string::npos) lineEnd = head.size();
            const std::string line = head.substr(position, lineEnd - position);
            const std::size_t colon = line.find(':');
            request.rawHeaders.emplace_back(line.substr(0, colon), line.substr(colon + 2));
            position = lineEnd + 2;
          }
          ws::Bytes rest(received->begin() + static_cast<std::ptrdiff_t>(end + 4), received->end());
          ws::UpgradeSocket upgradeSocket{std::move(*socket)};
          upgrade(request, upgradeSocket, std::move(rest));
        });
      };
      (*read)();
    });
  }

  rt::Task<void> waitFor(std::function<bool()> condition) {
    for (int step = 0; step < 400 && !condition(); ++step) co_await rt::sleep(5);
  }
}

ASYNC_TEST("loadWebSocketModule returns one promise holding ws's two constructors") {
  rt::Promise<runtime::websocket::WebSocketModule> first = runtime::websocket::loadWebSocketModule();
  rt::Promise<runtime::websocket::WebSocketModule> second = runtime::websocket::loadWebSocketModule();
  EXPECT(first == second);
  runtime::websocket::WebSocketModule module = co_await first.wait();
  EXPECT(static_cast<bool>(module.default_));
  EXPECT(static_cast<bool>(module.WebSocketServer));
  EXPECT(runtime::websocket::loadWebSocketModule() == first);
  EXPECT_EQ((std::array<int, 4>{ws::WebSocket::CONNECTING, ws::WebSocket::OPEN, ws::WebSocket::CLOSING, ws::WebSocket::CLOSED}), (std::array<int, 4>{0, 1, 2, 3}));
}

ASYNC_TEST("the loaded constructors serve the supervisor transport's ws+unix JSON-RPC exchange") {
  runtime::websocket::WebSocketModule module = co_await runtime::websocket::loadWebSocketModule().wait();
  const std::string directory = rt::fs::mkdtempSync(rt::os::tmpdir() + "/uos-runtime-ws-");
  const std::string socketPath = directory + "/app-server-control.sock";
  auto acceptor = std::make_shared<asio::local::stream_protocol::acceptor>(rt::executor(), asio::local::stream_protocol::endpoint(socketPath));
  auto requests = std::make_shared<std::vector<std::string>>();
  auto hosts = std::make_shared<std::vector<std::string>>();
  const auto makeServer = module.WebSocketServer;
  Upgrade upgrade = [makeServer, requests, hosts](const ws::UpgradeRequest& request, ws::UpgradeSocket& socket, ws::Bytes head) {
    ws::ServerOptions options;
    options.noServer = true;
    options.clientTracking = false;
    options.perMessageDeflate = false;
    options.maxPayload = 1048576;
    std::shared_ptr<ws::WebSocketServer> upgradeServer = makeServer(options);
    hosts->push_back(request.header("host").value_or("<none>"));
    upgradeServer->handleUpgrade(request, socket, std::move(head), [requests](std::shared_ptr<ws::WebSocket> server, const ws::UpgradeRequest&) {
      ws::WebSocket* raw = server.get();
      server->on("message", [raw, requests](const ws::EventArgs& args) {
        const auto& bytes = std::get<ws::Bytes>(args.data);
        const std::string text(bytes.begin(), bytes.end());
        requests->push_back(text);
        const json::value parsed = json::parse(text);
        if (parsed.as_object().contains("id")) raw->send(std::string(R"({"id":)") + json::serialize(parsed.at("id")) + R"(,"result":{"ok":true}})");
      });
    });
    upgradeServer->close();
  };
  acceptOne(acceptor, upgrade);
  ws::ClientOptions options;
  options.headers = {{"Host", "localhost"}};
  options.perMessageDeflate = false;
  options.maxPayload = 1048576;
  options.handshakeTimeout = 3000;
  std::shared_ptr<ws::WebSocket> socket = module.default_("ws+unix://" + socketPath + ":/", options);
  auto replies = std::make_shared<std::vector<std::string>>();
  socket->on("message", [replies](const ws::EventArgs& args) {
    const auto& bytes = std::get<ws::Bytes>(args.data);
    replies->emplace_back(bytes.begin(), bytes.end());
  });
  socket->once("open", [socket] {
    socket->send(std::string(R"({"id":1,"method":"initialize","params":{}})"));
    socket->send(std::string(R"({"method":"initialized"})"));
  });
  std::function<bool()> answered = [replies] { return replies->size() == 1; };
  co_await waitFor(answered);
  EXPECT_EQ(*replies, (std::vector<std::string>{R"({"id":1,"result":{"ok":true}})"}));
  EXPECT_EQ(*hosts, (std::vector<std::string>{"localhost"}));
  std::function<bool()> both = [requests] { return requests->size() == 2; };
  co_await waitFor(both);
  EXPECT_EQ(*requests, (std::vector<std::string>{R"({"id":1,"method":"initialize","params":{}})", R"({"method":"initialized"})"}));
  socket->close(1000, std::string("supervisor sample complete"));
  std::function<bool()> closed = [socket] { return socket->readyState() == ws::WebSocket::CLOSED; };
  co_await waitFor(closed);
  EXPECT_EQ(socket->readyState(), ws::WebSocket::CLOSED);
  boost::system::error_code ignored;
  acceptor->close(ignored);
  rt::fs::rmSync(directory, {.recursive = true, .force = true});
}
