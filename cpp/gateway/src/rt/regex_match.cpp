// Feed the native subject to libregexp and translate its captures into UTF-16 offsets.
#include "regex_internal.hpp"

#include "libregexp.h"
#include "uos/rt/error.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>

namespace uos::rt::regex_detail {
  namespace {
    bool equalAsciiCase(std::string_view input, std::string_view lower) {
      return std::ranges::equal(input, lower, [](unsigned char left, unsigned char right) {
        return (left >= 'A' && left <= 'Z' ? left + ('a' - 'A') : left) == right;
      });
    }

    bool base64Alphabet(std::string_view value) {
      if (value.ends_with("==")) value.remove_suffix(2);
      else if (value.ends_with('=')) value.remove_suffix(1);
      return std::transform_reduce(value.begin(), value.end(), true, std::bit_and<bool>(), [](unsigned char byte) {
        const auto lower = byte | 0x20;
        return (lower >= 'a' && lower <= 'z') || (byte >= '0' && byte <= '9') || byte == '+' || byte == '/';
      });
    }

    // The fixed image pattern has two captures and no other observable state.
    // Match its large ASCII case with a linear scan to avoid VM work per base64 byte.
    bool matchAsciiImage(std::string_view subject, std::size_t* offsets) {
      constexpr std::string_view prefix = "data:";
      constexpr std::string_view separator = ";base64,";
      if (!equalAsciiCase(subject.substr(0, prefix.size()), prefix)) return false;
      const auto mediaEnd = subject.find_first_of(";,", prefix.size());
      if (mediaEnd == prefix.size() || mediaEnd == std::string_view::npos ||
          !equalAsciiCase(subject.substr(mediaEnd, separator.size()), separator)) return false;
      const auto dataStart = mediaEnd + separator.size();
      if (!base64Alphabet(subject.substr(dataStart))) return false;
      const std::array spans{std::size_t{0}, subject.size(), prefix.size(), mediaEnd, dataStart, subject.size()};
      std::ranges::copy(spans, offsets);
      return true;
    }
  }

  Matcher::Matcher(const Program& program, std::size_t subjectBytes)
    : program_(program), limits_(limits::kRegexMatchBackstop, std::min(limits::kRegexMaxPolls, program.workBudget.pollsFor(subjectBytes))) {
    if (program.registerCount > inlineCount) registers_ = std::make_unique<std::uint8_t*[]>(program.registerCount);
    const std::size_t count = 2 * (static_cast<std::size_t>(program.captureCount) + 1);
    if (count > inlineCount) offsets_ = std::make_unique_for_overwrite<std::size_t[]>(count);
  }

  bool Matcher::search(const void* subject, std::size_t length, std::size_t start, bool wide, bool splitting) {
    const int shift = wide ? 1 : 0;
    if (length > static_cast<std::size_t>(std::numeric_limits<int>::max() >> shift)) throw RangeError("regular expression subject is too large");
    if (start > length) return false;
    auto* offsets = offsets_ ? offsets_.get() : inlineOffsets_.data();
    if (program_.imageDataUrl && !wide && length > limits::kRegexSubjectBytes) {
      return start == 0 && matchAsciiImage({static_cast<const char*>(subject), length}, offsets);
    }
    if (program_.standardBase64 && length > limits::kRegexSubjectBytes) {
      if (wide || start != 0 || length % 4 != 0) return false;
      const bool matched = base64Alphabet({static_cast<const char*>(subject), length});
      offsets[0] = 0;
      offsets[1] = length;
      return matched;
    }
    // A sequence of short VM searches may never reach an engine interrupt.
    constexpr auto backstopError = "regular expression execution wall-clock backstop exceeded";
    if (searches_++ % 256 == 0 && limits_.checkBackstop()) throw RangeError(backstopError);
    const auto* bytes = static_cast<const std::uint8_t*>(subject);
    const auto* bytecode = splitting && program_.splitBytecode ? program_.splitBytecode.get() : program_.bytecode.get();
    auto* registers = registers_ ? registers_.get() : inlineRegisters_.data();
    const int result = lre_exec(registers, bytecode, bytes, static_cast<int>(start), static_cast<int>(length), shift, &limits_);
    if (result == 0) return false;
    if (result == LRE_RET_TIMEOUT) {
      if (limits_.backstopExceeded) throw RangeError(backstopError);
      throw RangeError("regular expression execution limit exceeded");
    }
    if (result == LRE_RET_MEMORY_ERROR) throw RangeError("regular expression memory limit exceeded");
    if (result != 1) throw JsError("invalid regular expression bytecode");
    const std::size_t count = 2 * (static_cast<std::size_t>(program_.captureCount) + 1);
    for (std::size_t i = 0; i < count; ++i)
      offsets[i] = registers[i] ? static_cast<std::size_t>(registers[i] - bytes) >> shift : kNoPos;
    return true;
  }
}
