#pragma once
// src/codex/supervisor-transport.ts: the read-only JSON-RPC transport for a Codex
// app-server control socket.
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/websocket.hpp"

#include <functional>
#include <optional>
#include <string>

namespace uos::codex::supervisor_transport {
  // type SupervisorMethod = (typeof READ_ONLY_METHODS)[number]: a method name, checked
  // against the allowlist at run time ("thread/read", ...).
  using SupervisorMethod = std::string;

  // The `unknown` result of a call: the reply's `result` member, std::nullopt when the
  // reply has none (undefined).
  using SupervisorCallResult = std::optional<json::value>;

  /** One established control-socket connection. Never reused across sampling rounds. */
  // `call(method, params = {}, signal?)` and `close()`. Bind the Task a call returns to a
  // named local before awaiting it (GCC 12, docs/rt/async.md section 3).
  struct SupervisorConnection {
    std::function<rt::Task<SupervisorCallResult>(SupervisorMethod method, json::object params, std::optional<rt::AbortSignal> signal)> call;
    std::function<rt::Task<void>()> close;
  };

  /**
   * Test-only seam: replaces the lazily loaded constructor so transport cleanup
   * can be exercised without a live socket. Passing null restores lazy loading.
   */
  // An empty function is null (`typeof candidate !== "function"`).
  void setSupervisorWebSocketConstructorForTest(rt::ws::WebSocketConstructor candidate);

  /**
   * Opens one connection to a Codex app-server control socket.
   *
   * Every call carries its own deadline, and `close` performs a clean websocket
   * close before falling back to termination so the daemon never keeps a subscriber.
   */
  rt::Task<SupervisorConnection> openSupervisorConnection(std::string socketPath);
  rt::Task<SupervisorConnection> openSupervisorConnection(std::string socketPath, std::optional<rt::AbortSignal> signal);
}
