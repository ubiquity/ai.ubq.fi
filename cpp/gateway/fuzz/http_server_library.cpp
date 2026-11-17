#include "src/runtime/http.cpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0 || size > 32768) return 0;
  namespace rt = uos::rt;
  using uos::runtime::http::node_http::ConnectionState;
  rt::Loop loop;
  rt::LoopScope scope(loop);
  auto connection = std::make_shared<ConnectionState>(boost::asio::generic::stream_protocol::socket(loop.context()));

  const std::size_t stride = 1 + (data[0] & 63u);
  for (std::size_t offset = 0; offset < size && !connection->destroyed; offset += stride) {
    const auto end = std::min(size, offset + stride);
    connection->input.insert(connection->input.end(), data + offset, data + end);
    if (connection->phase == ConnectionState::Phase::head && !connection->parseHead()) continue;
    if (connection->framing != ConnectionState::Framing::chunked || connection->phase != ConnectionState::Phase::body) break;

    while (!connection->input.empty() && !connection->bodyComplete) {
      rt::ByteChunk chunk;
      const auto progress = connection->decodeChunked(chunk);
      if (!progress) {
        connection->destroy(nullptr);
        break;
      }
      if (!*progress) break;
    }
    if (connection->bodyComplete) break;
  }
  connection->destroy(nullptr);
  loop.run();
  return 0;
}
