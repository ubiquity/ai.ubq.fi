#include "fuzz/support.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/websocket.hpp"

#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>

namespace {
  namespace asio = boost::asio;
  namespace rt = uos::rt;
  namespace ws = rt::ws;

  rt::Task<void> drainPeer(asio::local::stream_protocol::socket peer) {
    std::array<std::uint8_t, 4096> storage;
    const auto output = asio::buffer(storage);
    boost::system::error_code error;
    do {
      auto reading = peer.async_read_some(output, asio::redirect_error(asio::use_awaitable, error));
      co_await std::move(reading);
    } while (!error);
    if (error != asio::error::eof) throw boost::system::system_error(error);
  }

  void exercise(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return;
    const std::uint8_t mode = bytes.front();
    bytes = bytes.subspan(1);
    const auto input = uos::fuzz::text(bytes.data(), bytes.size());
    const auto key = (mode & 1) != 0 ? std::string(input) : "dGhlIHNhbXBsZSBub25jZQ==";
    ws::UpgradeRequest request{"GET", "/", {{"Host", "localhost"}, {"Upgrade", "websocket"}, {"Connection", "Upgrade"}, {"Sec-WebSocket-Version", "13"}, {"Sec-WebSocket-Key", key}}};
    if ((mode & 4) != 0) request.rawHeaders.emplace_back("Sec-WebSocket-Protocol", input);
    rt::Loop loop;
    rt::LoopScope scope(loop);
    asio::local::stream_protocol::socket accepted(rt::executor()), peer(rt::executor());
    asio::local::connect_pair(accepted, peer);
    ws::UpgradeSocket socket{std::move(accepted)};
    ws::ServerOptions options;
    options.noServer = true;
    options.clientTracking = false;
    options.maxPayload = 4096;
    auto server = std::make_shared<ws::WebSocketServer>(options);
    ws::Bytes head;
    if ((mode & 5) == 0) head.assign(bytes.begin(), bytes.end());
    server->handleUpgrade(request, socket, std::move(head), [bytes, mode](const std::shared_ptr<ws::WebSocket>& websocket, const ws::UpgradeRequest&) {
      websocket->on("error", [](const ws::EventArgs& event) { uos::fuzz::malformed([&] { rt::rethrow(event.error); }); });
      const auto outgoing = bytes.first(std::min(bytes.size(), std::size_t{256}));
      ws::SendOptions sendOptions;
      sendOptions.mask = (mode & 2) != 0;
      websocket->send(ws::Bytes(outgoing.begin(), outgoing.end()), sendOptions);
    });
    peer.shutdown(asio::socket_base::shutdown_send);
    rt::spawn(drainPeer(std::move(peer)));
    loop.run();
  }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size <= uos::fuzz::maxInput) uos::fuzz::malformed([&] { exercise({data, size}); });
  return 0;
}
