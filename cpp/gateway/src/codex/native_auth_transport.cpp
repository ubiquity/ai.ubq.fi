#include "uos/codex/native_auth_transport.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/websocket.hpp"
#include "uos/runtime/http.hpp"
#include "uos/upstream_auth.hpp"
#include "uos/utils.hpp"

#include <exception>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace uos::codex::native_auth_transport {
  namespace ws = rt::ws;
  using utils::getString;
  using utils::isRecord;

  namespace {
    constexpr double CALL_TIMEOUT_MS = 12'000;

    // The `unknown` value a call resolves with (std::nullopt is undefined).
    using CallResult = std::optional<json::value>;

    struct Pending {
      rt::Resolve<CallResult> resolve;
      rt::Reject reject;
      rt::Timeout timer;
    };
  }

  NativeCodexAccountVerification verifyNativeCodexAccount(const json::value* account, const NativeCodexExpectedAccount& expected, const std::optional<std::string>& fileAccountId) {
    const json::value* identity = isRecord(account) && isRecord(rt::get(*account, "account")) ? rt::get(*account, "account") : nullptr;
    if (identity == nullptr) {
      const json::value* requiresOpenaiAuth = isRecord(account) ? rt::get(*account, "requiresOpenaiAuth") : nullptr;
      if (!isRecord(account) || requiresOpenaiAuth == nullptr || !requiresOpenaiAuth->is_bool() || requiresOpenaiAuth->as_bool()) {
        return {false, "Native Codex daemon did not report its ChatGPT account"};
      }
      if (!fileAccountId || *fileAccountId != expected.accountId) {
        return {false, "Native Codex daemon uses a different ChatGPT account"};
      }
      return {true, std::nullopt};
    }
    const json::value* routing = isRecord(account) && isRecord(rt::get(*account, "workspaceRouting")) ? rt::get(*account, "workspaceRouting") : nullptr;
    const json::value* type = rt::get(*identity, "type");
    const bool chatgpt = type != nullptr && type->is_string() && type->as_string() == "chatgpt";
    if (!chatgpt || getString(rt::get(*identity, "email")) != expected.email || getString(routing ? rt::get(*routing, "chatgptAccountId") : nullptr) != expected.accountId) {
      return {false, "Native Codex daemon uses a different ChatGPT account"};
    }
    return {true, std::nullopt};
  }

  NativeCodexAccountVerification verifyNativeCodexAccount(const json::value& account, const NativeCodexExpectedAccount& expected, const std::optional<std::string>& fileAccountId) {
    return verifyNativeCodexAccount(&account, expected, fileAccountId);
  }

  namespace {
    rt::Task<std::optional<std::string>> readNativeAccountId(std::string codexHome) {
      std::optional<std::string> accountId;
      try {
        const std::string text = rt::fs::promises::readFile(codexHome + "/auth.json", "utf8");
        co_await rt::microtask();
        const json::value raw = rt::jsonParse(text);
        const json::value* authMode = isRecord(raw) ? rt::get(raw, "auth_mode") : nullptr;
        const bool chatgpt = authMode != nullptr && authMode->is_string() && authMode->as_string() == "chatgpt";
        if (!isRecord(raw) || !chatgpt || !isRecord(rt::get(raw, "tokens"))) co_return std::nullopt;
        accountId = getString(rt::get(*rt::get(raw, "tokens"), "account_id"));
      } catch (...) {
        co_return std::nullopt;
      }
      co_return accountId;
    }

    // The state the TS closures of one refresh request share.
    struct RefreshState : std::enable_shared_from_this<RefreshState> {
      std::shared_ptr<ws::WebSocket> socket;
      rt::OrderedMap<double, Pending> pending;
      double serial = 0;

      void failPending() {
        for (Pending& call : pending.values()) {
          rt::clearTimeout(call.timer);
          call.reject(rt::makeReason(rt::JsError("Native Codex socket closed")));
        }
        pending.clear();
      }

      void handleMessage(const ws::Data& raw) {
        json::value value;
        try {
          const std::string text = std::holds_alternative<ws::Bytes>(raw) ? rt::decodeUtf8(std::get<ws::Bytes>(raw)) : std::get<std::string>(raw);
          value = rt::jsonParse(text);
        } catch (...) {
          return;
        }
        const json::value* id = isRecord(value) ? rt::get(value, "id") : nullptr;
        if (!isRecord(value) || id == nullptr || !rt::isNumber(*id)) return;
        const double callId = rt::toNumber(*id);
        Pending* found = pending.get(callId);
        if (!found) return;
        const Pending call = *found;
        pending.delete_(callId);
        rt::clearTimeout(call.timer);
        if (rt::get(value, "error") != nullptr) call.reject(rt::makeReason(rt::JsError("Native Codex auth request refused")));
        else {
          const json::value* result = rt::get(value, "result");
          call.resolve(result ? CallResult(*result) : std::nullopt);
        }
      }

      // `call(method: "initialize" | "account/read", params)`
      rt::Promise<CallResult> call(const std::string& method, const json::object& params) {
        serial += 1;
        const double id = serial;
        auto self = shared_from_this();
        return rt::newPromise<CallResult>([&](const rt::Resolve<CallResult>& resolve, const rt::Reject& reject) {
          const rt::Timeout timer = rt::setTimeout(
            [self, id, reject] {
              self->pending.delete_(id);
              reject(rt::makeReason(rt::JsError("Native Codex auth request timed out")));
            },
            CALL_TIMEOUT_MS);
          pending.set(id, Pending{resolve, reject, timer});
          try {
            json::object frame;
            frame["id"] = rt::makeNumber(id);
            frame["method"] = method;
            frame["params"] = params;
            socket->send(rt::jsonStringify(json::value(std::move(frame))));
          } catch (...) {
            pending.delete_(id);
            rt::clearTimeout(timer);
            reject(rt::makeReason(rt::JsError("Native Codex auth request could not be sent")));
          }
        });
      }
    };

    rt::Task<void> refreshThroughSocket(std::shared_ptr<RefreshState> state, std::string codexHome, std::string expectedEmail, std::string expectedAccountId) {
      const rt::Promise<void> opening = rt::newPromise<void>([&](const rt::Resolve<void>& resolve, const rt::Reject& reject) {
        state->socket->once("open", [resolve] { resolve(); });
        state->socket->once("error", [reject] { reject(rt::makeReason(rt::JsError("Native Codex daemon unavailable"))); });
      });
      co_await opening.wait();
      json::object clientInfo;
      clientInfo["name"] = "uos_native_auth";
      clientInfo["version"] = "1";
      json::object capabilities;
      capabilities["experimentalApi"] = true;
      json::object initializeParams;
      initializeParams["clientInfo"] = std::move(clientInfo);
      initializeParams["capabilities"] = std::move(capabilities);
      const rt::Promise<CallResult> initializing = state->call("initialize", initializeParams);
      const CallResult initialized = co_await initializing.wait();
      const json::value* initializedValue = initialized ? &*initialized : nullptr;
      const std::optional<std::string> serverHome = isRecord(initializedValue) ? getString(rt::get(*initializedValue, "codexHome")) : std::nullopt;
      bool differentHome = !serverHome || serverHome->empty();
      if (!differentHome) {
        const std::string serverRealpath = rt::fs::promises::realpath(*serverHome);
        co_await rt::microtask();
        const std::string homeRealpath = rt::fs::promises::realpath(codexHome);
        co_await rt::microtask();
        differentHome = serverRealpath != homeRealpath;
      }
      if (differentHome) throw rt::JsError("Native Codex daemon uses a different credential home");
      json::object initializedNotice;
      initializedNotice["method"] = "initialized";
      state->socket->send(rt::jsonStringify(json::value(std::move(initializedNotice))));
      json::object readParams;
      readParams["refreshToken"] = false;
      const rt::Promise<CallResult> reading = state->call("account/read", readParams);
      const CallResult account = co_await reading.wait();
      const json::value* accountValue = account ? &*account : nullptr;
      const json::value* reportedIdentity = isRecord(accountValue) && isRecord(rt::get(*accountValue, "account")) ? rt::get(*accountValue, "account") : nullptr;
      std::optional<std::string> fileAccountId;
      if (reportedIdentity == nullptr) {
        const rt::Promise<std::optional<std::string>> readingFile(readNativeAccountId(codexHome));
        fileAccountId = co_await readingFile.wait();
      }
      const NativeCodexAccountVerification verification = verifyNativeCodexAccount(accountValue, NativeCodexExpectedAccount{expectedEmail, expectedAccountId}, fileAccountId);
      if (!verification.ok) throw rt::JsError(*verification.detail);
      // Native Codex ignores the refresh outcome in this reply. The caller must
      // reread and validate the persisted generation before claiming success.
      json::object refreshParams;
      refreshParams["refreshToken"] = true;
      const rt::Promise<CallResult> refreshing = state->call("account/read", refreshParams);
      co_await refreshing.wait();
    }
  }

  rt::Task<void> requestNativeCodexRefresh(std::string codexHome, std::string expectedEmail, std::string expectedAccountId) {
    if (upstream_auth::followerMode()) throw rt::JsError("Native Codex refresh is disabled by UOS_GATEWAY_UPSTREAM_AUTH_FOLLOWER=1.");
    const rt::Promise<runtime::websocket::WebSocketModule> loading = runtime::http::loadWebSocketModule();
    const runtime::websocket::WebSocketModule module = co_await loading.wait();
    // `isRecord(module) ? module.default : null`: the module is always an object.
    const ws::WebSocketConstructor constructor = module.default_;
    if (!constructor) throw rt::JsError("Native Codex socket transport unavailable");
    ws::ClientOptions options;
    options.headers = {{"Host", "localhost"}};
    options.perMessageDeflate = false;
    options.maxPayload = 64 * 1024;
    options.handshakeTimeout = 3'000;
    auto state = std::make_shared<RefreshState>();
    state->socket = constructor("ws+unix://" + codexHome + "/app-server-control/app-server-control.sock:/", options);
    state->socket->on("message", [state](const ws::EventArgs& event) { state->handleMessage(event.data); });
    const ws::Listener failPending([state] { state->failPending(); });
    state->socket->on("error", failPending);
    state->socket->on("close", failPending);
    rt::Reason failure;
    try {
      auto refreshing = refreshThroughSocket(state, codexHome, expectedEmail, expectedAccountId);
      co_await std::move(refreshing);
    } catch (...) {
      failure = std::current_exception();
    }
    state->failPending();
    state->socket->terminate();
    if (failure) std::rethrow_exception(failure);
  }
}
