// Links: tests/legacy/websocket_library_test.cpp (the runtime is linked automatically).
#include "src/rt/websocket_internal.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/test.hpp"

#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace rt = uos::rt;
namespace ws = uos::rt::ws;
namespace asio = boost::asio;

namespace {
  constexpr auto sampleKey = "dGhlIHNhbXBsZSBub25jZQ==";

  ws::UpgradeRequest upgradeRequest(std::string key, std::string offer) {
    return {"GET", "/", {{"Host", "localhost"}, {"Upgrade", "websocket"}, {"Connection", "Upgrade"}, {"Sec-WebSocket-Version", "13"},
                         {"Sec-WebSocket-Key", std::move(key)}, {"Sec-WebSocket-Protocol", std::move(offer)}}};
  }

  std::string readWritten(asio::local::stream_protocol::socket& peer) {
    std::string response(peer.available(), '\0');
    asio::read(peer, asio::buffer(response));
    return response;
  }

  void expectRejectedFrame(int opcode, ws::Bytes payload, int expectedClose) {
    rt::Loop loop;
    rt::LoopScope scope(loop);
    auto receiver = std::make_shared<ws::detail::Receiver>(ws::detail::Receiver::Options{.skipUTF8Validation = true});
    int close = 0;
    bool delivered = false;
    receiver->onMessage = [&delivered](ws::Bytes, bool) { delivered = true; };
    receiver->onConclude = [&delivered](int, ws::Bytes) { delivered = true; };
    receiver->onError = [&close](rt::Reason, int code) { close = code; };
    receiver->write(ws::detail::Sender::frame(payload, opcode, true, false, false));
    loop.run();
    EXPECT_EQ(close, expectedClose);
    EXPECT(!delivered);
  }

  struct UpgradeResult {
    std::string response;
    std::optional<std::string> protocol;
  };

  UpgradeResult upgrade(std::string key, std::string offer) {
    rt::Loop loop;
    rt::LoopScope scope(loop);
    asio::local::stream_protocol::socket accepted(rt::executor()), peer(rt::executor());
    asio::local::connect_pair(accepted, peer);
    ws::UpgradeSocket socket{asio::generic::stream_protocol::socket(std::move(accepted))};
    auto server = std::make_shared<ws::WebSocketServer>(ws::ServerOptions{.noServer = true, .clientTracking = false});
    const auto request = upgradeRequest(std::move(key), std::move(offer));
    UpgradeResult result;
    server->handleUpgrade(request, socket, {}, [&result](const std::shared_ptr<ws::WebSocket>& websocket, const ws::UpgradeRequest&) {
      result.protocol = websocket->protocol();
      websocket->terminate();
    });
    loop.run();
    result.response = readWritten(peer);
    return result;
  }

  class LimitCloseSocket final : public ws::WebSocket {
  public:
    LimitCloseSocket() : WebSocket(nullptr, true) {}

    void close(std::optional<int> code, std::optional<ws::Data> reason) override {
      if (code == 1009 && !reason) return WebSocket::close(1011, ws::Data("fixture limit"));
      WebSocket::close(code, std::move(reason));
    }
  };
}

TEST("WebSocket receiver accepts the byte ceiling and rejects a continuation one byte above it") {
  for (const std::size_t extra : {0u, 1u}) {
    rt::Loop loop;
    rt::LoopScope scope(loop);
    auto receiver = std::make_shared<ws::detail::Receiver>(ws::detail::Receiver::Options{});
    std::size_t delivered = 0;
    int close = 0;
    receiver->onMessage = [&](ws::Bytes bytes, bool) { delivered = bytes.size(); };
    receiver->onError = [&](rt::Reason, int code) { close = code; };
    constexpr auto limit = rt::limits::kWebSocketMessageBytes;
    receiver->write(ws::detail::Sender::frame(ws::Bytes(limit - 1, 'a'), 2, false, false, false));
    receiver->write(ws::detail::Sender::frame(ws::Bytes(1 + extra, 'b'), 0, true, false, false));
    loop.run();
    EXPECT_EQ(delivered, extra == 0 ? limit : 0);
    EXPECT_EQ(close, extra == 0 ? 0 : 1009);
  }
}

TEST("WebSocket fragmentation counts empty data frames and ignores control frames") {
  for (const std::size_t extra : {0u, 1u}) {
    rt::Loop loop;
    rt::LoopScope scope(loop);
    auto receiver = std::make_shared<ws::detail::Receiver>(ws::detail::Receiver::Options{});
    int delivered = 0;
    int close = 0;
    receiver->onMessage = [&](ws::Bytes, bool) { ++delivered; };
    receiver->onError = [&](rt::Reason, int code) { close = code; };
    const auto continuation = ws::detail::Sender::frame({}, 0, false, false, false);
    receiver->write(ws::detail::Sender::frame({}, 2, false, false, false));
    receiver->write(ws::detail::Sender::frame({}, 9, true, false, false));
    for (std::size_t i = 2; i < rt::limits::kWebSocketFragments + extra; ++i) receiver->write(continuation);
    receiver->write(ws::detail::Sender::frame({}, 0, true, false, false));
    loop.run();
    EXPECT_EQ(delivered, extra == 0 ? 1 : 0);
    EXPECT_EQ(close, extra == 0 ? 0 : 1009);
  }
}

TEST("WebSocket validates received text even with the legacy skip flag") { expectRejectedFrame(1, {0xc3, 0x28}, 1007); }

TEST("WebSocket validates received close reasons even with the legacy skip flag") { expectRejectedFrame(8, {0x03, 0xe8, 0xff}, 1007); }

TEST("WebSocket rejects received close codes beyond the RFC range") { expectRejectedFrame(8, {0x13, 0x88}, 1002); }

TEST("WebSocket refuses an invalid handshake nonce and empty or duplicated subprotocols") {
  const auto badKey = upgrade("?GhlIHNhbXBsZSBub25jZQ==", "chat");
  EXPECT(badKey.response.starts_with("HTTP/1.1 400 "));
  EXPECT(!badKey.protocol);
  for (const auto* offer : {"", "chat,, json", "chat, chat", "chat,"}) {
    const auto refused = upgrade(sampleKey, offer);
    EXPECT(refused.response.starts_with("HTTP/1.1 400 "));
    EXPECT(!refused.protocol);
  }
  const auto accepted = upgrade(sampleKey, "chat,\tjson");
  EXPECT(accepted.response.starts_with("HTTP/1.1 101 "));
  EXPECT(accepted.response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
  EXPECT_EQ(accepted.protocol, std::optional<std::string>("chat"));
}

TEST("WebSocket writes the route-specific close at an oversized continuation header") {
  rt::Loop loop;
  rt::LoopScope scope(loop);
  asio::local::stream_protocol::socket accepted(rt::executor()), peer(rt::executor());
  asio::local::connect_pair(accepted, peer);
  ws::UpgradeSocket socket{std::move(accepted)};
  ws::ServerOptions options{.noServer = true, .clientTracking = false, .maxPayload = 1024};
  options.WebSocket = [](const ws::ServerOptions&) { return std::make_shared<LimitCloseSocket>(); };
  auto server = std::make_shared<ws::WebSocketServer>(std::move(options));
  auto head = ws::detail::Sender::frame(ws::Bytes(512, 0), 2, false, false, true);
  // The continuation declares 513 more bytes. The peer withholds its mask and payload.
  head.insert(head.end(), {0x80, 0xfe, 0x02, 0x01});
  int messages = 0;
  bool rejected = false;
  const auto timeout = rt::setTimeout([&peer] { peer.shutdown(asio::socket_base::shutdown_send); }, 1000);
  server->handleUpgrade(upgradeRequest(sampleKey, "chat"), socket, std::move(head),
                        [&](const std::shared_ptr<ws::WebSocket>& websocket, const ws::UpgradeRequest&) {
    websocket->on("message", [&messages] { ++messages; });
    websocket->on("error", [&] {
      rejected = true;
      rt::clearTimeout(timeout);
      peer.shutdown(asio::socket_base::shutdown_send);
    });
  });
  loop.run();

  EXPECT(rejected);
  EXPECT_EQ(messages, 0);
  const auto response = readWritten(peer);
  EXPECT(response.starts_with("HTTP/1.1 101 "));
  EXPECT(response.ends_with("\x88\x0f\x03\xf3" "fixture limit"));
}
