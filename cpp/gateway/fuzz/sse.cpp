// Finite test-TU access to the real private framer; no production API is added.
#include "src/responses_stream.cpp"
#include "fuzz/support.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  uos::fuzz::malformed([&] {
    uos::responses_stream::SseEventFramer framer(uos::rt::TextDecoder{});
    for (std::size_t offset = 0; offset < size;) {
      const std::size_t count = std::min(size - offset, std::size_t{1} + data[offset] % 32);
      const uos::rt::ByteChunk chunk(data + offset, data + offset + count);
      uos::responses_stream::SseEventFramer::Frames frames{&chunk};
      while (auto frame = framer.next(frames)) (void)uos::responses_stream::parseEventBlock(*frame);
      offset += count;
    }
    (void)uos::responses_stream::parseEventBlock(framer.flush());
  });
  return 0;
}
