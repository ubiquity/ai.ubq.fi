#include "fuzz/support.hpp"
#include "src/rt/websocket_internal.hpp"
#include "uos/rt/async.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  uos::rt::Loop loop;
  uos::rt::LoopScope scope(loop);
  for (const bool server : {false, true}) {
    auto receiver = std::make_shared<uos::rt::ws::detail::Receiver>(uos::rt::ws::detail::Receiver::Options{server, 16384, false});
    receiver->onError = [](uos::rt::Reason, int) {};
    receiver->onMessage = [](uos::rt::ws::Bytes, bool) {};
    receiver->onConclude = [](int, uos::rt::ws::Bytes) {};
    for (std::size_t offset = 0; offset < size;) {
      const std::size_t count = std::min(size - offset, std::size_t{1} + data[offset] % 32);
      receiver->write({data + offset, count});
      offset += count;
    }
    receiver->end();
  }
  loop.run();
  return 0;
}
