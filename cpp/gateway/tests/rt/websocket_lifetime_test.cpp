// Links: tests/rt/websocket_lifetime_test.cpp (every src/rt/*.cpp links automatically).
// Teardown releases ownership without inventing events after the loop stops.
#include "src/rt/websocket_internal.hpp"
#include "uos/rt/test.hpp"

#include <boost/asio/ip/tcp.hpp>
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

TEST("a synchronous Unix handshake exception releases the unpublished WebSocket") {
  rt::FakeClock clock;
  rt::Loop loop(clock);
  rt::LoopScope scope(loop);
  const std::string address = "ws+unix:///" + std::string(128, 'x') + ":/";
  EXPECT_THROWS(ws::WebSocket::create(address), boost::system::system_error);
}
