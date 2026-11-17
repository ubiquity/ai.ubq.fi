#include "fuzz/support.hpp"
#include "src/rt/fetch_transport_internal.hpp"
#include "src/rt/websocket_internal.hpp"

#include <string>

namespace {
  namespace fetch = uos::rt::detail::fetch;

  std::string_view nextFragment(std::string_view input) {
    return input.empty() ? input : input.substr(0, 1U + static_cast<unsigned char>(input.front()) % 32U);
  }

  void readFetchResponse(std::string_view input) {
    fetch::ResponseParser parser;
    std::size_t consumed = 0;
    for (;;) {
      const auto status = parser.readHead(nextFragment(input), consumed);
      input.remove_prefix(consumed);
      if (status == HPE_PAUSED) break;
      if (status != HPE_OK || input.empty()) return;
    }
    const auto head = parser.takeHead();
    if (head.status < 200 || fetch::responseFraming("GET", head).error) return;

    parser.beginBody();
    std::string body;
    do {
      const auto status = parser.readBody(nextFragment(input), consumed, body);
      if (status != HPE_OK) return;
      input.remove_prefix(consumed);
    } while (!input.empty());
    (void)parser.finish();
  }

  void readChunkedBody(std::string_view input) {
    fetch::ChunkedDecoder decoder;
    std::string body;
    std::size_t offset = 0;
    std::size_t end = 0;
    while (end < input.size()) {
      end += nextFragment(input.substr(end)).size();
      if (decoder.decode(input.substr(0, end), offset, body) != fetch::ChunkedDecoder::Result::more) return;
    }
  }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  const auto input = uos::fuzz::text(data, size);
  readFetchResponse(input);
  readChunkedBody(input);
  (void)fetch::parseResponseHead(input);
  uos::fuzz::malformed([&] { (void)uos::rt::ws::detail::parseResponseHead({data, size}); });
  return 0;
}
