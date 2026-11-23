#pragma once

#include "uos/rt/env.hpp"

#include <string_view>

namespace uos::upstream_auth {
  inline constexpr std::string_view FOLLOWER_ENV = "UOS_GATEWAY_UPSTREAM_AUTH_FOLLOWER";
  inline constexpr std::string_view LEADER_STATE_ENV = "UOS_GATEWAY_UPSTREAM_AUTH_LEADER_STATE";
  inline constexpr std::string_view FOLLOWER_STALE = "upstream_auth_follower_stale";
  inline constexpr std::string_view FOLLOWER_READ_ONLY = "UOS_GATEWAY_UPSTREAM_AUTH_FOLLOWER=1 makes the Codex auth pool read-only. Import updated auth while the follower is stopped.";

  inline bool followerMode() { return rt::env::get(FOLLOWER_ENV) == "1"; }
}
