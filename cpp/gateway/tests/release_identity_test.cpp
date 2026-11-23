// Links: tests/release_identity_test.cpp src/config.cpp src/release.cpp
#include "uos/config.hpp"
#include "uos/release.hpp"
#include "uos/rt/test.hpp"
#include <string>

TEST("release identity is the bound package commit and never unknown when packaged") {
  const std::string reported(uos::release::RELEASE_GIT_SHA);
#ifdef UOS_PACKAGE_GIT_COMMIT
  const std::string bound(UOS_PACKAGE_GIT_COMMIT);
  EXPECT_EQ(bound.size(), std::size_t{40});
  for (const char character : bound) EXPECT((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'));
  EXPECT_EQ(reported, bound);
  EXPECT(reported != "unknown");
#else
  EXPECT_EQ(reported, "unknown");
#endif
  EXPECT_EQ(uos::config::runtimeGitSha(), reported);
}
