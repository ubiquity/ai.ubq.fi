#pragma once
#include <string>
#include <unordered_set>

namespace uos::config {
  struct Config {
    bool isDeploy;
    /**
     * `DENO_TIMELINE=production`: the production VPS service, which never grants development auth bypasses:
     * `--disable-admin-auth` fails at parse time, an enabled listener or Mac bypass grants nothing, and the
     * loopback hostname fallback is off (#262).
     */
    bool isProductionTimeline;
    std::string allowOrigin;
    std::unordered_set<std::string> authTokens;
    std::unordered_set<std::string> adminTokens;
    std::string codexBaseUrl;
    std::string codexAuthJsonB64;
  };

  // Loaded from the environment when the module is initialized. Mutable like the
  // JS object, which tests change through casts.
  extern Config config;

  std::string runtimeGitSha();
  // Deploy 2 exposes the routed revision as DENO_DEPLOY_BUILD_ID. Keep the
  // Classic variable as a local/rollback fallback so older rollback artifacts
  // still identify themselves truthfully.
  std::string runtimeDeploymentId();
}
