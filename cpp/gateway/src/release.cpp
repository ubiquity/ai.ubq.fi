#include "uos/release.hpp"

#ifndef UOS_GATEWAY_RELEASE_COMMIT
#define UOS_GATEWAY_RELEASE_COMMIT "unknown"
#endif

namespace uos::release {
  const std::string_view RELEASE_GIT_SHA = UOS_GATEWAY_RELEASE_COMMIT;
}
