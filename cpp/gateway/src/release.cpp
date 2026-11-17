#include "uos/release.hpp"

namespace uos::release {
  // CI replaces this value inside the immutable deployment artifact.
  const std::string_view RELEASE_GIT_SHA = "unknown";
}
