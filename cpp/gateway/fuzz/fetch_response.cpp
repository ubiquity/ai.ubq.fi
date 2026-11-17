#include "fuzz/support.hpp"
#include "src/rt/fetch_transport_internal.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  const auto input = uos::fuzz::text(data, size);
  auto parsed = uos::rt::detail::fetch::parseResponseHead(input);
  if (parsed.status == uos::rt::detail::fetch::HeadStatus::complete) {
    (void)uos::rt::detail::fetch::responseFraming("GET", parsed.head);
    (void)uos::rt::detail::fetch::responseFraming("HEAD", parsed.head);
  }
  uos::rt::detail::fetch::ChunkedDecoder decoder;
  std::size_t offset = 0;
  std::string output;
  for (std::size_t end = 0; end < input.size();) {
    end = std::min(input.size(), end + 1 + (static_cast<unsigned char>(input[end]) % 32));
    const auto result = decoder.decode(input.substr(0, end), offset, output);
    if (result != uos::rt::detail::fetch::ChunkedDecoder::Result::more) break;
  }
  return 0;
}
