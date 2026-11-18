#include "fuzz/support.hpp"
#include "src/rt/fetch_transport_internal.hpp"
#include "src/rt/websocket_internal.hpp"

namespace {
  namespace fetch = uos::rt::detail::fetch;

  std::string_view nextFragment(std::string_view input) {
    return input.empty() ? input : input.substr(0, 1U + static_cast<unsigned char>(input.front()) % 32U);
  }

  void readBody(fetch::ResponseParser& parser, std::string_view input) {
    while (!input.empty()) {
      uos::rt::ByteChunk body;
      std::size_t consumed = 0;
      if (parser.readBody(nextFragment(input), consumed, &body) != HPE_OK) return;
      input.remove_prefix(consumed);
    }
    (void)parser.finish();
  }

  void readFetchResponse(std::string_view input, bool headRequest) {
    fetch::ResponseParser parser;
    while (!input.empty()) {
      std::size_t consumed = 0;
      const auto result = parser.readHead(nextFragment(input), consumed);
      input.remove_prefix(consumed);
      if (result == HPE_OK) continue;
      if (result != HPE_PAUSED || parser.head().status == 101 || parser.head().framing.error) return;
      const auto bodyStatus = parser.beginBody(headRequest);
      if (parser.head().status >= 100 && parser.head().status < 200 && bodyStatus == HPE_PAUSED) {
        parser.reset();
        continue;
      }
      if (bodyStatus == HPE_OK) readBody(parser, input);
      return;
    }
  }

  void readChunkedBody(std::string_view input) {
    fetch::ResponseParser parser;
    std::size_t consumed = 0;
    if (parser.readHead("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", consumed) != HPE_PAUSED) return;
    if (parser.beginBody() == HPE_OK) readBody(parser, input);
  }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  const auto input = uos::fuzz::text(data, size);
  readFetchResponse(input, false);
  readFetchResponse(input, true);
  readChunkedBody(input);
  (void)fetch::parseResponseHead(input);
  uos::fuzz::malformed([&] { (void)uos::rt::ws::detail::parseResponseHead({data, size}); });
  return 0;
}
