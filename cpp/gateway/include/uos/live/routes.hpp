#pragma once
// Pure route matcher for the Codex realtime voice relay (`/v1/live`).
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::live::routes {
  /** `call` is the multipart SDP call creation; `sideband` is the call's socket join. */
  struct LiveRouteCall {
    static constexpr std::string_view kind = "call";
    friend bool operator==(const LiveRouteCall&, const LiveRouteCall&) = default;
  };
  struct LiveRouteSideband {
    static constexpr std::string_view kind = "sideband";
    std::string callId;
    friend bool operator==(const LiveRouteSideband&, const LiveRouteSideband&) = default;
  };
  using LiveRoute = std::variant<LiveRouteCall, LiveRouteSideband>;

  /**
   * `POST /v1/live` creates one realtime call and `GET /v1/live/<call_id>` joins
   * its sideband. Callers pass the normalized path (`normalizePath` strips
   * trailing slashes) and the call id must be exactly one path segment, because
   * the Codex client appends the id from the call-creation `Location` as a single
   * percent-encoded segment and itself rejects `.` and `..`.
   */
  std::optional<LiveRoute> liveRouteForRequest(std::string_view method, std::string_view path);
}
