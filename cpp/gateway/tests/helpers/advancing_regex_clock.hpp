#pragma once

#include "src/rt/regex_internal.hpp"

#include <utility>

namespace uos::test {
  class AdvancingRegexClock {
    using Limits = rt::regex_detail::Limits;
    using Clock = Limits::Clock;

  public:
    explicit AdvancingRegexClock(Clock::duration step)
      : step_(step), previous_(std::exchange(active_, this)), previousClock_(std::exchange(Limits::clockForTest, read)) {}
    AdvancingRegexClock(const AdvancingRegexClock&) = delete;
    AdvancingRegexClock& operator=(const AdvancingRegexClock&) = delete;
    ~AdvancingRegexClock() {
      Limits::clockForTest = previousClock_;
      active_ = previous_;
    }

    Clock::duration elapsed() const { return step_ * reads_; }
    long reads() const { return reads_; }

  private:
    static Clock::time_point read() noexcept {
      const auto elapsed = active_->elapsed();
      ++active_->reads_;
      return Clock::time_point(elapsed);
    }

    inline static thread_local AdvancingRegexClock* active_ = nullptr;
    Clock::duration step_;
    long reads_ = 0;
    AdvancingRegexClock* previous_;
    decltype(Limits::clockForTest) previousClock_;
  };
}
