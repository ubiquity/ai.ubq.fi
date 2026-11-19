#pragma once

#include "uos/rt/limits.hpp"

#include <cstddef>
#include <string_view>

namespace uos::rt::detail {
  inline constexpr std::string_view decompressionLimitCode = "ERR_DECOMPRESSION_LIMIT";

  class DecompressionBudget {
  public:
    explicit DecompressionBudget(std::size_t byteLimit = limits::kHttpBodyBytes) : byteLimit_(byteLimit) {}

    bool feed(std::size_t input) {
      if (input > byteLimit_ - fed_) return false;
      fed_ += input;
      return true;
    }

    std::size_t remainingOutput() const { return byteLimit_ - produced_; }

    bool advance(std::size_t input, std::size_t output) {
      if (output > remainingOutput()) return false;
      consumed_ += input;
      const std::size_t total = produced_ + output;
      // ceil((total - slack) / ratio), expressed without multiplying the input.
      constexpr auto slack = limits::kDecompressionRatioSlackBytes;
      if (total > slack && (total - slack - 1) / limits::kDecompressionRatio >= consumed_) return false;
      produced_ = total;
      return true;
    }

  private:
    std::size_t byteLimit_;
    std::size_t fed_ = 0;
    std::size_t consumed_ = 0;
    std::size_t produced_ = 0;
  };
}
