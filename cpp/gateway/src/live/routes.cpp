#include "uos/live/routes.hpp"

#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

namespace uos::live::routes {
  std::optional<LiveRoute> liveRouteForRequest(std::string_view method, std::string_view path) {
    if (method == "POST" && path == "/v1/live") return LiveRouteCall{};
    if (method != "GET") return std::nullopt;
    static const rt::Regex SIDEBAND("^\\/v1\\/live\\/([^/]+)$");
    const std::optional<rt::RegexMatch> match = SIDEBAND.exec(path);
    if (!match) return std::nullopt;
    std::string callId;
    try {
      callId = rt::decodeURIComponent(*(*match)[1]);
    } catch (const rt::JsError&) {
      return std::nullopt;
    }
    if (callId.empty() || callId == "." || callId == "..") return std::nullopt;
    return LiveRouteSideband{std::move(callId)};
  }
}
