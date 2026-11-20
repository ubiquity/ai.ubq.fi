// Host resource hooks required by the standalone libregexp build.
#include "regex_internal.hpp"

#include "libregexp.h"

#include <algorithm>
#include <pthread.h>

namespace uos::rt::regex_detail {
  namespace {
    std::uintptr_t nativeStackBase() {
#if defined(__linux__)
      pthread_attr_t attributes;
      if (pthread_getattr_np(pthread_self(), &attributes) != 0) return 0;
      void* base = nullptr;
      std::size_t size = 0;
      const int result = pthread_attr_getstack(&attributes, &base, &size);
      pthread_attr_destroy(&attributes);
      return result == 0 ? reinterpret_cast<std::uintptr_t>(base) : 0;
#elif defined(__APPLE__)
      return reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(pthread_self())) - pthread_get_stacksize_np(pthread_self());
#else
      return 0;
#endif
    }
  }

  std::uintptr_t compileStackFloor(std::uintptr_t entry) {
    constexpr std::size_t parserStack = 128 * 1024;
    constexpr std::size_t stackReserve = 32 * 1024;
    const thread_local std::uintptr_t base = nativeStackBase();
    return std::max(entry - parserStack, base + stackReserve);
  }
}

extern "C" bool lre_check_stack_overflow(void* opaque, std::size_t size) {
  auto& limits = *static_cast<uos::rt::regex_detail::Limits*>(opaque);
  const auto frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
  return frame <= limits.stackFloor || size >= frame - limits.stackFloor || limits.checkBackstop();
}

extern "C" int lre_check_timeout(void* opaque) {
  auto& limits = *static_cast<uos::rt::regex_detail::Limits*>(opaque);
  return limits.pollsRemaining == 0 || --limits.pollsRemaining == 0 || limits.checkBackstop();
}

extern "C" void* lre_realloc(void*, void* ptr, std::size_t size) {
  if (size == 0) {
    std::free(ptr);
    return nullptr;
  }
  constexpr std::size_t maxAllocation = 72 * 1024 * 1024;
  return size > maxAllocation ? nullptr : std::realloc(ptr, size);
}
