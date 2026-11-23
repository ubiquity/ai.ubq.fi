#include "uos/auth/relay.hpp"

#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace uos::auth::relay {
  namespace {
    constexpr std::string_view DENO_PREVIEW_SUFFIX = "[a-z0-9]{12}";
    const std::unordered_map<std::string_view, std::vector<std::string_view>>& TRUSTED_DENO_APPLICATIONS() {
      static const std::unordered_map<std::string_view, std::vector<std::string_view>> applications{
        {"ubiquity-dao", {"ai-ubq-fi", "p-ai-ubq-fi"}},
        {"0x4007", {"telegram-daily-exporter"}},
        {"ubiquity-os", {"agent-worker"}},
      };
      return applications;
    }

    bool isTrustedDenoApplicationHost(const std::string& hostname) {
      const std::vector<std::string> labels = rt::split(hostname, ".");
      if (labels.size() != 4) return false;
      const std::string& app = labels[0];
      const std::string& organization = labels[1];
      const std::string& platform = labels[2];
      const std::string& topLevelDomain = labels[3];
      if (platform != "deno" || topLevelDomain != "net") return false;
      const auto trustedApps = TRUSTED_DENO_APPLICATIONS().find(organization);
      if (trustedApps == TRUSTED_DENO_APPLICATIONS().end() || app.empty()) return false;
      for (const std::string_view trustedApp : trustedApps->second) {
        if (app == trustedApp || rt::Regex("^" + std::string(trustedApp) + "-" + std::string(DENO_PREVIEW_SUFFIX) + "$").test(app)) return true;
      }
      return false;
    }

    bool isAiGatewayDeployHost(const std::string& hostname) {
      return hostname == "ai.ubq.fi" || hostname == "ai-ubq-fi.deno.dev" ||
             rt::Regex("^ai-ubq-fi-" + std::string(DENO_PREVIEW_SUFFIX) + "\\.deno\\.dev$").test(hostname) || isTrustedDenoApplicationHost(hostname);
    }

    /**
     * Removes the trailing run of "/" characters. Equivalent to
     * `value.replace(/\/+$/g, "")`, but linear: the regex form backtracked over the
     * whole trailing run at every offset, so a request such as
     * `?cors_origin=https://a.b////...////x` cost O(n^2) before being rejected.
     */
    std::string stripTrailingSlashes(std::string_view value) {
      std::size_t end = value.size();
      while (end > 0 && value[end - 1] == '/') end--;
      return std::string(value.substr(0, end));
    }
  }

  std::optional<std::string> parseTrustedAuthRelayOrigin(const std::optional<std::string>& value) {
    const std::string raw = value ? rt::trim(*value) : std::string();
    if (raw.empty()) return std::nullopt;
    try {
      const rt::URL url(raw);
      const std::string hostname = rt::toLowerCase(url.hostname());
      if (url.protocol() != "https:" || !url.port().empty() || url.origin() != stripTrailingSlashes(raw)) return std::nullopt;
      return isAiGatewayDeployHost(hostname) ? std::optional<std::string>(url.origin()) : std::nullopt;
    } catch (...) {
      return std::nullopt;
    }
  }
}
