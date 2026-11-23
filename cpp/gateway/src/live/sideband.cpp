#include "uos/live/sideband.hpp"

#include "uos/codex/auth.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/http.hpp"
#include "uos/live/upstream.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/websocket.hpp"
#include "uos/runtime/websocket.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace uos::live::sideband {
  namespace {
    namespace ws = rt::ws;
    using Socket = std::shared_ptr<ws::WebSocket>;
    constexpr int SOCKET_OPEN = 1;
    constexpr int SIDEBAND_FAILURE_CLOSE_CODE = 1011;
    ws::WebSocketConstructor upstreamWebSocketConstructor;
    rt::Task<ws::WebSocketConstructor> loadUpstreamWebSocketConstructor() {
      if (upstreamWebSocketConstructor) co_return upstreamWebSocketConstructor;
      auto pending = runtime::websocket::loadWebSocketModule(); auto module = co_await pending.wait();
      if (!module.default_) throw rt::JsError("ws module did not expose a WebSocket constructor");
      upstreamWebSocketConstructor = module.default_; co_return upstreamWebSocketConstructor;
    }
    void logLiveSideband(const std::string& event, const json::object& fields) {
      try { json::object entry{{"event", event}}; rt::objectAssign(entry, fields); rt::console::info("[ai.ubq.fi] live_sideband", rt::jsonStringify(entry)); } catch (...) {}
    }
    std::optional<int> upstreamCloseCode(int code) { return code == 1000 || (code >= 3000 && code <= 4999) ? std::optional<int>(code) : std::nullopt; }
    std::optional<int> downstreamCloseCode(int code) { return code >= 1000 && code <= 4999 && code != 1005 && code != 1006 && code != 1015 ? std::optional<int>(code) : std::nullopt; }
    std::optional<std::string> frameText(const ws::Data& raw) {
      if (const auto* text = std::get_if<std::string>(&raw)) return *text;
      if (const auto* bytes = std::get_if<ws::Bytes>(&raw)) return rt::decodeUtf8(*bytes);
      return std::nullopt;
    }
    double frameBytes(const ws::Data& raw) {
      if (const auto* text = std::get_if<std::string>(&raw)) return static_cast<double>(rt::utf8ByteLength(*text));
      if (const auto* bytes = std::get_if<ws::Bytes>(&raw)) return static_cast<double>(bytes->size());
      return 0;
    }
    void closeDownstream(const Socket& socket, int code, const std::string& reason) {
      if (socket->readyState() != 0 && socket->readyState() != SOCKET_OPEN) return;
      try { if (const auto sendable = downstreamCloseCode(code)) socket->close(*sendable, rt::slice(reason, 0, 100)); else socket->close(); }
      catch (...) { try { socket->close(); } catch (...) {} }
    }
    void closeUpstream(const Socket& socket, int code, const std::string& reason) {
      if (socket->readyState() != SOCKET_OPEN) { try { socket->terminate(); } catch (...) {} return; }
      try { if (const auto sendable = upstreamCloseCode(code)) socket->close(*sendable, rt::slice(reason, 0, 100)); else socket->close(); }
      catch (...) { try { socket->terminate(); } catch (...) {} }
    }
    struct BridgeState {
      Socket downstream;
      std::string callId;
      std::string accountId;
      std::vector<std::string> preopenFrames;
      double preopenBytes = 0;
      bool settled = false;
      bool upstreamOpen = false;
      bool dialed = false;
      Socket upstream;
    };
    void finish(const std::shared_ptr<BridgeState>& state, int code, const std::string& reason, std::optional<rt::Promise<void>> beforeClose = std::nullopt) {
      if (state->settled) return;
      state->settled = true; state->preopenFrames.clear(); state->preopenBytes = 0;
      const auto close = [state, code, reason] {
        if (state->dialed) closeUpstream(state->upstream, code, reason);
        closeDownstream(state->downstream, code, reason);
        logLiveSideband("closed", {{"call_id", state->callId}, {"account_id", state->accountId}, {"code", code}});
      };
      if (beforeClose) (void)beforeClose->then(close, [close](rt::Reason) { close(); }); else close();
    }
    bool sendUpstreamFrame(const std::shared_ptr<BridgeState>& state, const std::string& text) {
      const double bufferedBytes = state->upstream->bufferedAmount();
      if (bufferedBytes + frameBytes(ws::Data(text)) > upstream::LIVE_SIDEBAND_MAX_BUFFERED_BYTES) {
        finish(state, SIDEBAND_FAILURE_CLOSE_CODE, "realtime upstream is not draining frames"); return false;
      }
      try { state->upstream->send(text); return true; }
      catch (...) { finish(state, SIDEBAND_FAILURE_CLOSE_CODE, "realtime upstream send failed"); return false; }
    }
    rt::Task<void> bridgeLiveSideband(Socket downstream, std::string callId, std::string accountId, std::string accessToken) {
      auto state = std::make_shared<BridgeState>(); state->downstream = downstream; state->callId = callId; state->accountId = accountId;
      // Attach downstream handlers before the first await.
      downstream->onmessage([state](const ws::MessageEvent& event) {
        if (state->settled) return;
        if (frameBytes(event.data) > upstream::LIVE_SIDEBAND_MAX_PAYLOAD_BYTES) { finish(state, SIDEBAND_FAILURE_CLOSE_CODE, "realtime sideband frame exceeds payload limit"); return; }
        const auto text = frameText(event.data); if (!text) return;
        const auto bytes = frameBytes(ws::Data(*text));
        if (bytes > upstream::LIVE_SIDEBAND_MAX_PAYLOAD_BYTES) { finish(state, SIDEBAND_FAILURE_CLOSE_CODE, "realtime sideband frame exceeds payload limit"); return; }
        if (!state->upstreamOpen) {
          if (static_cast<double>(state->preopenFrames.size()) >= upstream::LIVE_SIDEBAND_PREOPEN_MAX_FRAMES || state->preopenBytes + bytes > upstream::LIVE_SIDEBAND_MAX_BUFFERED_BYTES) {
            finish(state, SIDEBAND_FAILURE_CLOSE_CODE, "realtime sideband frame queue overflow"); return;
          }
          state->preopenFrames.push_back(*text); state->preopenBytes += bytes; return;
        }
        sendUpstreamFrame(state, *text);
      });
      downstream->onclose([state](const ws::CloseEvent& event) { finish(state, event.code, event.reason); });
      downstream->onerror([state](const ws::ErrorEvent&) { finish(state, SIDEBAND_FAILURE_CLOSE_CODE, "realtime sideband error"); });
      bool failed = false;
      try {
        rt::Promise<ws::WebSocketConstructor> pending(loadUpstreamWebSocketConstructor()); const auto constructor = co_await pending.wait();
        if (state->settled) co_return;
        ws::ClientOptions options;
        const auto headers = upstream::liveSidebandUpstreamHeaders(accessToken, accountId);
        for (const auto& field : headers) options.headers.emplace_back(std::string(field.key()), std::string(field.value().get_string()));
        options.perMessageDeflate = false; options.maxPayload = upstream::LIVE_SIDEBAND_MAX_PAYLOAD_BYTES; options.handshakeTimeout = upstream::LIVE_SIDEBAND_HANDSHAKE_TIMEOUT_MS;
        state->upstream = constructor(upstream::liveSidebandUrl(callId), options); state->dialed = true;
      } catch (...) { failed = true; }
      if (failed) {
        logLiveSideband("rejected", {{"call_id", callId}, {"reason", "upstream_dial_failed"}});
        closeDownstream(downstream, SIDEBAND_FAILURE_CLOSE_CODE, "realtime upstream unavailable"); co_return;
      }
      state->upstream->on("open", [state] {
        state->upstreamOpen = true; logLiveSideband("joined", {{"call_id", state->callId}, {"account_id", state->accountId}}); state->preopenBytes = 0;
        auto frames = std::move(state->preopenFrames); state->preopenFrames.clear();
        for (const auto& frame : frames) if (!sendUpstreamFrame(state, frame)) return;
      });
      state->upstream->on("message", [state](const ws::EventArgs& event) {
        if (event.isBinary) return;
        const auto text = frameText(event.data);
        if (!text || state->downstream->readyState() != SOCKET_OPEN || state->downstream->bufferedAmount() + frameBytes(ws::Data(*text)) > upstream::LIVE_SIDEBAND_MAX_BUFFERED_BYTES) {
          finish(state, SIDEBAND_FAILURE_CLOSE_CODE, "realtime sideband is not writable"); return;
        }
        state->downstream->send(*text);
      });
      state->upstream->on("close", [state](const ws::EventArgs& event) { finish(state, event.code, rt::decodeUtf8(event.reason)); });
      state->upstream->on("error", [state] { finish(state, SIDEBAND_FAILURE_CLOSE_CODE, "realtime upstream error"); });
      state->upstream->on("unexpected-response", [state](const ws::EventArgs& event) {
        if (!state->settled && event.response && event.response->statusCode == 401 && codex::auth::hasLeaderAuthReader()) {
          const rt::Promise<bool> reading(codex::auth::checkLeaderAuth());
          finish(state, SIDEBAND_FAILURE_CLOSE_CODE, "realtime upstream rejected the sideband", reading.then([](bool) {}));
          return;
        }
        if (event.response && (event.response->statusCode == 404 || event.response->statusCode == 410)) {
          rt::Promise<void> retirement(upstream::retireLiveCallMapping(state->callId));
          finish(state, SIDEBAND_FAILURE_CLOSE_CODE, "realtime upstream rejected the sideband", retirement); return;
        }
        finish(state, SIDEBAND_FAILURE_CLOSE_CODE, "realtime upstream rejected the sideband");
      });
    }
    rt::Response liveCallMissingAccountResponse() { return http::openaiError(404, "The realtime call's account is no longer configured.", "invalid_request_error"); }
    rt::Response liveCallAuthUnavailableResponse() { return http::openaiError(503, "Codex auth pool is temporarily unavailable; retry the request.", "codex_auth_missing"); }
    using LiveCallAccountToken = std::variant<std::string, rt::Response>;
    rt::Task<LiveCallAccountToken> liveCallAccountToken(std::string accountId) {
      bool poolUnavailable = false; bool found = false;
      try {
        rt::Promise<codex::auth::CodexAuthPoolEntry> pending(codex::auth::getAuthPoolEntry(true, true)); const auto entry = co_await pending.wait();
        for (const auto& candidate : entry.pool.accounts) if (candidate.account_id == accountId) found = true;
      } catch (...) { poolUnavailable = true; }
      if (poolUnavailable) co_return liveCallAuthUnavailableResponse();
      if (!found) co_return liveCallMissingAccountResponse();
      rt::Reason failure;
      try {
        rt::Promise<codex::auth::CodexAuthAccountEntry> currentPending(codex::auth_refresh::getCurrentAccountEntry(accountId, true)); auto current = co_await currentPending.wait();
        auto task = codex::auth_refresh::getValidAuth(current); rt::Promise<::uos::types::CodexAuthState> pending(std::move(task)); const auto auth = co_await pending.wait();
        co_return auth.access_token;
      } catch (...) { failure = std::current_exception(); }
      if (const auto* error = codex::auth::codexErrorOf(failure)) {
        if (error->code == "codex_auth_missing") co_return liveCallMissingAccountResponse();
        co_return http::openaiError(error->status >= 400 && error->status <= 599 ? error->status : 503, error->message, error->code);
      }
      co_return liveCallAuthUnavailableResponse();
    }
  }
  rt::Task<runtime::http_upgrade::UpgradeResult> prepareLiveSidebandUpgrade(rt::Request req, std::string callId, std::string principal) {
    rt::Promise<std::optional<upstream::LiveCallMapping>> pending(upstream::readLiveCallMapping(callId)); const auto mapping = co_await pending.wait();
    if (!mapping) {
      logLiveSideband("rejected", {{"call_id", callId}, {"reason", "unknown_call"}}); co_return http::openaiError(404, "Unknown realtime call.", "invalid_request_error");
    }
    if (!mapping->principalId || *mapping->principalId != principal) {
      logLiveSideband("rejected", {{"call_id", callId}, {"reason", mapping->principalId ? "principal_mismatch" : "principal_missing"}});
      co_return http::openaiError(403, "The realtime call was not created by this gateway principal.", "forbidden");
    }
    rt::Promise<LiveCallAccountToken> accountPending(liveCallAccountToken(mapping->accountId)); const auto account = co_await accountPending.wait();
    if (const auto* response = std::get_if<rt::Response>(&account)) {
      logLiveSideband("rejected", {{"call_id", callId}, {"reason", "account_unavailable"}}); co_return *response;
    }
    if (rt::toLowerCase(rt::trim(req.headers().get("upgrade").value_or(""))) != "websocket")
      co_return http::openaiError(426, "The realtime sideband requires a WebSocket upgrade.", "invalid_request_error");
    runtime::http_upgrade::WebSocketUpgrade upgrade;
    upgrade.maxPayloadBytes = upstream::LIVE_SIDEBAND_MAX_PAYLOAD_BYTES;
    upgrade.payloadLimitClose = runtime::http_upgrade::PayloadLimitClose{SIDEBAND_FAILURE_CLOSE_CODE, "realtime sideband frame exceeds payload limit"};
    const auto accountId = mapping->accountId; const auto accessToken = std::get<std::string>(account);
    upgrade.accept = [callId, accountId, accessToken](Socket downstream) { rt::spawn(bridgeLiveSideband(std::move(downstream), callId, accountId, accessToken)); };
    co_return upgrade;
  }
  rt::Task<rt::Response> handleLiveSideband(rt::Request req, std::string callId, std::string principal) {
    rt::Promise<runtime::http_upgrade::UpgradeResult> pending(prepareLiveSidebandUpgrade(req, callId, principal)); auto prepared = co_await pending.wait();
    if (const auto* response = std::get_if<rt::Response>(&prepared)) co_return *response;
    co_return http::openaiError(426, "The realtime sideband requires a WebSocket upgrade.", "invalid_request_error");
  }
}
