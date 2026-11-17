#pragma once
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"

#include <optional>
#include <string_view>

namespace uos::codex::quota {
  inline constexpr std::string_view METERED_CODEX_LIMIT_NAME = "Metered balance";

  // `ClientQuotaSnapshot` is module-private in the TS; it is the parameter type of
  // both exports. Callers pass any object with `used_percent` (a Metered snapshot).
  struct ClientQuotaSnapshot {
    std::optional<double> used_percent;
    friend bool operator==(const ClientQuotaSnapshot&, const ClientQuotaSnapshot&) = default;
  };

  rt::Headers buildCodexQuotaHeaders(const rt::Headers& input, const std::optional<ClientQuotaSnapshot>& snapshot);

  rt::Response withCodexQuotaHeaders(const rt::Response& response, const std::optional<ClientQuotaSnapshot>& snapshot);
}
