#pragma once

#include "uos/rt/limits.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uos::rt::regex_detail {
  inline constexpr std::size_t kNoPos = static_cast<std::size_t>(-1);

  inline bool isHighSurrogate(std::uint32_t unit) { return unit >= 0xD800 && unit <= 0xDBFF; }
  inline bool isLowSurrogate(std::uint32_t unit) { return unit >= 0xDC00 && unit <= 0xDFFF; }

  struct NamedGroup {
    std::string name;
    std::vector<std::uint32_t> groups;
  };

  struct FreeBytecode {
    void operator()(std::uint8_t* data) const noexcept { std::free(data); }
  };
  using Bytecode = std::unique_ptr<std::uint8_t, FreeBytecode>;

  struct Program {
    std::string source;
    std::string flags;
    bool hasIndices = false;
    bool global = false;
    bool ignoreCase = false;
    bool multiline = false;
    bool dotAll = false;
    bool unicode = false;
    bool unicodeSets = false;
    bool sticky = false;
    bool fullUnicode() const { return unicode || unicodeSets; }
    std::uint32_t captureCount = 0;
    std::size_t registerCount = 0;
    std::size_t subjectBytes = limits::kRegexSubjectBytes;
    limits::RegexWorkBudget workBudget = limits::kRegexWorkBudget;
    bool sseFraming = false;
    bool imageDataUrl = false;
    bool standardBase64 = false;
    std::vector<NamedGroup> names;
    Bytecode bytecode;
    // String.prototype.split searches even if the input regex was sticky.
    Bytecode splitBytecode;
  };

  struct Limits {
    using Clock = std::chrono::steady_clock;
    // Internal thread-local seam for testing elapsed time without sleeping.
    inline static thread_local Clock::time_point (*clockForTest)() noexcept = nullptr;

    explicit Limits(Clock::duration backstop, std::size_t polls = 0) : deadline(now() + backstop), pollsRemaining(polls) {}
    bool checkBackstop() noexcept {
      backstopExceeded = now() >= deadline;
      return backstopExceeded;
    }

    static Clock::time_point now() noexcept { return clockForTest ? clockForTest() : Clock::now(); }
    Clock::time_point deadline;
    std::uintptr_t stackFloor = 0;
    // libregexp calls the hook once per 10,000 backtracking/loop polls.
    std::size_t pollsRemaining;
    bool backstopExceeded = false;
  };

  std::uintptr_t compileStackFloor(std::uintptr_t entry);
  std::shared_ptr<const Program> compile(std::string_view pattern, std::string_view flags);

  class Matcher {
  public:
    Matcher(const Program& program, std::size_t subjectBytes);
    bool search(const void* subject, std::size_t length, std::size_t start, bool wide, bool splitting);
    const std::size_t* captures() const { return offsets_.get() ? offsets_.get() : inlineOffsets_.data(); }
    std::size_t captureStart(std::size_t group) const { return captures()[2 * group]; }
    std::size_t captureEnd(std::size_t group) const { return captures()[2 * group + 1]; }

  private:
    static constexpr std::size_t inlineCount = 32;
    const Program& program_;
    Limits limits_;
    unsigned searches_ = 0;
    std::array<std::uint8_t*, inlineCount> inlineRegisters_{};
    std::array<std::size_t, inlineCount> inlineOffsets_;
    std::unique_ptr<std::uint8_t*[]> registers_;
    std::unique_ptr<std::size_t[]> offsets_;
  };
}
