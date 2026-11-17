#pragma once
// Port of src/live/sideband.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/runtime/http_upgrade.hpp"

#include <string>

namespace uos::live::sideband {
  rt::Task<runtime::http_upgrade::UpgradeResult> prepareLiveSidebandUpgrade(rt::Request req, std::string callId, std::string principal);
  rt::Task<rt::Response> handleLiveSideband(rt::Request req, std::string callId, std::string principal);
}
