#include "fuzz/support.hpp"
#include "uos/rt/regex.hpp"

#include <algorithm>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 4 || size > 1024) return 0;
  const std::size_t patternSize = std::min<std::size_t>(data[0], size - 4);
  const std::size_t flagSize = std::min<std::size_t>(data[1] & 0x0F, size - 4 - patternSize);
  const std::size_t remaining = size - 4 - patternSize - flagSize;
  const std::size_t subjectSize = data[2] == 0 ? remaining : std::min<std::size_t>(data[2], remaining);
  const auto pattern = uos::fuzz::text(data + 4, patternSize);
  const auto flagBytes = uos::fuzz::text(data + 4 + patternSize, flagSize);
  const auto* text = data + 4 + patternSize + flagSize;
  const auto subject = uos::fuzz::text(text, subjectSize);
  const auto replacement = uos::fuzz::text(text + subjectSize, remaining - subjectSize);

  constexpr std::string_view supportedFlags = "dgimsuvy";
  std::string flags;
  for (std::size_t i = 0; i < supportedFlags.size(); ++i)
    if (data[3] & (1u << i)) flags += supportedFlags[i];
  if (!flagBytes.empty()) flags.assign(flagBytes);

  uos::fuzz::malformed([&] {
    uos::rt::Regex regex(pattern, flags);
    regex.lastIndex = data[1] >> 4;
    (void)regex.exec(subject);
    regex.lastIndex = 0;
    (void)regex.test(subject);
    (void)uos::rt::search(subject, regex);
    if (regex.global()) (void)uos::rt::matchAll(subject, regex);
    (void)uos::rt::replace(subject, regex, replacement);
    (void)uos::rt::replace(subject, regex, [](const uos::rt::RegexMatch& match) { return match.str(); });
    (void)uos::rt::splitWithCaptures(subject, regex, 16);
  });
  return 0;
}
