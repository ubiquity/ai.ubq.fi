#pragma once
#include <string_view>

namespace uos::release {
  // CI replaces this value inside the immutable deployment artifact.
  extern const std::string_view RELEASE_GIT_SHA;
}
