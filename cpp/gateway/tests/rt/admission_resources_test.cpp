#include "uos/rt/admission_resources.hpp"
#include "uos/rt/test.hpp"

#include <fcntl.h>
#include <unistd.h>

using namespace uos::rt::admission_resources;

#if defined(__linux__)
TEST("host headroom requires complete MemTotal and MemAvailable in KiB and reserves 20 percent") {
  const auto headroom = detail::linuxHostHeadroom("MemTotal:    1000 kB\nMemFree: 20 kB\nMemAvailable:  600 kB\n");
  EXPECT_EQ(headroom, std::optional<std::uint64_t>(400 * 1024));
  EXPECT_EQ(detail::linuxHostHeadroom("MemTotal: 1000 kB\nMemAvailable: 99 kB\n"), std::optional<std::uint64_t>(0));
  EXPECT(!detail::linuxHostHeadroom("MemTotal: 1000 kB\nMemFree: 100 kB\n"));
  EXPECT(!detail::linuxHostHeadroom("MemTotal: 99999999999999999999 kB\nMemAvailable: 99 kB\n"));
  EXPECT(!detail::linuxHostHeadroom("MemTotal: 1000 kB\nMemAvailable: 200 kB\nMemAvailable: 800 kB\n"));
  EXPECT(!detail::linuxHostHeadroom("MemTotal: 1000 kB\nMemAvailable: 200 MB\n"));
}

TEST("finite cgroup headroom subtracts usage and reserves 20 percent with saturation") {
  EXPECT_EQ(detail::linuxLimitedHeadroom("1000\n", "700\n"), std::optional<std::uint64_t>(100));
  EXPECT_EQ(detail::linuxLimitedHeadroom("501\n", "390\n"), std::optional<std::uint64_t>(10));
  EXPECT_EQ(detail::linuxLimitedHeadroom("1000\n", "1200\n"), std::optional<std::uint64_t>(0));
  EXPECT(!detail::linuxLimitedHeadroom("max\n", "700\n"));
  EXPECT(!detail::linuxLimitedHeadroom("1000\n", "700garbage\n"));
}
#endif

TEST("descriptor headroom tracks opened and closed process descriptors") {
  const auto before = readHeadroom().descriptors;
  ASSERT(before && *before > 0);
  const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT(fd >= 0);
  const auto during = readHeadroom().descriptors;
  EXPECT_EQ(::close(fd), 0);
  const auto after = readHeadroom().descriptors;
  EXPECT_EQ(during, std::optional<std::uint64_t>(*before - 1));
  EXPECT_EQ(after, before);
}
