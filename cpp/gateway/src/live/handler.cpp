// `/v1/live` dispatch: authenticate once, then route to call creation or the
// sideband bridge.
#include "uos/live/handler.hpp"

#include "uos/auth/index.hpp"
#include "uos/handler/http.hpp"
#include "uos/http.hpp"
#include "uos/live/calls.hpp"
#include "uos/live/routes.hpp"
#include "uos/live/sideband.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/url.hpp"

#include <variant>

namespace uos::live::handler {
  /**
   * Serves the Codex realtime voice relay; null when the request is not a
   * `/v1/live` route.
   *
   * JSON arms receive the standard `withCors`/`withRequestId` decorations here.
   * The separate transport upgrade entry point reads only headers and authenticates
   * before accepting a WebSocket. The
   * resolved principal is passed to both arms so the call is bound to the one
   * credential that created it and only that principal may join its sideband.
   */
  rt::Task<std::optional<rt::Response>> handleLiveRoute(rt::Request req, std::string requestId) {
    const auto route = routes::liveRouteForRequest(req.method(), ::uos::handler::http::normalizePath(rt::URL(req.url()).pathname()));
    if (!route) co_return std::nullopt;

    rt::Promise<auth::index::AuthenticateClientResult> authentication(auth::index::authenticateClient(req));
    const auto authResult = co_await authentication.wait();
    if (const auto* refusal = std::get_if<auth::index::AuthenticateClientResultNotOk>(&authResult)) {
      co_return ::uos::handler::http::withRequestId(http::withCors(refusal->response, req), requestId);
    }

    rt::Promise<std::string> identity(::uos::handler::http::resolveIdempotencyPrincipal(std::get<auth::index::AuthenticateClientResultOk>(authResult)));
    const auto principal = co_await identity.wait();
    rt::Promise<rt::Response> pending = std::holds_alternative<routes::LiveRouteCall>(*route)
      ? rt::Promise<rt::Response>(calls::handleLiveCallCreate(req, principal))
      : rt::Promise<rt::Response>(sideband::handleLiveSideband(req, std::get<routes::LiveRouteSideband>(*route).callId, principal));
    const auto response = co_await pending.wait();
    co_return ::uos::handler::http::withRequestId(http::withCors(response, req), requestId);
  }

  /** Authenticated upgrade entry point, invoked only by the real HTTP transport. */
  rt::Task<runtime::http_upgrade::UpgradeResult> handleLiveUpgrade(rt::Request req, runtime::http_upgrade::UpgradeInfo) {
    const auto requestId = rt::crypto::randomUUID();
    const auto decorate = [req, requestId](rt::Response response) {
      return ::uos::handler::http::withRequestId(http::withCors(response, req), requestId);
    };
    const auto route = routes::liveRouteForRequest(req.method(), ::uos::handler::http::normalizePath(rt::URL(req.url()).pathname()));
    if (!route || !std::holds_alternative<routes::LiveRouteSideband>(*route)) {
      co_return decorate(http::openaiError(404, "Unknown WebSocket endpoint.", "not_found"));
    }
    rt::Promise<auth::index::AuthenticateClientResult> authentication(auth::index::authenticateClient(req));
    const auto authResult = co_await authentication.wait();
    if (const auto* refusal = std::get_if<auth::index::AuthenticateClientResultNotOk>(&authResult)) co_return decorate(refusal->response);
    rt::Promise<std::string> identity(::uos::handler::http::resolveIdempotencyPrincipal(std::get<auth::index::AuthenticateClientResultOk>(authResult)));
    const auto principal = co_await identity.wait();
    rt::Promise<runtime::http_upgrade::UpgradeResult> preparation(sideband::prepareLiveSidebandUpgrade(req, std::get<routes::LiveRouteSideband>(*route).callId, principal));
    const auto prepared = co_await preparation.wait();
    if (const auto* response = std::get_if<rt::Response>(&prepared)) co_return decorate(*response);
    co_return prepared;
  }
}
