#include "uos/config.hpp"

#include "uos/release.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include <exception>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace uos::config {
  namespace {

    std::unordered_set<std::string> parseTokens(const std::optional<std::string>& raw) {
      if (!raw || raw->empty()) return {};
      static const rt::Regex SEPARATOR("[\\n,]", "g");
      std::unordered_set<std::string> tokens;
      for (const std::string& part : rt::split(*raw, SEPARATOR)) {
        std::string token = rt::trim(part);
        if (!token.empty()) tokens.insert(std::move(token));
      }
      return tokens;
    }

    std::optional<std::string> getEnv(std::string_view key) {
      try {
        return rt::env::get(key);
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }

    std::optional<std::string> firstNonEmpty(std::initializer_list<std::optional<std::string>> values) {
      for (const auto& value : values) {
        if (value && !value->empty()) return value;
      }
      return std::nullopt;
    }

    Config loadConfig() {
      std::optional<std::string> deploy = getEnv("DENO_DEPLOY");
      if (!deploy) deploy = getEnv("DENO_DEPLOYMENT_ID");
      if (!deploy) deploy = getEnv("DENO_REGION");
      const bool isDeploy = deploy.has_value() && !deploy->empty();
      const bool isProductionTimeline = getEnv("DENO_TIMELINE") == std::optional<std::string>("production");
      std::unordered_set<std::string> authTokens = parseTokens(getEnv("UOS_AI_TOKEN"));
      std::unordered_set<std::string> adminTokens = parseTokens(getEnv("DENO_DEPLOY_TOKEN"));
      const std::string allowOrigin = "*";

      const std::string codexBaseUrl = "https://chatgpt.com/backend-api/codex";
      const std::string codexAuthJsonB64 = rt::trim(getEnv("CODEX_AUTH_JSON_B64").value_or(""));

      return {
          .isDeploy = isDeploy,
          .isProductionTimeline = isProductionTimeline,
          .allowOrigin = allowOrigin,
          .authTokens = std::move(authTokens),
          .adminTokens = std::move(adminTokens),
          .codexBaseUrl = codexBaseUrl,
          .codexAuthJsonB64 = codexAuthJsonB64,
      };
    }

    std::optional<std::string> trimmed(const std::optional<std::string>& value) {
      if (!value) return std::nullopt;
      return rt::trim(*value);
    }
  }

  Config config = loadConfig();

  std::string runtimeGitSha() { return std::string(release::RELEASE_GIT_SHA); }

  std::string runtimeDeploymentId() {
    // A configured-but-blank variable must still fall through to the next
    // source, so this cannot use nullish coalescing.
    const std::optional<std::string> configured = firstNonEmpty({trimmed(getEnv("DENO_DEPLOY_BUILD_ID")), trimmed(getEnv("DENO_DEPLOYMENT_ID"))});
    if (configured) return *configured;
    if (config.isDeploy || runtimeGitSha() == "unknown") return "unknown";
    return std::string(rt::process::platform == "darwin" ? "mac" : "vps") + "-" + runtimeGitSha();
  }
}
