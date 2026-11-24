#include "uos/live/calls.hpp"

#include "uos/bounded_response_body.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/codex/responses_operations.hpp"
#include "uos/codex/routing_mutations.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/http.hpp"
#include "uos/live/upstream.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/types.hpp"
#include "uos/utils.hpp"

#include <exception>
#include <optional>
#include <utility>
#include <variant>

namespace uos::live::calls {
  namespace {
    constexpr int LIVE_CALL_MAX_UPSTREAM_ATTEMPTS = 2;
    constexpr double LIVE_CALL_BODY_TIMEOUT_MS = 5'000;
    struct LiveCallCreateParts { std::string sdp; json::object session; };
    using ParsedLiveCallCreate = std::variant<LiveCallCreateParts, rt::Response>;
    rt::Response liveCallCreateError(double status, const std::string& message, const std::string& code, const std::optional<std::string>& param = std::nullopt) {
      http::OpenaiErrorOptions options; if (param) options.param = param; return http::openaiError(status, message, code, options);
    }
    rt::Task<std::optional<rt::Response>> declaredLiveCallBodyTooLarge(rt::Request req) {
      const auto declaredLength = req.headers().get("content-length"); if (!declaredLength) co_return std::nullopt;
      const auto normalized = rt::trim(*declaredLength); static const rt::Regex LENGTH(R"(^(?:0|[1-9]\d*)$)", "u");
      if (!LENGTH.test(normalized)) co_return http::openaiError(400, "Live call creation Content-Length is invalid.", "invalid_request_error");
      const auto parsedLength = rt::stringToNumber(normalized);
      if (rt::Number::isSafeInteger(parsedLength) && parsedLength <= upstream::LIVE_CALL_MAX_BODY_BYTES) co_return std::nullopt;
      if (const auto body = req.body()) { try { auto cancellation = body->cancelPromise(); co_await cancellation.wait(); } catch (...) {} }
      co_return http::openaiError(413, "Live call creation bodies must be no larger than 1048576 bytes.", "invalid_request_error");
    }
    using ReadLiveCallBodyResult = std::variant<rt::Bytes, rt::Response>;
    rt::Task<ReadLiveCallBodyResult> readLiveCallBody(rt::Request req) {
      rt::Promise<std::optional<rt::Response>> declared(declaredLiveCallBodyTooLarge(req)); const auto rejected = co_await declared.wait(); if (rejected) co_return *rejected;
      bounded_response_body::ReadBoundedResponseBodyOptions options;
      options.maxBytes = upstream::LIVE_CALL_MAX_BODY_BYTES + 1; options.timeoutMs = LIVE_CALL_BODY_TIMEOUT_MS;
      options.signal = req.signal(); options.cancellationReason = "Live call creation body exceeded its read limit";
      rt::Response source(rt::bodyInit(req.body()));
      auto task = bounded_response_body::readBoundedResponseBody(source, options); rt::Promise<bounded_response_body::BoundedResponseBody> pending(std::move(task));
      const auto bounded = co_await pending.wait();
      if (static_cast<double>(bounded.bytes.size()) > upstream::LIVE_CALL_MAX_BODY_BYTES)
        co_return http::openaiError(413, "Live call creation bodies must be no larger than 1048576 bytes.", "invalid_request_error");
      if (!bounded.complete) co_return http::openaiError(400, "Live call creation body could not be read completely.", "invalid_request_error");
      co_return bounded.bytes;
    }
    std::optional<std::string> liveCallPartText(const rt::FormValue* value) {
      if (!value) return std::nullopt;
      if (const auto* text = std::get_if<std::string>(value)) return *text;
      return rt::decodeUtf8(std::get<rt::FormFile>(*value).bytes);
    }
    rt::Task<ParsedLiveCallCreate> parseLiveCallCreate(rt::Request req) {
      const auto contentType = req.headers().get("content-type").value_or(""); const auto mediaType = rt::toLowerCase(rt::trim(rt::split(contentType, ";").front()));
      static const rt::Regex BOUNDARY("boundary=", "iu");
      if (mediaType != "multipart/form-data" || !BOUNDARY.test(contentType))
        co_return liveCallCreateError(400, "Live call creation requires multipart/form-data with a declared boundary.", "invalid_request_error");
      rt::Promise<ReadLiveCallBodyResult> pending(readLiveCallBody(req)); auto body = co_await pending.wait();
      if (const auto* response = std::get_if<rt::Response>(&body)) co_return *response;
      rt::FormParts form;
      try {
        const rt::InboundFormDecoder decoder(contentType);
        form = decoder.decode(std::get<rt::Bytes>(body));
      } catch (...) {
        co_return liveCallCreateError(400, "Live call creation multipart body could not be parsed.", "invalid_request_error");
      }
      const auto sdp = liveCallPartText(rt::findFormPart(form, "sdp"));
      if (!sdp || sdp->empty()) co_return liveCallCreateError(400, "Live call creation requires a non-empty `sdp` part.", "invalid_request_error", "sdp");
      const auto sessionText = liveCallPartText(rt::findFormPart(form, "session"));
      if (!sessionText) co_return liveCallCreateError(400, "Live call creation requires a `session` part.", "invalid_request_error", "session");
      std::optional<json::value> session;
      try { session = rt::jsonParse(*sessionText); } catch (...) {}
      if (!session) co_return liveCallCreateError(400, "Live call creation `session` part must be valid JSON.", "invalid_request_error", "session");
      if (!session->is_object()) co_return liveCallCreateError(400, "Live call creation `session` part must be a JSON object.", "invalid_request_error", "session");
      LiveCallCreateParts parts{*sdp, session->get_object()}; co_return parts;
    }
    rt::Response liveRoutingFailureResponse(const codex::routing_state::RouteSelection& selection) {
      if (auto shared = codex::dispatch::initialCodexSelectionResponse(selection)) return *shared;
      if (const auto* blocked = std::get_if<codex::routing_state::RouteSelectionQuotaBlocked>(&selection))
        return codex::dispatch::routingErrorResponse(429, "Codex subscription capacity is exhausted for realtime voice; retry after the reported reset.", std::string(codex::auth::CODEX_QUOTA_BLOCKED_ERROR_CODE), blocked->retryAtMs);
      return codex::dispatch::routingErrorResponse(503, "Codex routing state is temporarily unavailable; retry the request.", "codex_auth_missing");
    }
    using QuotaBlocked = codex::routing_state::RouteSelectionQuotaBlocked;
    using LiveCallAccountSelection = std::variant<codex::routing_state::RoutingAccount, QuotaBlocked, rt::Response>;
    rt::Task<LiveCallAccountSelection> selectLiveCallAccount(rt::OrderedSet<std::string> attemptedAccountIds, std::optional<std::string> model) {
      std::optional<::uos::types::CodexAuthPoolState> pool; rt::Reason failure;
      try {
        rt::Promise<codex::auth::CodexAuthPoolEntry> pending(codex::auth::getAuthPoolEntry(true, true));
        auto entry = co_await pending.wait();
        if (codex::auth::shouldCheckLeaderAuth(entry.pool)) {
          const bool adopted = co_await codex::auth::checkLeaderAuth();
          if (adopted) {
            auto rereading = codex::auth::getAuthPoolEntry(true, true);
            entry = co_await std::move(rereading);
          }
        }
        pool = entry.pool;
        co_await codex::overage_settings::loadOverageUsageSettings(*pool);
      }
      catch (...) { failure = std::current_exception(); }
      if (failure) {
        if (const auto* error = codex::auth::codexErrorOf(failure)) co_return codex::dispatch::routingErrorResponse(503, error->message, error->code);
        co_return codex::dispatch::routingErrorResponse(503, "Codex routing state is temporarily unavailable; retry the request.", "codex_auth_missing");
      }
      auto task = codex::account_routing::selectCodexRoutingAccountsStrong(*pool, pool->accounts, rt::dateNow(), model);
      rt::Promise<codex::routing_state::RouteSelection> pending(std::move(task)); auto selection = co_await pending.wait();
      if (codex::auth::leaderAuthHasFencedCredential()) {
        const bool adopted = co_await codex::auth::checkLeaderAuth();
        if (adopted) {
          auto reading = codex::auth::getAuthPoolEntry(true, true);
          const auto current = co_await std::move(reading);
          auto reselecting = codex::account_routing::selectCodexRoutingAccountsStrong(current.pool, current.pool.accounts, rt::dateNow(), model);
          selection = co_await std::move(reselecting);
        }
      }
      if (const auto* eligible = std::get_if<codex::routing_state::RouteSelectionEligible>(&selection)) {
        for (const auto& candidate : eligible->accounts) if (!attemptedAccountIds.has(candidate.auth.account_id)) co_return candidate;
        co_return codex::dispatch::routingErrorResponse(503, "Codex routing state exposed no account for this realtime call; retry the request.", "codex_auth_missing");
      }
      if (const auto* blocked = std::get_if<QuotaBlocked>(&selection)) co_return *blocked;
      co_return liveRoutingFailureResponse(selection);
    }
    struct LiveCallAttempt { std::string accountId; codex::routing_state::RoutingAccount routing; ::uos::types::CodexAuthState auth; };
    using LiveCallAttemptResult = std::variant<LiveCallAttempt, QuotaBlocked, rt::Response>;
    rt::Task<LiveCallAttemptResult> resolveLiveCallAttempt(rt::OrderedSet<std::string> attemptedAccountIds, std::optional<std::string> model, rt::AbortSignal signal) {
      auto task = selectLiveCallAccount(attemptedAccountIds, model); rt::Promise<LiveCallAccountSelection> pending(std::move(task)); auto selection = co_await pending.wait();
      if (auto* routing = std::get_if<codex::routing_state::RoutingAccount>(&selection); routing && routing->quotaUsageProbeRequired) {
        const auto pool = co_await codex::auth::getAuthPoolEntry(true, true);
        const auto claimed = co_await codex::routing_mutations::claimCodexRoutingProbe(pool.pool, *routing);
        if (claimed) {
          try {
            if (co_await codex::capacity_routing::refreshCodexQuotaUsage(*claimed, signal) == codex::capacity_routing::QuotaUsageProbeResult::refreshed)
              selection = co_await selectLiveCallAccount(attemptedAccountIds, model);
          } catch (...) {
          }
          co_await codex::capacity_routing::releaseCodexQuotaUsageProbe(*claimed);
        }
      }
      if (const auto* response = std::get_if<rt::Response>(&selection)) co_return *response;
      if (const auto* blocked = std::get_if<QuotaBlocked>(&selection)) co_return *blocked;
      const auto account = std::get<codex::routing_state::RoutingAccount>(selection); const auto accountId = account.auth.account_id;
      if (account.quotaUsageProbeRequired)
        co_return codex::dispatch::routingErrorResponse(429, "Codex capacity is temporarily unavailable; retry the request.", "codex_capacity_unavailable");
      rt::Reason failure;
      try {
        rt::Promise<codex::auth::CodexAuthAccountEntry> currentPending(codex::auth_refresh::getCurrentAccountEntry(accountId, true)); auto current = co_await currentPending.wait();
        auto authTask = codex::auth_refresh::getValidAuth(current); rt::Promise<::uos::types::CodexAuthState> authPending(std::move(authTask)); auto auth = co_await authPending.wait();
        auto routing = account;
        if (auth.access_token != account.auth.access_token || auth.refresh_token != account.auth.refresh_token) {
          auto reconcile = codex::routing_mutations::reconcileCodexRoutingAccount(account, auth); rt::Promise<codex::routing_state::RoutingAccount> reconciled(std::move(reconcile)); routing = co_await reconciled.wait();
        }
        LiveCallAttempt result{accountId, routing, auth}; co_return result;
      } catch (...) { failure = std::current_exception(); }
      if (const auto* error = codex::auth::codexErrorOf(failure))
        co_return codex::dispatch::routingErrorResponse(error->status == 401 || error->status == 404 || error->status == 429 ? error->status : 503, error->message, error->code);
      co_return codex::dispatch::routingErrorResponse(503, "Codex auth pool is temporarily unavailable; retry the request.", "codex_auth_missing");
    }
    rt::Response liveCallDispatchFailureResponse(const rt::Reason& failure) {
      if (const auto* error = codex::auth::codexErrorOf(failure)) return http::openaiError(error->status >= 400 && error->status <= 599 ? error->status : 502, error->message, error->code);
      return http::openaiError(502, "Codex upstream request failed: upstream unreachable.", "codex_upstream_unreachable");
    }
    rt::Task<rt::Response> captureLiveCallAttempt(rt::Response response, std::optional<rt::AbortSignal> signal) {
      bounded_response_body::ReadBoundedResponseBodyOptions options;
      options.maxBytes = upstream::LIVE_CALL_CAPTURED_ERROR_MAX_BYTES; options.timeoutMs = 1000; options.signal = signal;
      options.cancellationReason = "Live call retry replaced this upstream response";
      auto task = bounded_response_body::readBoundedResponseBody(response, options); rt::Promise<bounded_response_body::BoundedResponseBody> pending(std::move(task));
      const auto captured = co_await pending.wait(); rt::Headers headers;
      if (const auto type = response.headers().get("content-type")) headers.set("content-type", *type);
      co_return rt::Response(captured.bytes, rt::ResponseInit{.status = response.status(), .statusText = response.statusText(), .headers = headers});
    }
    rt::Task<rt::Response> relayLiveCallResponse(rt::Response response, std::string accountId, std::string principal) {
      rt::Headers headers; if (const auto type = response.headers().get("content-type")) headers.set("content-type", *type);
      const auto location = response.headers().get("location"); const auto callId = location ? upstream::parseLiveCallIdFromLocation(*location) : std::nullopt;
      if (location) headers.set("location", callId ? upstream::liveCallLocation(*callId) : *location);
      if (callId && response.ok()) { rt::Promise<void> saved(upstream::writeLiveCallAccount(*callId, accountId, principal)); co_await saved.wait(); }
      co_return rt::Response(rt::bodyInit(response.body()), rt::ResponseInit{.status = response.status(), .statusText = response.statusText(), .headers = headers});
    }
  }
  rt::Task<rt::Response> handleLiveCallCreate(rt::Request req, std::string principal) {
    rt::Promise<ParsedLiveCallCreate> parsedPending(parseLiveCallCreate(req)); const auto parsed = co_await parsedPending.wait();
    if (const auto* response = std::get_if<rt::Response>(&parsed)) co_return *response;
    const auto& parts = std::get<LiveCallCreateParts>(parsed); const auto serializedBody = rt::jsonStringify(json::object{{"sdp", parts.sdp}, {"session", parts.session}});
    const auto headers = upstream::liveCallsUpstreamHeaders(req); const auto model = utils::getString(rt::get(parts.session, "model"));
    rt::OrderedSet<std::string> attemptedAccountIds; std::optional<rt::Response> captured;
    bool recoveryAttempted = false;
    for (int attempt = 1; attempt <= LIVE_CALL_MAX_UPSTREAM_ATTEMPTS; ++attempt) {
      auto task = resolveLiveCallAttempt(attemptedAccountIds, model, req.signal()); rt::Promise<LiveCallAttemptResult> resolvedPending(std::move(task)); auto resolved = co_await resolvedPending.wait();
      if (const auto* response = std::get_if<rt::Response>(&resolved)) co_return captured.value_or(*response);
      std::optional<LiveCallAttempt> dispatched;
      std::optional<rt::Response> upstreamResponse; rt::Reason failure;
      try {
        if (const auto* blocked = std::get_if<QuotaBlocked>(&resolved)) {
          if (std::exchange(recoveryAttempted, true)) co_return captured.value_or(liveRoutingFailureResponse(*blocked));
          auto recovery = codex::responses_operations::recoverBlockedSubscription({upstream::liveCallsUrl(), serializedBody, headers, model, req.signal(), attemptedAccountIds});
          const auto recovered = co_await std::move(recovery);
          if (!recovered) co_return captured.value_or(liveRoutingFailureResponse(*blocked));
          dispatched = LiveCallAttempt{recovered->routing.auth.account_id, recovered->routing, recovered->routing.auth};
          upstreamResponse = recovered->response;
        } else {
          dispatched = std::get<LiveCallAttempt>(resolved);
          upstreamResponse = co_await codex::dispatch::fetchCodexResponseWithAuth(dispatched->auth, upstream::liveCallsUrl(), serializedBody, headers, req.signal());
        }
      } catch (...) { failure = std::current_exception(); }
      if (failure) {
        if (dispatched) co_await codex::dispatch::recordCodexThrownHealth(dispatched->accountId, failure);
        co_return captured.value_or(liveCallDispatchFailureResponse(failure));
      }
      const auto& account = *dispatched;
      attemptedAccountIds.add(account.accountId);
      const auto response = *upstreamResponse; const bool credentialFailure = response.status() == 401 || response.status() == 403;
      if (credentialFailure) {
        auto mark = codex::routing_mutations::markCodexCredentialInvalid(account.routing); rt::Promise<void> marked(std::move(mark)); co_await marked.wait();
        if (codex::auth::hasLeaderAuthReader()) {
          try {
            auto reading = codex::auth::useLeaderCredential(account.auth, true);
            co_await std::move(reading);
            attemptedAccountIds.delete_(account.accountId);
          } catch (...) {}
        }
      }
      auto record = codex::dispatch::recordCodexResponseHealth(account.accountId, response, account.auth, std::string("success")); rt::Promise<void> recorded(std::move(record)); co_await recorded.wait();
      if (!credentialFailure || attempt == LIVE_CALL_MAX_UPSTREAM_ATTEMPTS) {
        auto result = co_await relayLiveCallResponse(response, account.accountId, principal);
        if (response.ok() && account.routing.probeGeneration) {
          const auto location = response.headers().get("location");
          if (location && upstream::parseLiveCallIdFromLocation(*location)) co_await codex::routing_mutations::markCodexSuccess(account.routing);
          else co_await codex::routing_mutations::releaseCodexRoutingProbe(account.routing);
        }
        co_return result;
      }
      rt::Promise<void> transitions(codex::dispatch::awaitPendingCodexProbeTransitions()); co_await transitions.wait();
      rt::Promise<rt::Response> capture(captureLiveCallAttempt(response, req.signal())); captured = co_await capture.wait();
    }
    co_return captured.value_or(http::openaiError(502, "Codex upstream request failed: upstream unreachable.", "codex_upstream_unreachable"));
  }
}
