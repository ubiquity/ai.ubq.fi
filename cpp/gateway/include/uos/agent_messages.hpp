#pragma once
// Port of src/agent-messages.ts.
#include "uos/auth/index.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace uos::agent_messages {
  // `AgentMessagesKv`: the store's `set` and `list` (the only members the handlers call).
  using AgentMessagesKv = rt::Kv;

  // `AgentMessagesDeps`: an empty function is undefined; `kv` std::nullopt is undefined (getKv()
  // decides) and a null pointer is null.
  struct AgentMessagesDeps {
    std::function<rt::Task<auth::index::AuthenticateClientResult>(rt::Request req)> authenticateClient{};
    std::optional<std::shared_ptr<AgentMessagesKv>> kv{};
    std::function<double()> now{};
    std::function<std::string()> uuid{};
  };

  [[nodiscard]] rt::Task<rt::Response> handleAgentMessagesPost(rt::Request req);
  [[nodiscard]] rt::Task<rt::Response> handleAgentMessagesPost(rt::Request req, AgentMessagesDeps deps);

  [[nodiscard]] rt::Task<rt::Response> handleAgentMessagesList(rt::Request req);
  [[nodiscard]] rt::Task<rt::Response> handleAgentMessagesList(rt::Request req, AgentMessagesDeps deps);
}
