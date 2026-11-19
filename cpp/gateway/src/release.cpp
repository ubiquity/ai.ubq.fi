#include "uos/release.hpp"

namespace uos::release {
#if defined(UOS_PACKAGE_GIT_COMMIT)
  const std::string_view RELEASE_GIT_SHA = UOS_PACKAGE_GIT_COMMIT;
#else
  const std::string_view RELEASE_GIT_SHA = "unknown";
#endif
}
