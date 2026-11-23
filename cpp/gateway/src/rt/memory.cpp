#include "uos/rt/memory.hpp"
#include "uos/rt/clock.hpp"

#include <boost/json/array.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UOS_MEMORY_ADDRESS_SANITIZED 1
#endif
#endif

namespace uos::rt::memory {
  namespace {
    struct Totals {
      std::atomic<std::size_t> entries{0};
      std::atomic<std::size_t> bytes{0};
    };

    constexpr std::size_t cacheCount = static_cast<std::size_t>(Cache::count);
    constinit std::array<Totals, cacheCount> totals{};
    constexpr std::array<std::string_view, cacheCount> names{
      "api_key_policy", "codex_auth_pool", "codex_routing", "codex_catalog",
      "codex_account_models", "runtime_config", "provider_selection", "subscription_hashes",
      "provider_health", "static_assets", "json_parser", "sqlite_pages", "sqlite_statements",
      "github_auth", "deploy_admin_auth", "passport_jwks", "passkey_crls",
      "kernel_attestation_ids", "kernel_public_keys", "codex_overage", "codex_supervisor_inventory", "openrouter_models",
      "openlux_models", "surplus_models", "lithos_models", "lithos_rate_windows",
      "claude_models", "claude_credentials", "deepseek_flash_aliases", "codex_learned_model_details",
      "sentinel_replay_owners", "runtime_timers", "runtime_pending_promises"
    };

    Totals& counter(Cache cache) noexcept { return totals[static_cast<std::size_t>(cache)]; }

    struct ProcessMemory {
      std::optional<std::uint64_t> resident;
      std::optional<std::uint64_t> anonymous;
    };

#if defined(__linux__)
    template <std::size_t Size>
    std::string_view readProc(const char* path, std::array<char, Size>& buffer) noexcept {
      const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
      if (fd < 0) return {};
      const auto size = ::read(fd, buffer.data(), buffer.size());
      ::close(fd);
      return size > 0 ? std::string_view(buffer.data(), static_cast<std::size_t>(size)) : std::string_view{};
    }

    std::optional<std::uint64_t> number(std::string_view& input) noexcept {
      const auto start = input.find_first_not_of(" \t");
      if (start == std::string_view::npos) return std::nullopt;
      input.remove_prefix(start);
      std::uint64_t value = 0;
      const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
      if (error != std::errc{}) return std::nullopt;
      input.remove_prefix(static_cast<std::size_t>(end - input.data()));
      return value;
    }

    std::optional<std::uint64_t> rollupBytes(std::string_view body, std::string_view label) noexcept {
      const auto position = body.find(label);
      if (position == std::string_view::npos) return std::nullopt;
      body.remove_prefix(position + label.size());
      const auto kib = number(body);
      return kib ? std::optional(*kib * 1024) : std::nullopt;
    }

    ProcessMemory readMemory(bool useSmaps) noexcept {
      std::array<char, 2048> buffer;
      if (useSmaps) {
        const auto body = readProc("/proc/self/smaps_rollup", buffer);
        const auto resident = rollupBytes(body, "\nRss:");
        const auto anonymous = rollupBytes(body, "\nAnonymous:");
        if (resident && anonymous) return {resident, anonymous};
      }
      auto body = readProc("/proc/self/statm", buffer);
      (void)number(body);  // Virtual size.
      const auto resident = number(body);
      const auto shared = number(body);  // File-backed and shmem resident pages.
      static const long pageBytes = ::sysconf(_SC_PAGESIZE);
      if (!resident || !shared || pageBytes <= 0) return {};
      const auto bytesPerPage = static_cast<std::uint64_t>(pageBytes);
      return {*resident * bytesPerPage, (*resident - std::min(*resident, *shared)) * bytesPerPage};
    }
#else
    ProcessMemory readMemory(bool) noexcept { return {}; }
#endif

    boost::json::value valueOrNull(std::optional<std::uint64_t> value) {
      return value ? boost::json::value(*value) : boost::json::value(nullptr);
    }

#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__) && !defined(UOS_MEMORY_ADDRESS_SANITIZED)
    struct HeapSample {
      std::optional<std::uint64_t> anonymous;
      std::size_t inUse;
      std::size_t free;
      bool operator==(const HeapSample&) const = default;
    };

    HeapSample sampleHeap() noexcept {
      const auto allocator = ::mallinfo2();
      return {readMemory(false).anonymous, allocator.uordblks + allocator.hblkhd, allocator.fordblks};
    }
#endif
  }

  void setCache(Cache cache, std::size_t entries, std::size_t bytes) noexcept {
    auto& total = counter(cache);
    total.entries.store(entries, std::memory_order_relaxed);
    total.bytes.store(bytes, std::memory_order_relaxed);
  }

  ScopedCache::ScopedCache(ScopedCache&& other) noexcept
    : cache_(other.cache_), entries_(std::exchange(other.entries_, 0)), bytes_(std::exchange(other.bytes_, 0)) {}

  void ScopedCache::set(std::size_t entries, std::size_t bytes) noexcept {
    auto& total = counter(cache_);
    total.entries.fetch_add(entries - std::exchange(entries_, entries), std::memory_order_relaxed);
    total.bytes.fetch_add(bytes - std::exchange(bytes_, bytes), std::memory_order_relaxed);
  }

  std::size_t bytes(const std::string& value) noexcept { return value.capacity() + 1; }

  std::size_t bytes(const std::vector<std::string>& strings) noexcept {
    std::size_t size = strings.capacity() * sizeof(std::string);
    for (const auto& string : strings) size += bytes(string);
    return size;
  }

  std::size_t bytes(const boost::json::value& value) noexcept {
    if (const auto* string = value.if_string()) return string->capacity() + 1;
    if (const auto* object = value.if_object()) return bytes(*object);
    if (const auto* array = value.if_array()) return bytes(*array);
    return 0;
  }

  std::size_t bytes(const boost::json::object& object) noexcept {
    std::size_t size = object.capacity() * sizeof(boost::json::key_value_pair);
    for (const auto& member : object) size += member.key().size() + 1 + bytes(member.value());
    return size;
  }

  std::size_t bytes(const boost::json::array& array) noexcept {
    std::size_t size = array.capacity() * sizeof(boost::json::value);
    for (const auto& member : array) size += bytes(member);
    return size;
  }

  boost::json::object snapshot(bool useSmaps, boost::json::storage_ptr storage) {
    const auto memory = readMemory(useSmaps);
    boost::json::object result(storage);
    result.emplace("resident_bytes", valueOrNull(memory.resident));
    result.emplace("anonymous_bytes", valueOrNull(memory.anonymous));
#if defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
    const auto allocator = ::mallinfo2();
    result.emplace("allocator_in_use_bytes", allocator.uordblks + allocator.hblkhd);
    result.emplace("allocator_free_bytes", allocator.fordblks);
    result.emplace("allocator_mmap_bytes", allocator.hblkhd);
#else
    result.emplace("allocator_in_use_bytes", nullptr);
    result.emplace("allocator_free_bytes", nullptr);
    result.emplace("allocator_mmap_bytes", nullptr);
#endif
#else
    result.emplace("allocator_in_use_bytes", nullptr);
    result.emplace("allocator_free_bytes", nullptr);
    result.emplace("allocator_mmap_bytes", nullptr);
#endif
    boost::json::object caches(storage);
    caches.reserve(cacheCount);
    for (std::size_t index = 0; index < cacheCount; ++index) {
      boost::json::object entry(storage);
      entry.emplace("entries", totals[index].entries.load(std::memory_order_relaxed));
      entry.emplace("bytes", totals[index].bytes.load(std::memory_order_relaxed));
      caches.emplace(names[index], std::move(entry));
    }
    result.emplace("caches", std::move(caches));
    return result;
  }

  Timeout startHeapReclamation() {
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__) && !defined(UOS_MEMORY_ADDRESS_SANITIZED)
    constexpr std::size_t minimumFree = 8 * 1024 * 1024;
    constexpr std::size_t residentHeadroom = 5 * 1024 * 1024;
    constexpr std::size_t urgentHeadroom = 24 * 1024 * 1024;
    // Keep the kernel from rebuilding huge pages across discarded heap holes.
    (void)::prctl(PR_SET_THP_DISABLE, 1L, 0L, 0L, 0L);
    // Glibc can discard pages inside free blocks while keeping the blocks in
    // the arena. Check RSS as well so those blocks do not trigger repeated trims.
    auto timer = setInterval([previous = std::optional<HeapSample>{}, lastTrim = std::optional<HeapSample>{}]() mutable {
      const auto current = sampleHeap();
      const bool stable = current == std::exchange(previous, current);
      const bool urgent = current.anonymous && *current.anonymous > current.inUse + urgentHeadroom;
      // Give transient buffers time to finish turning over during ordinary load.
      if ((!stable && !urgent) || current.free < minimumFree || current == lastTrim ||
          (current.anonymous && *current.anonymous <= current.inUse + residentHeadroom)) return;
      (void)::malloc_trim(0);
      lastTrim = sampleHeap();
    }, 1000);
    timer.unref();
    return timer;
#else
    return {};
#endif
  }
}
