// `/v1/live` dispatch: authenticate once, then route to call creation or the
// sideband bridge.
#include "uos/live/handler.hpp"

#include "uos/auth/index.hpp"
#include "uos/handler/http.hpp"
#include "uos/handler/failure_journal.hpp"
#include "uos/http.hpp"
#include "uos/live/calls.hpp"
#include "uos/live/routes.hpp"
#include "uos/live/sideband.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/url.hpp"

#include <memory>
#include <utility>
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
  rt::Task<std::optional<rt::Response>> handleLiveRoute(rt::Request req, std::string requestId,
                                                        std::optional<rt::Promise<void>> deliveryCompleted) {
    const auto route = routes::liveRouteForRequest(req.method(), ::uos::handler::http::normalizePath(rt::URL(req.url()).pathname()));
    if (!route) co_return std::nullopt;
    const bool isCall = std::holds_alternative<routes::LiveRouteCall>(*route);
    const std::string_view label = isCall ? "live.call" : "live.sideband";
    const auto decorate = [&req, &requestId, label](rt::Response response, bool authenticated, std::string_view bodyEvidence) {
      auto cors = http::withCors(response, req);
      return cors.status() >= 400
        ? ::uos::handler::failure_journal::withApplicationFailure(req, std::move(cors), label, requestId, authenticated, bodyEvidence)
        : ::uos::handler::http::withRequestId(cors, requestId);
    };

    rt::Promise<auth::index::AuthenticateClientResult> authentication(auth::index::authenticateClient(req));
    const auto authResult = co_await authentication.wait();
    if (const auto* refusal = std::get_if<auth::index::AuthenticateClientResultNotOk>(&authResult)) {
      co_return decorate(refusal->response, false, req.body() ? "unauthenticated" : "no_body");
    }

    const auto& accepted = std::get<auth::index::AuthenticateClientResultOk>(authResult);
    const bool authenticated = !std::holds_alternative<auth::index::ClientAuthMethodDisabled>(accepted.method);
    rt::Promise<std::string> identity(::uos::handler::http::resolveIdempotencyPrincipal(accepted));
    const auto principal = co_await identity.wait();
    std::optional<calls::FailedLiveCallCapture> capture;
    std::shared_ptr<bool> projected;
    if (isCall && authenticated) {
      projected = std::make_shared<bool>(false);
      capture = calls::FailedLiveCallCapture{requestId, std::move(deliveryCompleted), projected};
    }
    rt::Promise<rt::Response> pending = isCall
      ? rt::Promise<rt::Response>(calls::handleLiveCallCreate(req, principal, std::move(capture)))
      : rt::Promise<rt::Response>(sideband::handleLiveSideband(req, std::get<routes::LiveRouteSideband>(*route).callId, principal));
    const auto response = co_await pending.wait();
    std::string_view bodyEvidence = "no_body";
    if (req.body()) {
      if (!authenticated) bodyEvidence = "unauthenticated";
      else if (!isCall) bodyEvidence = "live_control_body_excluded";
      else bodyEvidence = projected && *projected ? "redacted_live_session_projection" : "no_safe_live_session_projection";
    }
    co_return decorate(response, authenticated, bodyEvidence);
  }

  /** Authenticated upgrade entry point, invoked only by the real HTTP transport. */
  rt::Task<runtime::http_upgrade::UpgradeResult> handleLiveUpgrade(rt::Request req, runtime::http_upgrade::UpgradeInfo) {
    const auto requestId = rt::crypto::randomUUID();
    const auto decorate = [&req, &requestId](rt::Response response, bool authenticated) {
      auto cors = http::withCors(response, req);
      return cors.status() >= 400
        ? ::uos::handler::failure_journal::withApplicationFailure(req, std::move(cors), "live.upgrade", requestId, authenticated,
                                                               req.body() ? "live_control_body_excluded" : "no_body")
        : ::uos::handler::http::withRequestId(cors, requestId);
    };
    const auto route = routes::liveRouteForRequest(req.method(), ::uos::handler::http::normalizePath(rt::URL(req.url()).pathname()));
    if (!route || !std::holds_alternative<routes::LiveRouteSideband>(*route)) {
      co_return decorate(http::openaiError(404, "Unknown WebSocket endpoint.", "not_found"), false);
    }
    rt::Promise<auth::index::AuthenticateClientResult> authentication(auth::index::authenticateClient(req));
    const auto authResult = co_await authentication.wait();
    if (const auto* refusal = std::get_if<auth::index::AuthenticateClientResultNotOk>(&authResult)) co_return decorate(refusal->response, false);
    const auto& accepted = std::get<auth::index::AuthenticateClientResultOk>(authResult);
    const bool authenticated = !std::holds_alternative<auth::index::ClientAuthMethodDisabled>(accepted.method);
    rt::Promise<std::string> identity(::uos::handler::http::resolveIdempotencyPrincipal(accepted));
    const auto principal = co_await identity.wait();
    rt::Promise<runtime::http_upgrade::UpgradeResult> preparation(sideband::prepareLiveSidebandUpgrade(req, std::get<routes::LiveRouteSideband>(*route).callId, principal));
    const auto prepared = co_await preparation.wait();
    if (const auto* response = std::get_if<rt::Response>(&prepared)) co_return decorate(*response, authenticated);
    co_return prepared;
  }
}
