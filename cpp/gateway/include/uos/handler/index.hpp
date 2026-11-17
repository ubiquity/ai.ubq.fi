#pragma once
// Port of src/handler/index.ts.
#include "uos/handler/http.hpp"
#include "uos/agent_messages.hpp"
#include "uos/models/catalog.hpp"
#include "uos/rt/http.hpp"
#include "uos/runtime/http_upgrade.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace uos::handler::index {
  struct ExactRouteEntry {
    std::vector<std::string> methods;
    std::string path;
    std::function<rt::Task<rt::Response>(rt::Request req)> run;
  };

  inline const std::vector<ExactRouteEntry> UOS_ROUTES = {
    {{"GET"}, "/uos/auth", auth::index::handleV1Auth},
    {{"GET"}, "/uos/models/catalog", [](rt::Request) { return models::catalog::handlePublicModelCatalog(); }},
    {{"GET"}, "/uos/models/capabilities", [](rt::Request req) -> rt::Task<rt::Response> {
      rt::Promise<auth::index::AuthenticateClientResult> authPending(auth::index::authenticateClient(req));
      auto authResult = co_await authPending.wait();
      if (const auto* rejected = std::get_if<auth::index::AuthenticateClientResultNotOk>(&authResult)) co_return rejected->response;
      rt::Promise<rt::Response> pending(models::catalog::handleModelCapabilities());
      auto response = co_await pending.wait();
      co_return response;
    }},
    {{"GET"}, "/uos/agent-messages", [](rt::Request req) { return agent_messages::handleAgentMessagesList(req); }},
    {{"POST"}, "/uos/agent-messages", [](rt::Request req) { return agent_messages::handleAgentMessagesPost(req); }},
  };

  rt::Task<rt::Response> handler(rt::Request req, std::optional<http::RequestDeliveryInfo> delivery = std::nullopt);

  rt::Task<runtime::http_upgrade::UpgradeResult> upgradeHandler(rt::Request req, runtime::http_upgrade::UpgradeInfo info);
}
