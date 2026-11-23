#pragma once
// src/auth/local-admin.ts: the development-only admin-auth bypass and its
// listener and peer bindings.
#include "uos/rt/http.hpp"
#include "uos/runtime/http.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::auth::local_admin {
  // export type ServeRuntimeOptions = Readonly<{ disableAdminAuth: boolean }>;
  struct ServeRuntimeOptions {
    bool disableAdminAuth;
    friend bool operator==(const ServeRuntimeOptions&, const ServeRuntimeOptions&) = default;
  };

  // Readonly<{ isDeploy: boolean; isProductionTimeline: boolean }>
  struct ParseServeRuntimeOptionsOptions {
    bool isDeploy;
    bool isProductionTimeline;
  };

  // Throws Error "[ai.ubq.fi] Unknown server argument '<arg>'. ...", "... may only be
  // specified once.", "... is unavailable in Deno Deploy." or "... is unavailable on the
  // production timeline (DENO_TIMELINE=production).". The result is frozen (a const value).
  ServeRuntimeOptions parseServeRuntimeOptions(const std::vector<std::string>& args, const ParseServeRuntimeOptionsOptions& options);

  bool isLoopbackHostname(std::string_view value);

  // Throws Error "[ai.ubq.fi] --disable-admin-auth requires a loopback TCP listener; got <address>."
  bool shouldDisableAdminAuthForListener(const ServeRuntimeOptions& options, const runtime::http::Addr& address);

  bool configureAdminAuthForListener(const ServeRuntimeOptions& options, const runtime::http::Addr& address);

  /**
   * Records the TCP peer of a request being handled by the server. The optional
   * `request` binds the peer to that exact request object (used by the LAN-facing
   * Mac listener); without it the peer is the process-wide binding the loopback
   * development server has always used.
   */
  // `peer: Addr | null` (std::nullopt is null).
  void configureAdminAuthPeerForRequest(std::optional<runtime::http::Addr> peer);
  void configureAdminAuthPeerForRequest(std::optional<runtime::http::Addr> peer, const rt::Request& request);

  // Throws Error "[ai.ubq.fi] the Mac local admin bypass requires the 0.0.0.0 TCP listener; got <address>."
  bool configureMacLocalAdminAuthBypassForListener(const runtime::http::Addr& address);

  /**
   * True only in the Mac companion service process: `scripts/serve-mac.ts` is the
   * one caller of `configureMacLocalAdminAuthBypassForListener`. A development
   * server (`bun run dev`, `dev:local`) configures its loopback bypass through
   * `configureAdminAuthForListener`, which clears this signal. Work that must run
   * only on the real Mac service, such as pushing its capacity samples to the VPS
   * chart, gates on this rather than on the request-level loopback bypass.
   */
  bool isMacCompanionServiceListener();

  bool isAdminAuthDisabledForRequest(const rt::Request& request);
}
