#pragma once
// Port of src/live/handler.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/runtime/http_upgrade.hpp"

#include <optional>
#include <string>

namespace uos::live::handler {
  rt::Task<std::optional<rt::Response>> handleLiveRoute(rt::Request req, std::string requestId);
  rt::Task<runtime::http_upgrade::UpgradeResult> handleLiveUpgrade(rt::Request req, runtime::http_upgrade::UpgradeInfo info);
}
