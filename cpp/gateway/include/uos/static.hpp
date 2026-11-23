#pragma once
// Port of src/static.ts.
//
// The TS reads its preloaded pages with top-level awaits when the module loads; here the
// first call loads them (construct on first use), relative to the process working directory
// (the release root, whose static/ directory sits next to src/). A missing preloaded file
// throws from that first call instead of failing the import.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"

#include <optional>
#include <string>

namespace uos::static_ {
  // std::nullopt is null.
  [[nodiscard]] rt::Task<std::optional<rt::Response>> handleStaticAsset(std::string path);

  bool hasStaticAsset(const std::string& path);

  [[nodiscard]] rt::Task<rt::Response> handleRoot(rt::Request req);
}
