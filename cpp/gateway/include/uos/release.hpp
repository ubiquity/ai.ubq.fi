#pragma once
#include <string_view>

namespace uos::release {
  // Release packagers bind this value when compiling src/release.cpp.
  extern const std::string_view RELEASE_GIT_SHA;
}
