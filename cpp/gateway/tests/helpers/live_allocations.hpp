#pragma once

#include "uos/rt/test_async.hpp"

#include <cstddef>
#include <string>
#include <utility>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UOS_TEST_ADDRESS_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(UOS_TEST_ADDRESS_SANITIZER)
#include <sanitizer/allocator_interface.h>
#endif

namespace uos::tests::helpers {
  inline std::size_t liveAllocatedBytes() {
#if defined(__SANITIZE_ADDRESS__) || defined(UOS_TEST_ADDRESS_SANITIZER)
    return __sanitizer_get_current_allocated_bytes();
#else
    return 0;
#endif
  }

  // Pending Promise reactions can be released by Loop teardown. Sample with the
  // same loop still alive so their retained outcomes count even when LSan is clean.
  template <class Trial>
  rt::Task<void> expectStableLiveAllocations(Trial trial, std::size_t count, double timerDelay = 0) {
    for (int warmup = 0; warmup < 8; ++warmup) {
      auto task = trial();
      co_await std::move(task);
    }
    if (timerDelay > 0) co_await rt::sleep(timerDelay);
    co_await rt::microtask();
    const auto before = liveAllocatedBytes();
    for (std::size_t sample = 0; sample < count; ++sample) {
      auto task = trial();
      co_await std::move(task);
    }
    if (timerDelay > 0) co_await rt::sleep(timerDelay);
    co_await rt::microtask();
    const auto after = liveAllocatedBytes();
    constexpr std::size_t bookkeepingBytes = 4'096;
    if (after > before + bookkeepingBytes) {
      rt::test::report(__FILE__, __LINE__, "live allocations grew by " + std::to_string(after - before) + " bytes", false);
    }
  }
}
