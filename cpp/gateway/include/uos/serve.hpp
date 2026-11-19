#pragma once
// Port of serve.ts. Linking this module starts no migration, listener, or maintenance.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/runtime/http.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uos::serve {
  rt::Task<void> shutdownOptionalTelemetry();

  std::function<rt::Promise<void>()> startMacMaintenance(std::shared_ptr<rt::Kv> kv);

  rt::Task<void> configureLoopbackServeRuntime(std::vector<std::string> args);

  // Start the reset-settings migration while the listener is starting.
  rt::Promise<void> initializeModule();

  namespace detail {
    rt::Task<rt::Response> applicationFetch(rt::Request request, runtime::http::ServeHandlerInfo info);
    rt::Task<runtime::http_upgrade::UpgradeResult> applicationUpgrade(rt::Request request, runtime::http_upgrade::UpgradeInfo info);
    void applicationOnListen(const runtime::http::NetAddr& address);
  }
  inline const runtime::http::ServeDefaultExport application{
    .fetch = detail::applicationFetch,
    .onListen = detail::applicationOnListen,
    .upgrade = detail::applicationUpgrade,
  };

  struct ServeStartupOptions {
    std::string hostname;
    int port;
  };

  // The verified native launcher prepares durable storage and selects the listener explicitly.
  // This entry retains the ordinary authenticated application and source shutdown lifecycle.
  rt::Task<void> importMetaMain(const ServeStartupOptions& options);
}
