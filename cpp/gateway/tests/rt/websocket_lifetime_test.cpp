// Links: tests/rt/websocket_lifetime_test.cpp (every src/rt/*.cpp links automatically).
// Teardown releases ownership without inventing events after the loop stops.
#include "src/rt/websocket_internal.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/test.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>
#include <memory>
#include <string>
#include <utility>

namespace asio = boost::asio;
namespace rt = uos::rt;
namespace ws = uos::rt::ws;

namespace {
  std::shared_ptr<ws::WebSocket> openSocket(asio::ip::tcp::socket socket) {
    auto websocket = std::make_shared<ws::WebSocket>(nullptr);
    auto stream = std::make_shared<ws::detail::Socket>(ws::detail::Socket::Stream(std::move(socket)));
    ws::detail::setSocket(websocket, std::move(stream), {}, 1024, false);
    return websocket;
  }

  void checkLoopTeardown(bool terminate) {
    std::weak_ptr<ws::WebSocket> weak;
    bool closed = false;
    {
      rt::FakeClock clock;
      rt::Loop loop(clock);
      rt::LoopScope scope(loop);
      asio::ip::tcp::acceptor acceptor(rt::executor(), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
      asio::ip::tcp::socket client(rt::executor());
      client.connect(acceptor.local_endpoint());
      auto peer = acceptor.accept();
      auto websocket = openSocket(std::move(client));
      weak = websocket;
      websocket->on("close", [&closed] { closed = true; });
      websocket->on("message", [websocket] {});
      if (terminate) websocket->terminate();
      websocket.reset();
      EXPECT(!weak.expired());
    }
    EXPECT(weak.expired());
    EXPECT(!closed);
  }
}

TEST("loop teardown releases open WebSocket cycles without emitting close") { checkLoopTeardown(false); }

TEST("loop teardown releases a WebSocket whose close completion was abandoned") { checkLoopTeardown(true); }

TEST("an externally retained WebSocket releases its Asio socket at loop teardown") {
  for (bool closeFirst : {false, true}) {
    std::shared_ptr<ws::WebSocket> websocket;
    std::weak_ptr<ws::detail::Socket> native;
    {
      rt::FakeClock clock;
      rt::Loop loop(clock);
      rt::LoopScope scope(loop);
      asio::ip::tcp::acceptor acceptor(rt::executor(), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
      asio::ip::tcp::socket client(rt::executor());
      client.connect(acceptor.local_endpoint());
      auto peer = acceptor.accept();
      websocket = openSocket(std::move(client));
      native = websocket->state().socket;
      if (closeFirst) {
        websocket->terminate();
        loop.run();
        EXPECT_EQ(websocket->readyState(), ws::WebSocket::CLOSED);
      }
    }
    EXPECT(native.expired());
    EXPECT_EQ(websocket->readyState(), ws::WebSocket::CLOSED);
    websocket.reset();
  }
}

TEST("loop teardown releases an opening WebSocket and its pending connect attempt") {
  std::weak_ptr<ws::WebSocket> weak;
  {
    rt::FakeClock clock;
    rt::Loop loop(clock);
    rt::LoopScope scope(loop);
    asio::ip::tcp::acceptor acceptor(rt::executor(), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto websocket = ws::WebSocket::create("ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/");
    weak = websocket;
    websocket.reset();
    EXPECT(!weak.expired());
  }
  EXPECT(weak.expired());
}

TEST("WebSocket idle expiry refreshes on activity and closes at sixty seconds") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  rt::LoopScope scope(loop);
  asio::ip::tcp::acceptor acceptor(rt::executor(), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  asio::ip::tcp::socket client(rt::executor());
  client.connect(acceptor.local_endpoint());
  auto peer = acceptor.accept();
  auto websocket = openSocket(std::move(client));
  int code = 0;
  websocket->once("close", [&](const ws::EventArgs& event) { code = event.code; });
  constexpr auto idleMs = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(rt::limits::kWebSocketIdleTimeout).count());
  rt::queueMicrotask([&] {
    clock.advance(idleMs - 1);
    EXPECT_EQ(websocket->readyState(), ws::WebSocket::OPEN);
    websocket->state().socket->onActivity();
    clock.advance(idleMs - 1);
    rt::queueMicrotask([&] {
      EXPECT_EQ(websocket->readyState(), ws::WebSocket::OPEN);
      clock.advance(1);
    });
  });
  loop.run();
  EXPECT_EQ(websocket->readyState(), ws::WebSocket::CLOSED);
  EXPECT_EQ(code, 1006);
}

TEST("a synchronous Unix handshake exception releases the unpublished WebSocket") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  rt::LoopScope scope(loop);
  const std::string address = "ws+unix:///" + std::string(128, 'x') + ":/";
  EXPECT_THROWS(ws::WebSocket::create(address), boost::system::system_error);
}

TEST("public event delivery retains the WebSocket through its last listener") {
  auto websocket = std::make_shared<ws::WebSocket>(nullptr);
  const std::weak_ptr<ws::WebSocket> weak = websocket;
  ws::EventEmitter& emitter = *websocket;
  int calls = 0;
  emitter.on("message", [&] { websocket.reset(); });
  emitter.once("message", [&] {
    EXPECT(!weak.expired());
    ++calls;
  });

  EXPECT(emitter.emit("message"));
  EXPECT_EQ(calls, 1);
  EXPECT(weak.expired());
}

TEST("a WebSocket upgrade retains its server through callbacks that release it") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  rt::LoopScope scope(loop);
  asio::local::stream_protocol::socket client(rt::executor()), peer(rt::executor());
  asio::local::connect_pair(client, peer);
  ws::UpgradeSocket socket{ws::detail::Socket::Stream(std::move(client))};
  const ws::UpgradeRequest request{"GET", "/", {{"Upgrade", "websocket"}, {"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="}, {"Sec-WebSocket-Version", "13"}}};
  std::shared_ptr<ws::WebSocketServer> server;
  ws::ServerOptions options;
  options.noServer = true;
  options.WebSocket = [&server](const ws::ServerOptions& duringUpgrade) {
    server.reset();
    return std::make_shared<ws::WebSocket>(nullptr, duringUpgrade.autoPong);
  };
  server = std::make_shared<ws::WebSocketServer>(std::move(options));
  const std::weak_ptr<ws::WebSocketServer> weak = server;
  std::shared_ptr<ws::WebSocket> accepted;
  server->handleUpgrade(request, socket, {}, [&](std::shared_ptr<ws::WebSocket> websocket, const ws::UpgradeRequest&) {
    EXPECT(!weak.expired());
    accepted = std::move(websocket);
  });
  EXPECT(weak.expired());
  ASSERT(accepted);
  EXPECT_EQ(accepted->readyState(), ws::WebSocket::OPEN);
  accepted->terminate();
  loop.run();
}

TEST("backpressured socket writes survive partial completion and report cancellation once") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  rt::LoopScope scope(loop);
  asio::local::stream_protocol::socket client(rt::executor()), peer(rt::executor());
  asio::local::connect_pair(client, peer);
  client.set_option(asio::socket_base::send_buffer_size(4096));
  auto socket = std::make_shared<ws::detail::Socket>(ws::detail::Socket::Stream(std::move(client)));

  const ws::Bytes payload(128 * 1024, 0x42);
  ws::Bytes received(payload.size());
  int completed = 0;
  socket->write(payload, [&](rt::Reason error) {
    EXPECT(!error);
    ++completed;
  });
  ASSERT_EQ(socket->writableLength(), payload.size());
  asio::async_read(peer, asio::buffer(received), [&](const boost::system::error_code& error, std::size_t size) {
    EXPECT(!error);
    EXPECT_EQ(size, payload.size());
  });
  loop.run();
  EXPECT_EQ(received, payload);
  EXPECT_EQ(completed, 1);
  EXPECT_EQ(socket->writableLength(), 0u);

  int cancelled = 0;
  socket->write(payload, [&](rt::Reason error) {
    EXPECT(error);
    ++cancelled;
  });
  ASSERT_EQ(socket->writableLength(), payload.size());
  socket->destroy();
  loop.run();
  EXPECT_EQ(cancelled, 1);
  EXPECT_EQ(socket->writableLength(), 0u);
}
