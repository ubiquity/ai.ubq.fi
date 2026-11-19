#include "uos/release.hpp"

// A packaged build compiles this translation unit with
// -DUOS_PACKAGE_GIT_COMMIT=<full lowercase commit>. Source-tree development
// and test builds retain the historical "unknown" identity. The portable
// packager and the macOS packager both bind the commit before compiling, so
// /health can report the exact release instead of "unknown".
#ifndef UOS_PACKAGE_GIT_COMMIT
#define UOS_PACKAGE_GIT_COMMIT "unknown"
#endif

namespace uos::release {
  const std::string_view RELEASE_GIT_SHA = UOS_PACKAGE_GIT_COMMIT;
}
