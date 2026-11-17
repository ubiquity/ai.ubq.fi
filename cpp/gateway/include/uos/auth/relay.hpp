#pragma once
#include <optional>
#include <string>

namespace uos::auth::relay {
  /**
   * Parses the exact HTTPS origins allowed by the browser auth relay contract.
   * Keep this host allowlist aligned with static/auth-relay.js.
   *
   * `value: unknown`: every value that is not a string (null, undefined, a
   * number, ...) is passed as std::nullopt.
   */
  std::optional<std::string> parseTrustedAuthRelayOrigin(const std::optional<std::string>& value);
}
