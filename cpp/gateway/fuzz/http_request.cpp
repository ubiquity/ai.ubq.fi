// Finite test-TU access matches existing include-scoped private-helper tests.
#include "src/runtime/http.cpp"
#include "fuzz/support.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  uos::rt::Loop loop;
  uos::rt::LoopScope scope(loop);
  auto connection = std::make_shared<uos::runtime::http::node_http::ConnectionState>(boost::asio::generic::stream_protocol::socket(loop.context()));
  connection->input.assign(data, data + size);
  (void)connection->parseHead();
  if (connection->framing == uos::runtime::http::node_http::ConnectionState::Framing::chunked) {
    uos::rt::ByteChunk chunk;
    for (std::size_t step = 0; step <= size; ++step) {
      const auto progress = connection->decodeChunked(chunk);
      if (!progress || !*progress) break;
    }
  }
  connection->destroy(nullptr);
  loop.run();
  return 0;
}
