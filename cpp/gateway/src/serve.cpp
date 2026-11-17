#include "uos/serve.hpp"
#include "uos/auth/local_admin.hpp"
#include "uos/auth/local_development_key.hpp"
#include "uos/cache/prompt_analytics.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/config.hpp"
#include "uos/handler/serve_handler.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/process.hpp"
#include "uos/runtime/native_kv_bootstrap.hpp"
#include "uos/storage/native_addon.hpp"

#include <optional>
#include <utility>

namespace uos::serve {
  rt::Task<void> shutdownOptionalTelemetry() {
    try {
      rt::Promise<void> pending(cache::prompt_analytics::closeOptionalPromptCacheAnalytics());
      co_await pending.wait();
    } catch (...) {}
    const auto snapshot = cache::prompt_analytics::optionalPromptCacheAnalyticsSnapshot();
    if (!snapshot) co_return;
    try {
      const json::object event{
        {"enqueued", snapshot->enqueued}, {"delivered", snapshot->delivered}, {"failed", snapshot->failed},
        {"dropped_by_entries", snapshot->dropped_by_entries}, {"dropped_by_bytes", snapshot->dropped_by_bytes},
        {"dropped_by_age", snapshot->dropped_by_age}, {"dropped_after_closed", snapshot->dropped_after_closed},
        {"drain_timeouts", snapshot->drain_timeouts}, {"retained_entries", snapshot->retained_entries},
        {"retained_bytes", snapshot->retained_bytes}, {"queued_entries", snapshot->queued_entries},
        {"writes_in_flight", snapshot->writes_in_flight}, {"last_error_class", snapshot->last_error_class ? json::value(*snapshot->last_error_class) : json::value(nullptr)},
        {"shutdown_incomplete", snapshot->shutdown_incomplete},
      };
      rt::console::info("[ai.ubq.fi] optional_telemetry_shutdown", rt::jsonStringify(event));
    } catch (...) {}
  }

  namespace {
    struct MacMaintenance {
      std::shared_ptr<rt::Kv> kv;
      bool stopped = false;
      std::optional<rt::Promise<void>> reconciliation;
      std::optional<rt::Promise<void>> pruning;
      std::optional<rt::Promise<void>> stopPromise;
    };
    rt::Task<void> reconcile(std::shared_ptr<MacMaintenance> state) {
      auto reset = rt::scopeExit([state] { state->reconciliation.reset(); });
      try {
        rt::Promise<double> pending(paid_fallback::ledger_backfill::reconcileDuePaidFallbacksV3(rt::dateNow(), state->kv));
        co_await pending.wait();
      } catch (...) {
        const auto error = std::current_exception();
        rt::console::error("[ai.ubq.fi] Mac paid fallback reconciliation failed:", rt::isError(error) ? rt::errorMessage(error) : rt::reasonToString(error));
      }
    }
    rt::Task<void> prune(std::shared_ptr<MacMaintenance> state) {
      auto reset = rt::scopeExit([state] { state->pruning.reset(); });
      try {
        cache::prompt_analytics_core::PromptCacheAnalyticsOptions options;
        options.kv = state->kv;
        rt::Promise<cache::prompt_analytics_core::PromptCacheAnalyticsPruneResult> pending(cache::prompt_analytics::prunePromptCacheAnalytics(options));
        auto result = co_await pending.wait();
        if (result.status == "unavailable") {
          const json::object event{{"status", "prune_unavailable"}};
          rt::console::warn("[ai.ubq.fi] prompt_cache_analytics", rt::jsonStringify(event));
        }
      } catch (...) {
        const json::object event{{"status", "prune_failed"}};
        rt::console::warn("[ai.ubq.fi] prompt_cache_analytics", rt::jsonStringify(event));
      }
    }
    auth::local_admin::ServeRuntimeOptions& runtimeOptions() {
      static auth::local_admin::ServeRuntimeOptions options = [] {
        const auth::local_admin::ParseServeRuntimeOptionsOptions context{config::config.isDeploy, config::config.isProductionTimeline};
        return auth::local_admin::parseServeRuntimeOptions({}, context);
      }();
      return options;
    }
  }

  std::function<rt::Promise<void>()> startMacMaintenance(std::shared_ptr<rt::Kv> kv) {
    auto state = std::make_shared<MacMaintenance>();
    state->kv = std::move(kv);
    if (!state->stopped && !state->reconciliation) state->reconciliation = rt::Promise<void>(reconcile(state));
    if (!state->stopped && !state->pruning) state->pruning = rt::Promise<void>(prune(state));
    return [state] {
      if (state->stopPromise) return *state->stopPromise;
      state->stopped = true;
      std::vector<rt::Promise<void>> tasks{state->reconciliation.value_or(rt::Promise<void>::resolve()), state->pruning.value_or(rt::Promise<void>::resolve())};
      state->stopPromise = rt::all(std::move(tasks)).then([] {});
      return *state->stopPromise;
    };
  }

  rt::Task<void> configureLoopbackServeRuntime(std::vector<std::string> args) {
    const auth::local_admin::ParseServeRuntimeOptionsOptions context{config::config.isDeploy, config::config.isProductionTimeline};
    runtimeOptions() = auth::local_admin::parseServeRuntimeOptions(args, context);
    if (!runtimeOptions().disableAdminAuth) co_return;
    try {
      auto kvPending = kv::getKv();
      auto store = co_await kvPending.wait();
      if (store) {
        rt::Promise<auth::local_development_key::LocalDevelopmentKeyStatus> pending(auth::local_development_key::ensureLocalDevelopmentApiKey(store));
        auto status = co_await pending.wait();
        if (status == "created") rt::console::log("[ai.ubq.fi] Provisioned the local development API key for loopback inference.");
        else if (status == "revoked") rt::console::warn("[ai.ubq.fi] The local development API key is revoked; local paid-provider routing stays off.");
      }
    } catch (...) {
      const auto error = std::current_exception();
      rt::console::warn("[ai.ubq.fi] Local development key provisioning failed:", rt::isError(error) ? rt::errorMessage(error) : rt::reasonToString(error));
    }
  }

  namespace {
    rt::Task<void> migrateCodexResetSettings() {
      try {
        auto kvPending = kv::getKv();
        auto store = co_await kvPending.wait();
        if (!store) co_return;
        rt::Promise<std::string> pending(codex::reset_settings::migrateLegacyCodexResetOptOut(store));
        auto status = co_await pending.wait();
        if (status == "pending") rt::console::warn("[ai.ubq.fi] Codex reset-settings migration waits for a configured account pool and an unchanged snapshot; the persisted global opt-out stays in force.");
      } catch (...) {
        const auto error = std::current_exception();
        rt::console::warn("[ai.ubq.fi] Codex reset-settings migration failed:", rt::isError(error) ? rt::errorMessage(error) : rt::reasonToString(error));
      }
    }
  }

  rt::Promise<void> initializeModule() { return rt::Promise<void>(migrateCodexResetSettings()); }

  namespace detail {
    rt::Task<rt::Response> applicationFetch(rt::Request request, runtime::http::ServeHandlerInfo info) {
      if (runtimeOptions().disableAdminAuth) auth::local_admin::configureAdminAuthPeerForRequest(info.remoteAddr, request);
      static const auto serveHandler = handler::serve_handler::createServeHandler();
      return serveHandler(request, info);
    }
    rt::Task<runtime::http_upgrade::UpgradeResult> applicationUpgrade(rt::Request request, runtime::http_upgrade::UpgradeInfo info) {
      if (runtimeOptions().disableAdminAuth) auth::local_admin::configureAdminAuthPeerForRequest(runtime::http::Addr(info.remoteAddr), request);
      static const auto serveUpgradeHandler = handler::serve_handler::createServeUpgradeHandler();
      return serveUpgradeHandler(request, info);
    }
    void applicationOnListen(const runtime::http::NetAddr& address) {
      if (runtimeOptions().disableAdminAuth) auth::local_admin::configureAdminAuthForListener(runtimeOptions(), runtime::http::Addr(address));
      const auto hostname = address.hostname.find(':') != std::string::npos ? "[" + address.hostname + "]" : address.hostname;
      rt::console::log("Listening on http://" + hostname + ":" + rt::numberToString(address.port) + "/");
      if (runtimeOptions().disableAdminAuth) rt::console::warn("[ai.ubq.fi] WARNING: admin authentication is disabled for this loopback development server.");
    }
  }

  namespace {
    rt::Task<void> serveConfigured(std::string hostname, int port) {
      (void)initializeModule();
      runtime::http::ServeHttpOptions options{.hostname = std::move(hostname), .port = static_cast<double>(port), .onListen = application.onListen, .upgrade = application.upgrade};
      const auto server = runtime::http::serveHttp(options, application.fetch);
      rt::process::Listener shutdown([server] {
        server.shutdown().catch_([](rt::Reason) { rt::console::error("[ai.ubq.fi] Development HTTP shutdown failed"); });
      });
      rt::process::once("SIGINT", shutdown);
      rt::process::once("SIGTERM", shutdown);
      {
        auto removeListeners = rt::scopeExit([shutdown] {
          rt::process::removeListener("SIGINT", shutdown);
          rt::process::removeListener("SIGTERM", shutdown);
        });
        co_await server.ready.wait();
        co_await server.finished.wait();
        rt::Promise<void> drained(shutdownOptionalTelemetry());
        co_await drained.wait();
        auto kvPending = kv::getKv();
        auto store = co_await kvPending.wait();
        if (store) store->close();
      }
      rt::process::exit(0);
    }

    rt::Task<void> servePrepared(std::string hostname, int port) {
      if (!storage::native_addon::nativeKvBackendIsPrepared()) throw rt::JsError("Native KV backend must be prepared before starting the gateway");
      const std::vector<std::string> args;
      rt::Promise<void> configured(configureLoopbackServeRuntime(args));
      co_await configured.wait();
      auto serving = serveConfigured(std::move(hostname), port);
      co_await std::move(serving);
    }
  }

  rt::Task<void> importMetaMain(const ServeStartupOptions& options) {
    return servePrepared(options.hostname, options.port);
  }

  rt::Task<void> importMetaMain() {
    auto prepared = runtime::native_kv_bootstrap::prepareNativeKvForDevelopment();
    co_await prepared.wait();
    const auto& argv = rt::process::argv();
    std::vector<std::string> args(argv.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(2, argv.size())), argv.end());
    rt::Promise<void> configured(configureLoopbackServeRuntime(args));
    co_await configured.wait();

    const std::string hostname = "127.0.0.1";
    auto serving = serveConfigured(hostname, 8000);
    co_await std::move(serving);
  }
}
