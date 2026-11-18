// Links: tests/legacy/websocket_library_test.cpp (the runtime is linked automatically).
#include "src/rt/websocket_internal.hpp"
#include "uos/rt/test.hpp"

#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace rt = uos::rt;
namespace ws = uos::rt::ws;
namespace asio = boost::asio;

namespace {
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
    const ws::UpgradeRequest request{"GET", "/", {{"Host", "localhost"}, {"Upgrade", "websocket"}, {"Connection", "Upgrade"},
                                                      {"Sec-WebSocket-Version", "13"}, {"Sec-WebSocket-Key", std::move(key)},
                                                      {"Sec-WebSocket-Protocol", std::move(offer)}}};
    UpgradeResult result;
    server->handleUpgrade(request, socket, {}, [&result](const std::shared_ptr<ws::WebSocket>& websocket, const ws::UpgradeRequest&) {
      result.protocol = websocket->protocol();
      websocket->terminate();
    });
    loop.run();
    result.response.resize(peer.available());
    if (!result.response.empty()) result.response.resize(peer.read_some(asio::buffer(result.response)));
    return result;
  }
}

TEST("WebSocket validates received text even with the legacy skip flag") { expectRejectedFrame(1, {0xc3, 0x28}, 1007); }

TEST("WebSocket validates received close reasons even with the legacy skip flag") { expectRejectedFrame(8, {0x03, 0xe8, 0xff}, 1007); }

TEST("WebSocket rejects received close codes beyond the RFC range") { expectRejectedFrame(8, {0x13, 0x88}, 1002); }

TEST("WebSocket refuses an invalid handshake nonce and empty or duplicated subprotocols") {
  constexpr auto key = "dGhlIHNhbXBsZSBub25jZQ==";
  const auto badKey = upgrade("?GhlIHNhbXBsZSBub25jZQ==", "chat");
  EXPECT(badKey.response.starts_with("HTTP/1.1 400 "));
  EXPECT(!badKey.protocol);
  for (const auto* offer : {"", "chat,, json", "chat, chat", "chat,"}) {
    const auto refused = upgrade(key, offer);
    EXPECT(refused.response.starts_with("HTTP/1.1 400 "));
    EXPECT(!refused.protocol);
  }
  const auto accepted = upgrade(key, "chat,\tjson");
  EXPECT(accepted.response.starts_with("HTTP/1.1 101 "));
  EXPECT(accepted.response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
  EXPECT_EQ(accepted.protocol, std::optional<std::string>("chat"));
}
