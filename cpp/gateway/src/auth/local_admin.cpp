#include "uos/auth/local_admin.hpp"

#include "uos/config.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <cstddef>
#include <utility>
#include <variant>

namespace uos::auth::local_admin {
  using runtime::http::Addr;
  using runtime::http::AddrUnix;
  using runtime::http::NetAddr;

  namespace {
    constexpr std::string_view DISABLE_ADMIN_AUTH_FLAG = "--disable-admin-auth";

    rt::JsError formatArgumentError(std::string_view argument) {
      return rt::JsError("[ai.ubq.fi] Unknown server argument '" + std::string(argument) + "'. Supported arguments: " + std::string(DISABLE_ADMIN_AUTH_FLAG));
    }
  }

  ServeRuntimeOptions parseServeRuntimeOptions(const std::vector<std::string>& args, const ParseServeRuntimeOptionsOptions& options) {
    bool disableAdminAuth = false;

    for (const std::string& argument : args) {
      if (argument != DISABLE_ADMIN_AUTH_FLAG) throw formatArgumentError(argument);
      if (disableAdminAuth) {
        throw rt::JsError("[ai.ubq.fi] Server argument '" + std::string(DISABLE_ADMIN_AUTH_FLAG) + "' may only be specified once.");
      }
      disableAdminAuth = true;
    }

    if (disableAdminAuth && options.isDeploy) {
      throw rt::JsError("[ai.ubq.fi] " + std::string(DISABLE_ADMIN_AUTH_FLAG) + " is unavailable in Deno Deploy.");
    }
    // The production VPS serves Caddy-proxied public traffic on its loopback listener (#262).
    if (disableAdminAuth && options.isProductionTimeline) {
      throw rt::JsError("[ai.ubq.fi] " + std::string(DISABLE_ADMIN_AUTH_FLAG) + " is unavailable on the production timeline (DENO_TIMELINE=production).");
    }

    return ServeRuntimeOptions{disableAdminAuth};
  }

  namespace {
    std::string normalizeHostname(std::string_view value) {
      std::string normalized = rt::toLowerCase(rt::trim(value));
      return rt::startsWith(normalized, "[") && rt::endsWith(normalized, "]") ? rt::slice(normalized, 1, -1) : normalized;
    }

    bool isIpv4Loopback(std::string_view hostname) {
      const std::vector<std::string> octets = rt::split(hostname, ".");
      if (octets.size() != 4) return false;
      std::vector<double> parsed;
      for (const std::string& octet : octets) parsed.push_back(rt::stringToNumber(octet));
      for (std::size_t index = 0; index < parsed.size(); ++index) {
        const double octet = parsed[index];
        if (!(rt::Number::isInteger(octet) && octet >= 0 && octet <= 255 && rt::numberToString(octet) == octets[index])) return false;
      }
      return parsed[0] == 127;
    }
  }

  bool isLoopbackHostname(std::string_view value) {
    const std::string hostname = normalizeHostname(value);
    return hostname == "localhost" || hostname == "::1" || hostname == "0:0:0:0:0:0:0:1" || isIpv4Loopback(hostname);
  }

  namespace {
    bool isNumericLoopbackHostname(std::string_view value) {
      const std::string hostname = normalizeHostname(value);
      return hostname == "::1" || hostname == "0:0:0:0:0:0:0:1" || isIpv4Loopback(hostname);
    }

    std::string formatListenerAddress(const Addr& address) {
      if (const NetAddr* net = std::get_if<NetAddr>(&address)) {
        if (net->transport == "tcp" || net->transport == "udp") {
          const std::string hostname = rt::includes(net->hostname, ":") ? "[" + net->hostname + "]" : net->hostname;
          return net->transport + "://" + hostname + ":" + rt::numberToString(net->port);
        }
        // `"path" in address` is false for a NetAddr.
        return net->transport;
      }
      return "unix://" + std::get<AddrUnix>(address).path;
    }
  }

  bool shouldDisableAdminAuthForListener(const ServeRuntimeOptions& options, const Addr& address) {
    if (!options.disableAdminAuth) return false;
    const NetAddr* net = std::get_if<NetAddr>(&address);
    if (net == nullptr || net->transport != "tcp" || !isNumericLoopbackHostname(net->hostname)) {
      throw rt::JsError("[ai.ubq.fi] " + std::string(DISABLE_ADMIN_AUTH_FLAG) + " requires a loopback TCP listener; got " + formatListenerAddress(address) + ".");
    }
    return true;
  }

  namespace {
    bool adminAuthDisabled = false;
    std::optional<Addr> adminAuthPeer;
    /** Set only by the Mac companion's listener; see `isMacCompanionServiceListener`. */
    bool macCompanionListener = false;
    /**
     * Request-scoped peers for a LAN-facing listener. Routing awaits before it
     * authenticates, so a single process-wide slot can be overwritten by a
     * concurrent request: a LAN request could then observe a loopback peer that
     * belongs to someone else's connection. A peer bound to the request object
     * itself cannot be moved to another request.
     */
    rt::WeakMap<rt::Request, Addr> adminAuthRequestPeers;
  }

  bool configureAdminAuthForListener(const ServeRuntimeOptions& options, const Addr& address) {
    const bool disabled = shouldDisableAdminAuthForListener(options, address);
    adminAuthDisabled = disabled;
    adminAuthPeer = std::nullopt;
    macCompanionListener = false;
    return disabled;
  }

  void configureAdminAuthPeerForRequest(std::optional<Addr> peer) { adminAuthPeer = std::move(peer); }

  void configureAdminAuthPeerForRequest(std::optional<Addr> peer, const rt::Request& request) {
    if (!peer) adminAuthRequestPeers.delete_(request);
    else adminAuthRequestPeers.set(request, std::move(*peer));
  }

  namespace {
    /**
     * The only non-loopback listener that may enable the loopback-peer-gated local
     * bypass: the Mac companion's LAN-facing wildcard TCP listener. This stays a
     * separate, narrowly named entry point so the generic `--disable-admin-auth`
     * path keeps rejecting a non-loopback listener for every other caller, and it
     * never weakens `isAdminAuthDisabledForRequest`, which still requires an actual
     * numeric loopback TCP peer plus a loopback request URL and same-origin checks.
     */
    constexpr std::string_view MAC_LAN_LISTENER_HOSTNAME = "0.0.0.0";
  }

  bool configureMacLocalAdminAuthBypassForListener(const Addr& address) {
    const NetAddr* net = std::get_if<NetAddr>(&address);
    if (net == nullptr || net->transport != "tcp" || net->hostname != MAC_LAN_LISTENER_HOSTNAME) {
      throw rt::JsError("[ai.ubq.fi] the Mac local admin bypass requires the " + std::string(MAC_LAN_LISTENER_HOSTNAME) + " TCP listener; got " +
                        formatListenerAddress(address) + ".");
    }
    adminAuthDisabled = true;
    adminAuthPeer = std::nullopt;
    macCompanionListener = true;
    return true;
  }

  bool isMacCompanionServiceListener() { return macCompanionListener; }

  namespace {
    bool isLoopbackPeer(const Addr& peer) {
      const NetAddr* net = std::get_if<NetAddr>(&peer);
      return net != nullptr && net->transport == "tcp" && isNumericLoopbackHostname(net->hostname);
    }
  }

  bool isAdminAuthDisabledForRequest(const rt::Request& request) {
    // The production timeline never grants a development bypass, however a
    // launcher enabled one: its loopback peers are the public proxy (#262).
    if (!adminAuthDisabled || config::config.isProductionTimeline) return false;
    // The bypass must be granted only to an actual loopback peer, never to a
    // forwarded, tunneled, or port-forwarded request whose URL hostname is
    // client-controlled. Fail closed when the peer is unknown (for example a
    // request constructed outside a Deno serve listener).
    const Addr* bound = adminAuthRequestPeers.find(request);
    const std::optional<Addr> peer = bound != nullptr ? std::optional<Addr>(*bound) : adminAuthPeer;
    if (!peer || !isLoopbackPeer(*peer)) return false;
    try {
      const rt::URL url(request.url());
      const std::optional<std::string> origin = request.headers().get("origin");
      if (origin && *origin != url.origin()) return false;
      const std::optional<std::string> fetchSite = request.headers().get("sec-fetch-site");
      if (fetchSite && *fetchSite != "same-origin" && *fetchSite != "none") return false;
      return isLoopbackHostname(url.hostname());
    } catch (...) {
      return false;
    }
  }
}
