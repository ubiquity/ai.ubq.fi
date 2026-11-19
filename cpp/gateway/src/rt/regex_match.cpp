// Feed the native subject to libregexp and translate its captures into UTF-16 offsets.
#include "regex_internal.hpp"

#include "libregexp.h"
#include "uos/rt/error.hpp"

#include <limits>

namespace uos::rt::regex_detail {
  Matcher::Matcher(const Program& program) : program_(program) {
    if (program.registerCount > inlineCount) registers_ = std::make_unique<std::uint8_t*[]>(program.registerCount);
    const std::size_t count = 2 * (static_cast<std::size_t>(program.captureCount) + 1);
    if (count > inlineCount) offsets_ = std::make_unique_for_overwrite<std::size_t[]>(count);
  }

  bool Matcher::search(const void* subject, std::size_t length, std::size_t start, bool wide, bool splitting) {
    const int shift = wide ? 1 : 0;
    if (length > static_cast<std::size_t>(std::numeric_limits<int>::max() >> shift)) throw RangeError("regular expression subject is too large");
    if (start > length) return false;
    // Also check the shared deadline between searches that are individually too short to poll.
    if (++searches_ % 256 == 0 && Limits::Clock::now() >= limits_.deadline) throw RangeError("regular expression execution limit exceeded");
    const auto* bytes = static_cast<const std::uint8_t*>(subject);
    const auto* bytecode = splitting && program_.splitBytecode ? program_.splitBytecode.get() : program_.bytecode.get();
    auto* registers = registers_ ? registers_.get() : inlineRegisters_.data();
    const int result = lre_exec(registers, bytecode, bytes, static_cast<int>(start), static_cast<int>(length), shift, &limits_);
    if (result == 0) return false;
    if (result == LRE_RET_TIMEOUT) throw RangeError("regular expression execution limit exceeded");
    if (result == LRE_RET_MEMORY_ERROR) throw RangeError("regular expression memory limit exceeded");
    if (result != 1) throw JsError("invalid regular expression bytecode");
    auto* offsets = offsets_ ? offsets_.get() : inlineOffsets_.data();
    const std::size_t count = 2 * (static_cast<std::size_t>(program_.captureCount) + 1);
    for (std::size_t i = 0; i < count; ++i)
      offsets[i] = registers[i] ? static_cast<std::size_t>(registers[i] - bytes) >> shift : kNoPos;
    return true;
  }
}
